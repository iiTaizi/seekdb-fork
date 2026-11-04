/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_update_resolver.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/engine/ai_column/ai_column_internal_write_guard.h"

namespace oceanbase::sql {
using namespace common;
using namespace share::schema;
namespace {
constexpr int64_t RETRY_MAX_NODES = 4096;
int check_retry_condition(const ParseNode *node, int64_t &nodes, int64_t depth = 0)
{
  if (node == nullptr) { return OB_SUCCESS; }
  if (depth > 256 || ++nodes > RETRY_MAX_NODES) { return OB_SIZE_OVERFLOW; }
  if (node->type_ == T_COLUMN_REF && node->num_child_ == 3 && node->children_[2] != nullptr) {
    const auto *name = node->children_[2]; const ObString column(name->str_len_, name->str_value_);
    if (column.case_compare("__seekdb_ai_state") == 0 || column.case_compare(OB_HIDDEN_PK_INCREMENT_COLUMN_NAME) == 0) {
      return OB_NOT_SUPPORTED;
    }
  }
  if (node->type_ == T_FUN_SYS && node->num_child_ > 0 && node->children_[0] != nullptr) {
    const auto *name = node->children_[0]; const ObString function(name->str_len_, name->str_value_);
    if (function.case_compare("__ai_column_status") == 0 || function.case_compare("__ai_column_capture") == 0) {
      return OB_NOT_SUPPORTED;
    }
  }
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
    ret = check_retry_condition(node->children_[i], nodes, depth+1);
  }
  return ret;
}
int retry_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name[i] == '\0') { ret = OB_INVALID_ARGUMENT; }
    else if (name[i] == '`') { ret = sql.append("``"); }
    else { ret = sql.append(name.ptr()+i, 1); }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); } return ret;
}
int rewrite_retry_status(ParseNode *&node, const ObString &status_sql,
    ObSQLSessionInfo &session, ObIAllocator &allocator, int64_t &nodes, int64_t depth = 0)
{
  if (node == nullptr) { return OB_SUCCESS; }
  if (depth > 256 || ++nodes > RETRY_MAX_NODES) { return OB_SIZE_OVERFLOW; }
  if (node->type_ == T_COLUMN_REF && node->num_child_ == 3 && node->children_[2] != nullptr) {
    const auto *name = node->children_[2]; const ObString column(name->str_len_, name->str_value_);
    if (!name->is_input_quoted_ && column.case_compare("status") == 0) {
      const ParseNode *replacement = nullptr;
      const int ret = ObRawExprUtils::parse_expr_node_from_str(status_sql,
          session.get_charsets4parser(), allocator, replacement, session.get_sql_mode());
      if (ret != OB_SUCCESS) { return ret; }
      if (replacement == nullptr) { return OB_ERR_UNEXPECTED; }
      node = const_cast<ParseNode *>(replacement); return OB_SUCCESS;
    }
  }
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
    ret = rewrite_retry_status(node->children_[i], status_sql, session, allocator, nodes, depth+1);
  }
  return ret;
}
int mark_retry_status(ObRawExpr *expr, int64_t &nodes, int64_t depth = 0)
{
  if (expr == nullptr) { return OB_ERR_UNEXPECTED; }
  if (depth > 256 || ++nodes > RETRY_MAX_NODES * 4) { return OB_SIZE_OVERFLOW; }
  int ret = OB_SUCCESS;
  if (expr->get_expr_type() == T_FUN_SYS_AI_COLUMN_STATUS) { expr->set_ai_column_status_marker(); }
  if (expr->is_query_ref_expr()) {
    auto *stmt = static_cast<ObQueryRefRawExpr *>(expr)->get_ref_stmt(); ObSEArray<ObRawExpr *, 8> roots;
    if (stmt == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(stmt->get_relation_exprs(roots))) {}
    for (int64_t i = 0; OB_SUCC(ret) && i < roots.count(); ++i) { ret = mark_retry_status(roots.at(i), nodes, depth+1); }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
    ret = mark_retry_status(expr->get_param_expr(i), nodes, depth+1);
  }
  return ret;
}
}
int ObUpdateResolver::resolve_ai_column_retry_where(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS; auto *stmt = get_update_stmt(); const ObTableSchema *table = nullptr;
  if (stmt == nullptr || stmt->get_table_items().count() != 1 || schema_checker_ == nullptr
      || session_info_ == nullptr || params_.allocator_ == nullptr || parse_tree.num_child_ != 11) { return OB_INVALID_ARGUMENT; }
  const TableItem *item = stmt->get_table_items().at(0);
  const ParseNode *assignments = parse_tree.children_[UPDATE_LIST]; const ParseNode *column_ref = nullptr;
  if (item == nullptr || !item->is_basic_table() || item->is_view_table_ || assignments == nullptr
      || assignments->num_child_ != 1 || assignments->children_[0] == nullptr
      || assignments->children_[0]->num_child_ != 2) { return OB_INVALID_ARGUMENT; }
  column_ref = assignments->children_[0]->children_[0];
  if (column_ref == nullptr || column_ref->type_ != T_COLUMN_REF || column_ref->num_child_ != 3
      || column_ref->children_[2] == nullptr) { return OB_INVALID_ARGUMENT; }
  if (OB_FAIL(schema_checker_->get_table_schema(item->ref_id_, table))) { return ret; }
  if (table == nullptr || !table->is_user_table()) { return OB_NOT_SUPPORTED; }
  const auto *name = column_ref->children_[2];
  const auto *column = table->get_column_schema(ObString(name->str_len_, name->str_value_));
  if (column == nullptr || !column->is_ai_column()) { return OB_INVALID_ARGUMENT; }
  const ObColumnSchemaV2 *outbox = nullptr;
  for (auto it = table->column_begin(); it != table->column_end(); ++it) { if ((*it)->is_ai_column_outbox()) { outbox = *it; } }
  if (outbox == nullptr) { return OB_INVALID_DATA; }
  ParseNode *condition = parse_tree.children_[WHERE]; int64_t nodes = 0; ObSqlString status;
  if (OB_FAIL(check_retry_condition(condition, nodes))) { return ret; }
  if (OB_FAIL(status.append("__ai_column_status("))) {}
  else if (!item->database_name_.empty() && OB_FAIL(retry_identifier(status, item->database_name_))) {}
  else if (!item->database_name_.empty() && OB_FAIL(status.append("."))) {}
  else if (OB_FAIL(retry_identifier(status, item->table_name_))) {}
  else if (OB_FAIL(status.append("."))) {}
  else if (OB_FAIL(retry_identifier(status, outbox->get_column_name_str()))) {}
  else { ret = status.append_fmt(",%lu,%ld)", column->get_column_id(), column->get_schema_version()); }
  nodes = 0;
  if (OB_SUCC(ret)) { ret = rewrite_retry_status(condition, status.string(), *session_info_, *params_.allocator_, nodes); }
  if (OB_SUCC(ret)) {
    AiColumnInternalWriteGuard read_generated_condition(*session_info_, false);
    ret = resolve_where_clause(condition);
  }
  nodes = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < stmt->get_condition_exprs().count(); ++i) {
    ret = mark_retry_status(stmt->get_condition_exprs().at(i), nodes);
  }
  return ret;
}
}
