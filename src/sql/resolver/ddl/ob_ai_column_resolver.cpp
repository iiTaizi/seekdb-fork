/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_ddl_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/ob_sql_utils.h"
#include "share/ai_column/ai_column_definition.h"
#include "share/ai_column/ai_column_state.h"
#include "query/vector/ob_vector_index_util.h"
#include <algorithm>
#include <climits>

namespace oceanbase::sql {
using namespace common;
using namespace share;
using namespace share::schema;
namespace {
class AiColumnSqlModeGuard
{
public:
  AiColumnSqlModeGuard(ObSQLSessionInfo &session, ObSQLMode mode)
      : session_(session), saved_(session.get_sql_mode()) { session_.set_sql_mode(mode); }
  ~AiColumnSqlModeGuard() { session_.set_sql_mode(saved_); }
private:
  ObSQLSessionInfo &session_; ObSQLMode saved_;
};
int ai_column_check_argument_tree(const ObRawExpr *expr, int64_t depth = 0)
{
  if (expr == nullptr) { return OB_INVALID_ARGUMENT; }
  if (depth > 256) { return OB_SIZE_OVERFLOW; }
  const ObItemType type = expr->get_expr_type();
  if (type == T_FUN_SYS_AI_COMPLETE || type == T_FUN_SYS_AI_EMBED || type == T_FUN_SYS_AI_RERANK) {
    return OB_INVALID_ARGUMENT;
  }
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
    ret = ai_column_check_argument_tree(expr->get_param_expr(i), depth+1);
  }
  return ret;
}
}
int ObDDLResolver::resolve_ai_column_syntax(ObColumnSchemaV2 &column, const ParseNode &node)
{
  int ret = OB_SUCCESS; AiColumnDefinition definition;
  if (node.num_child_ != AI_COLUMN_DEFINITION_NUM_CHILD || node.children_[4] == nullptr
      || session_info_ == nullptr || allocator_ == nullptr) { return OB_INVALID_ARGUMENT; }
  const ParseNode *expr = node.children_[4]; const ParseNode *options = node.children_[5];
  if (expr->type_ != T_FUN_SYS || expr->num_child_ < 1 || expr->children_[0] == nullptr) { ret = OB_INVALID_ARGUMENT; }
  else {
    const ParseNode *name = expr->children_[0]; const ObString function(name->str_len_, name->str_value_);
    if (function.case_compare("AI_COMPLETE") == 0) { definition.function_ = static_cast<int64_t>(AiColumnFunction::COMPLETE); }
    else if (function.case_compare("AI_EMBED") == 0) { definition.function_ = static_cast<int64_t>(AiColumnFunction::EMBED); }
    else { ret = OB_INVALID_ARGUMENT; }
  }
  uint64_t seen = 0;
  for (int64_t i = 0; OB_SUCC(ret) && options != nullptr && i < options->num_child_; ++i) {
    const ParseNode *option = options->children_[i];
    if (option == nullptr || option->type_ != T_OP_EQ || option->num_child_ != 2
        || option->children_[0] == nullptr || option->children_[1] == nullptr) { ret = OB_INVALID_ARGUMENT; }
    else {
      const ParseNode *key = option->children_[0], *value = option->children_[1];
      if (key->type_ != T_COLUMN_REF || key->num_child_ != 3 || key->children_[0] != nullptr
          || key->children_[1] != nullptr || key->children_[2] == nullptr) { ret = OB_INVALID_ARGUMENT; }
      else {
        const ParseNode *name = key->children_[2]; const ObString key_name(name->str_len_, name->str_value_);
        int64_t slot = -1;
        if (key_name.case_compare("sync_mode") == 0) {
          slot = 0;
          if (value->type_ != T_VARCHAR || ObString(value->str_len_, value->str_value_).case_compare("async") != 0) {
            ret = OB_INVALID_ARGUMENT;
          }
        } else if (key_name.case_compare("max_retries") == 0) {
          slot = 1;
          if (value->type_ != T_INT || value->value_ < 0 || value->value_ > AiColumnOutbox::MAX_RETRIES) {
            ret = OB_INVALID_ARGUMENT;
          } else { definition.max_retries_ = value->value_; }
        } else { ret = OB_INVALID_ARGUMENT; }
        if (OB_SUCC(ret) && (seen & (1ULL << slot)) != 0) { ret = OB_INVALID_ARGUMENT; }
        if (OB_SUCC(ret)) { seen |= 1ULL << slot; }
      }
    }
  }
  if (OB_SUCC(ret)) {
    definition.sql_mode_ = session_info_->get_sql_mode();
    definition.definer_id_ = session_info_->get_priv_user_id();
    definition.expression_.assign_ptr(expr->str_value_, expr->str_len_);
    ObString metadata;
    if (!column.is_nullable() || column.is_autoincrement() || column.is_rowkey_column()
        || column.is_generated_column() || !column.get_cur_default_value().is_null()) { ret = OB_INVALID_ARGUMENT; }
    else if (OB_FAIL(definition.encode(*allocator_, metadata))) {}
    else if (OB_FAIL(column.set_ai_column_info(metadata))) {}
    else { column.add_column_flag(AI_COLUMN_FLAG); }
  }
  if (OB_FAIL(ret)) { LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI COLUMN definition or async options"); }
  return ret;
}
int ObDDLResolver::resolve_ai_column_expression(ObIAllocator &allocator, ObTableSchema &table,
    ObIArray<ObColumnSchemaV2 *> &resolved_columns, ObColumnSchemaV2 &column,
    ObSQLSessionInfo *session, ObSchemaChecker *schema_checker)
{
  int ret = OB_SUCCESS; AiColumnDefinition definition;
  if (session == nullptr || schema_checker == nullptr || !column.is_ai_column()) { return OB_INVALID_ARGUMENT; }
  if (OB_FAIL(definition.decode(allocator, column.get_ai_column_info()))) { return ret; }
  AiColumnSqlModeGuard mode_guard(*session, definition.sql_mode_);
  ObRawExprFactory factory(allocator); const ParseNode *node = nullptr; ObRawExpr *expr = nullptr;
  ObSEArray<ObQualifiedName, 8> names; definition.dependencies_.reuse();
  if (!column.is_nullable() || column.is_rowkey_column() || column.is_autoincrement() || column.is_hidden()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObRawExprUtils::parse_expr_node_from_str(definition.expression_, session->get_charsets4parser(), allocator, node,
      definition.sql_mode_))) {}
  else if (node == nullptr) { ret = OB_INVALID_ARGUMENT; }
  else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(factory, *session, *node, expr, names,
      nullptr, nullptr, schema_checker, ObResolverUtils::DISABLE_CHECK))) {}
  else if (expr == nullptr || (expr->get_expr_type() != T_FUN_SYS_AI_COMPLETE && expr->get_expr_type() != T_FUN_SYS_AI_EMBED)
      || expr->get_param_count() < 2 || expr->get_param_count() > 3) { ret = OB_INVALID_ARGUMENT; }
  for (int64_t i = 0; OB_SUCC(ret) && expr != nullptr && i < expr->get_param_count(); ++i) {
    ret = ai_column_check_argument_tree(expr->get_param_expr(i));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    ObQualifiedName &name = names.at(i); const ObColumnSchemaV2 *source = nullptr;
    if (name.is_sys_func() || !name.database_name_.empty() || !name.tbl_name_.empty()) { ret = OB_INVALID_ARGUMENT; }
    else {
      source = table.get_column_schema(name.col_name_);
      if (source == nullptr) { source = ObResolverUtils::get_column_schema_from_array(resolved_columns, name.col_name_); }
      if (name.ref_expr_ == nullptr || source == nullptr || source->is_ai_column() || source->is_hidden() || source->is_generated_column()
          || source->get_column_id() == column.get_column_id()) { ret = OB_INVALID_ARGUMENT; }
      else if (OB_FAIL(ObRawExprUtils::init_column_expr(*source, session, *name.ref_expr_))) {}
      else {
        bool found = false;
        for (int64_t j = 0; !found && j < definition.dependencies_.count(); ++j) {
          found = definition.dependencies_.at(j) == source->get_column_id();
        }
        if (!found) { ret = definition.dependencies_.push_back(source->get_column_id()); }
      }
    }
  }
  if (OB_SUCC(ret)) { ret = expr->formalize(session); }
  if (OB_SUCC(ret)) {
    const bool embed = expr->get_expr_type() == T_FUN_SYS_AI_EMBED;
    definition.function_ = static_cast<int64_t>(embed ? AiColumnFunction::EMBED : AiColumnFunction::COMPLETE);
    int64_t target_dimension = 0;
    if (column.is_collection()) {
      if (!embed) { ret = OB_INVALID_ARGUMENT; }
      else { ret = ObVectorIndexUtil::get_vector_dim_from_extend_type_info(column.get_extended_type_info(), target_dimension); }
    }
    definition.dimension_ = target_dimension;
    if (OB_SUCC(ret) && embed && expr->get_param_count() == 3) {
      const ObRawExpr *dimension = expr->get_param_expr(2); ObObj value;
      ParamStore empty_params((ObWrapperAllocator(allocator)));
      if (!dimension->is_static_const_expr()) { ret = OB_INVALID_ARGUMENT; }
      else if (OB_FAIL(ObSQLUtils::calc_const_expr(session, *dimension, value, allocator, empty_params))) {}
      else if (!value.is_integer_type() || (ob_is_unsigned_type(value.get_type())
          ? value.get_uint64() == 0 || value.get_uint64() > static_cast<uint64_t>(INT64_MAX) : value.get_int() <= 0)) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const int64_t requested = ob_is_unsigned_type(value.get_type()) ? static_cast<int64_t>(value.get_uint64()) : value.get_int();
        if (target_dimension > 0 && target_dimension != requested) { ret = OB_INVALID_ARGUMENT; }
        else { definition.dimension_ = requested; }
      }
    }
    if (OB_SUCC(ret) && definition.dependencies_.count() > 1) {
      std::sort(definition.dependencies_.get_data(), definition.dependencies_.get_data()+definition.dependencies_.count());
    }
    ObString encoded;
    if (OB_SUCC(ret)) { ret = definition.encode(allocator, encoded); }
    if (OB_SUCC(ret)) { ret = column.set_ai_column_info(encoded); }
  }
  if (OB_FAIL(ret)) { LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI COLUMN function, dependencies, type or vector dimension"); }
  return ret;
}
int ObDDLResolver::resolve_ai_columns(ObTableSchema &table, bool &has_ai_columns)
{
  int ret = OB_SUCCESS; int64_t count = 0; has_ai_columns = false;
  ObSEArray<ObColumnSchemaV2 *, 4> resolved;
  for (auto it = table.column_begin(); OB_SUCC(ret) && it != table.column_end(); ++it) {
    ObColumnSchemaV2 *column = *it;
    if (column == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (column->is_ai_column()) {
      has_ai_columns = true;
      if (++count > AiColumnOutbox::MAX_COLUMNS) { ret = OB_SIZE_OVERFLOW; }
      else { ret = resolve_ai_column_expression(*allocator_, table, resolved, *column, session_info_, schema_checker_); }
    }
  }
  return ret;
}
int ObDDLResolver::build_ai_column_outbox(ObColumnSchemaV2 &column, uint64_t table_id, uint64_t column_id)
{
  column.reset(); column.set_table_id(table_id); column.set_column_id(column_id);
  column.set_data_type(ObLongTextType); column.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObLongTextType]);
  column.set_data_length(AiColumnOutbox::MAX_BYTES); column.set_charset_type(CHARSET_BINARY);
  column.set_collation_type(CS_TYPE_BINARY); column.set_is_hidden(true); column.set_nullable(true);
  column.add_column_flag(AI_COLUMN_OUTBOX_FLAG);
  return column.set_column_name("__seekdb_ai_state");
}
}
