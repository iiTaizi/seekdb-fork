/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_del_upd_resolver.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ai_column/ai_column_capture_expr.h"
#include "share/ai_column/ai_column_definition.h"
#include "share/ai_column/ai_column_state.h"

namespace oceanbase::sql {
using namespace common;
using namespace share;
using namespace share::schema;
namespace {
class CaptureSqlModeGuard
{
public:
  CaptureSqlModeGuard(ObSQLSessionInfo &session, ObSQLMode mode)
      : session_(session), saved_(session.get_sql_mode()) { session_.set_sql_mode(mode); }
  ~CaptureSqlModeGuard() { session_.set_sql_mode(saved_); }
private:
  ObSQLSessionInfo &session_; ObSQLMode saved_;
};
int append_capture_integer(ObRawExprFactory &factory, ObSysFunRawExpr &capture, int64_t value)
{
  int ret = OB_SUCCESS; ObConstRawExpr *constant = nullptr;
  if (OB_FAIL(ObRawExprUtils::build_const_int_expr(factory, ObIntType, value, constant))) {}
  else { ret = capture.add_param_expr(constant); }
  return ret;
}
ObRawExpr *inserted_value(ObInsertTableInfo &info, uint64_t column)
{
  ObRawExpr *value = nullptr;
  for (int64_t i = 0; value == nullptr && i < info.column_exprs_.count(); ++i) {
    if (info.column_exprs_.at(i) != nullptr && info.column_exprs_.at(i)->get_column_id() == column
        && i < info.column_conv_exprs_.count()) { value = info.column_conv_exprs_.at(i); }
  }
  return value;
}
int append_insert_task(ObRawExprFactory &factory, ObIAllocator &allocator,
    ObSQLSessionInfo &session, ObSchemaChecker &checker, const ObTableSchema &table,
    const ObColumnSchemaV2 &column, ObInsertTableInfo &info, ObSysFunRawExpr &capture)
{
  int ret = OB_SUCCESS; AiColumnDefinition definition;
  if (OB_FAIL(definition.decode(allocator, column.get_ai_column_info()))) { return ret; }
  CaptureSqlModeGuard mode_guard(session, definition.sql_mode_);
  const ParseNode *node = nullptr; ObRawExpr *root = nullptr; ObSEArray<ObQualifiedName, 8> names;
  if (OB_FAIL(ObRawExprUtils::parse_expr_node_from_str(definition.expression_, session.get_charsets4parser(), allocator, node,
      definition.sql_mode_))) {}
  else if (node == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(factory, session, *node, root, names,
      nullptr, nullptr, &checker, ObResolverUtils::DISABLE_CHECK))) {}
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    const ObQualifiedName &name = names.at(i); const ObColumnSchemaV2 *source = table.get_column_schema(name.col_name_);
    ObRawExpr *value = source == nullptr ? nullptr : inserted_value(info, source->get_column_id());
    if (source == nullptr || value == nullptr || name.ref_expr_ == nullptr
        || !name.database_name_.empty() || !name.tbl_name_.empty()
        || source->is_ai_column() || source->is_generated_column()) { ret = OB_ERR_UNEXPECTED; }
    else { ret = ObRawExprUtils::replace_ref_column(root, name.ref_expr_, value); }
  }
  if (OB_SUCC(ret) && (root == nullptr || (root->get_expr_type() != T_FUN_SYS_AI_COMPLETE
      && root->get_expr_type() != T_FUN_SYS_AI_EMBED))) { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret)) { ret = root->formalize(&session); }
  if (OB_SUCC(ret)) { ret = append_capture_integer(factory, capture, column.get_column_id()); }
  if (OB_SUCC(ret)) { ret = append_capture_integer(factory, capture, column.get_schema_version()); }
  if (OB_SUCC(ret)) { ret = append_capture_integer(factory, capture, definition.max_retries_); }
  if (OB_SUCC(ret)) { ret = append_capture_integer(factory, capture, root->get_param_count()); }
  for (int64_t i = 0; OB_SUCC(ret) && i < root->get_param_count(); ++i) {
    ret = capture.add_param_expr(root->get_param_expr(i));
  }
  return ret;
}
int create_capture(ObRawExprFactory &factory, ObIAllocator &allocator, ObSQLSessionInfo &session,
    ObSchemaChecker &checker, const ObTableSchema &table, ObInsertTableInfo &bindings,
    const ObIArray<const ObColumnSchemaV2 *> &columns, ObRawExpr *old_state, bool rotate,
    ObRawExpr *&result)
{
  int ret = OB_SUCCESS; ObSysFunRawExpr *capture = nullptr;
  if (old_state == nullptr || columns.empty()) { return OB_INVALID_ARGUMENT; }
  if (OB_FAIL(factory.create_raw_expr(T_FUN_SYS_AI_COLUMN_CAPTURE, capture))) {}
  else if (OB_FAIL(capture->init_param_exprs(4 + table.get_rowkey_info().get_size() + columns.count() * 7))) {}
  else if (OB_FAIL(capture->add_param_expr(old_state))) {}
  else {
    capture->set_func_name(ObString::make_string("__ai_column_capture"));
    capture->set_ai_column_capture_marker();
    ret = append_capture_integer(factory, *capture, rotate ? 1 : 0);
  }
  if (OB_SUCC(ret)) { ret = append_capture_integer(factory, *capture, table.get_rowkey_info().get_size()); }
  for (int64_t i = 0; OB_SUCC(ret) && i < table.get_rowkey_info().get_size(); ++i) {
    uint64_t id = 0; ObRawExpr *value = nullptr;
    if (OB_FAIL(table.get_rowkey_info().get_column_id(i, id))) {}
    else if ((value = inserted_value(bindings, id)) == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else { ret = capture->add_param_expr(value); }
  }
  if (OB_SUCC(ret)) { ret = append_capture_integer(factory, *capture, columns.count()); }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    ret = append_insert_task(factory, allocator, session, checker, table, *columns.at(i), bindings, *capture);
  }
  if (OB_SUCC(ret)) { ret = capture->formalize(&session); }
  if (OB_SUCC(ret)) { result = capture; }
  return ret;
}
}
int ObDelUpdResolver::build_ai_column_insert_capture(ObInsertTableInfo &info)
{
  int ret = OB_SUCCESS; const ObTableSchema *table = nullptr; int64_t count = 0, outbox = -1;
  if (session_info_ != nullptr && session_info_->is_ai_column_internal_write()) { return OB_SUCCESS; }
  for (int64_t i = 0; i < info.values_desc_.count(); ++i) {
    if (info.values_desc_.at(i) != nullptr
        && (info.values_desc_.at(i)->get_column_flags() & AI_COLUMN_OUTBOX_FLAG) != 0) {
      return OB_NOT_SUPPORTED;
    }
  }
  if (schema_checker_ == nullptr || session_info_ == nullptr || params_.expr_factory_ == nullptr
      || params_.allocator_ == nullptr) { ret = OB_ERR_UNEXPECTED; }
  else if (OB_FAIL(schema_checker_->get_table_schema(info.ref_table_id_, table))) {}
  else if (table == nullptr) { ret = OB_ERR_UNEXPECTED; }
  if (OB_FAIL(ret)) { return ret; }
  for (auto it = table->column_begin(); it != table->column_end(); ++it) {
    if ((*it)->is_ai_column()) { ++count; }
    if ((*it)->is_ai_column_outbox()) {
      for (int64_t i = 0; i < info.column_exprs_.count(); ++i) {
        if (info.column_exprs_.at(i)->get_column_id() == (*it)->get_column_id()) { outbox = i; }
      }
    }
  }
  if (count == 0) { return OB_SUCCESS; }
  if (count > AiColumnOutbox::MAX_COLUMNS || outbox < 0 || outbox >= info.column_conv_exprs_.count()) {
    return OB_ERR_UNEXPECTED;
  }
  ObRawExprFactory &factory = *params_.expr_factory_; ObRawExpr *capture = nullptr; ObRawExpr *empty = nullptr;
  ObSEArray<const ObColumnSchemaV2 *, 4> columns;
  for (auto it = table->column_begin(); OB_SUCC(ret) && it != table->column_end(); ++it) {
    if ((*it)->is_ai_column()) { ret = columns.push_back(*it); }
  }
  if (OB_SUCC(ret)) { ret = ObRawExprUtils::build_null_expr(factory, empty); }
  if (OB_SUCC(ret)) { ret = create_capture(factory, *params_.allocator_, *session_info_, *schema_checker_,
      *table, info, columns, empty, true, capture); }
  if (OB_SUCC(ret)) { info.column_conv_exprs_.at(outbox) = capture; }
  return ret;
}
int ObDelUpdResolver::build_ai_column_update_capture(ObTableAssignment &assignments)
{
  int ret = OB_SUCCESS;
  if (session_info_ != nullptr && session_info_->is_ai_column_internal_write()) { return OB_SUCCESS; }
  if (get_stmt() == nullptr || schema_checker_ == nullptr || session_info_ == nullptr
      || params_.expr_factory_ == nullptr || params_.allocator_ == nullptr) { return OB_ERR_UNEXPECTED; }
  const TableItem *item = get_stmt()->get_table_item_by_id(assignments.table_id_);
  const ObTableSchema *table = nullptr;
  if (item == nullptr) { return OB_ERR_UNEXPECTED; }
  if (OB_FAIL(schema_checker_->get_table_schema(item->get_base_table_item().ref_id_, table))) { return ret; }
  if (table == nullptr) { return OB_ERR_UNEXPECTED; }
  const ObColumnSchemaV2 *state_column = nullptr;
  for (auto it = table->column_begin(); it != table->column_end(); ++it) {
    if ((*it)->is_ai_column_outbox()) { state_column = *it; }
  }
  if (state_column == nullptr) { return OB_SUCCESS; }
  bool rotate = false;
  for (int64_t i = 0; i < assignments.assignments_.count(); ++i) {
    const ObColumnSchemaV2 *column = table->get_column_schema(assignments.assignments_.at(i).column_expr_->get_column_id());
    if (column != nullptr && column->is_rowkey_column()) { rotate = true; }
  }
  ObSEArray<const ObColumnSchemaV2 *, 4> columns;
  for (auto it = table->column_begin(); OB_SUCC(ret) && it != table->column_end(); ++it) {
    if ((*it)->is_ai_column()) {
      AiColumnDefinition definition; bool affected = rotate;
      if (OB_FAIL(definition.decode(*params_.allocator_, (*it)->get_ai_column_info()))) {}
      for (int64_t i = 0; OB_SUCC(ret) && !affected && i < assignments.assignments_.count(); ++i) {
        uint64_t id = assignments.assignments_.at(i).column_expr_->get_column_id();
        affected = id == (*it)->get_column_id();
        for (int64_t j = 0; !affected && j < definition.dependencies_.count(); ++j) {
          affected = id == definition.dependencies_.at(j);
        }
      }
      if (OB_SUCC(ret) && affected) { ret = columns.push_back(*it); }
    }
  }
  if (OB_FAIL(ret) || columns.empty()) { return ret; }
  ObInsertTableInfo bindings;
  bindings.table_id_ = item->table_id_; bindings.ref_table_id_ = table->get_table_id();
  if (OB_FAIL(add_all_rowkey_columns_to_stmt(*item, bindings.column_exprs_))) {}
  else if (OB_FAIL(add_all_columns_to_stmt(*item, bindings.column_exprs_))) {}
  ObColumnRefRawExpr *old_state = nullptr;
  for (int64_t i = 0; OB_SUCC(ret) && i < bindings.column_exprs_.count(); ++i) {
    ObColumnRefRawExpr *column = bindings.column_exprs_.at(i); ObRawExpr *value = column;
    if (column->get_column_id() == state_column->get_column_id()) { old_state = column; }
    for (int64_t j = 0; j < assignments.assignments_.count(); ++j) {
      if (assignments.assignments_.at(j).column_expr_ == column) { value = assignments.assignments_.at(j).expr_; }
    }
    ret = bindings.column_conv_exprs_.push_back(value);
  }
  ObRawExpr *capture = nullptr;
  if (OB_SUCC(ret)) { ret = create_capture(*params_.expr_factory_, *params_.allocator_, *session_info_,
      *schema_checker_, *table, bindings, columns, old_state, rotate, capture); }
  // These assignments enter the existing DML/index path; do not expand the
  // already-resolved new-value expressions a second time.
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    ObColumnRefRawExpr *target = nullptr;
    for (int64_t j = 0; target == nullptr && j < bindings.column_exprs_.count(); ++j) {
      if (bindings.column_exprs_.at(j)->get_column_id() == columns.at(i)->get_column_id()) { target = bindings.column_exprs_.at(j); }
    }
    ObRawExpr *empty = nullptr;
    if (target == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(ObRawExprUtils::build_null_expr(*params_.expr_factory_, empty))) {}
    else if (OB_FAIL(ObRawExprUtils::build_column_conv_expr(*params_.expr_factory_, *params_.allocator_,
        *target, empty, session_info_))) {}
    else if (OB_FAIL(empty->formalize(session_info_))) {}
    else {
      bool found = false;
      for (int64_t j = 0; !found && j < assignments.assignments_.count(); ++j) {
        if (assignments.assignments_.at(j).column_expr_ == target) { assignments.assignments_.at(j).expr_ = empty; found = true; }
      }
      if (!found) { ObAssignment assign; assign.column_expr_ = target; assign.expr_ = empty; ret = assignments.assignments_.push_back(assign); }
    }
  }
  if (OB_SUCC(ret)) {
    ObAssignment assign; assign.column_expr_ = old_state; assign.expr_ = capture;
    ret = assignments.assignments_.push_back(assign);
  }
  return ret;
}
}
