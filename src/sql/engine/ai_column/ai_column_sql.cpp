/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/ai_column/ai_column_sql.h"
#include "sql/engine/ai_column/ai_column_internal_write_guard.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/ob_sql.h"
#include "sql/ob_sql_context.h"
#include "sql/ob_result_set.h"
#include "share/ai_column/ai_column_state.h"
#include "share/ai_column/ai_column_definition.h"
#include "share/schema/ob_table_schema.h"
#include <cstdio>

namespace oceanbase::sql {
using namespace common;
namespace {
class BoundAiColumnValues
{
public:
  explicit BoundAiColumnValues(ObSQLSessionInfo &session) : session_(session), allocator_("AiColBind") {}
  ~BoundAiColumnValues() { int ignored = close(); UNUSED(ignored); }
  int bind(const ObIArray<ObObj> &values)
  {
    int ret = OB_SUCCESS;
    if (values.count() > AiColumnSql::MAX_PARAMETERS) { return OB_SIZE_OVERFLOW; }
    for (int64_t i = 0; OB_SUCC(ret) && i < values.count(); ++i) {
      std::snprintf(names_[i], sizeof(names_[i]), "%s%ld", AiColumnSql::PARAMETER_PREFIX, i);
      const ObString name = ObString::make_string(names_[i]); ObSessionVariable previous;
      int found = session_.get_user_variable(name, previous);
      if (found != OB_SUCCESS && found != OB_ERR_USER_VARIABLE_UNKNOWN) { ret = found; }
      else if (found == OB_SUCCESS) {
        saved_[i].meta_ = previous.meta_;
        if (OB_FAIL(ob_write_obj(allocator_, previous.value_, saved_[i].value_))) {}
        else { existed_[i] = true; }
      }
      if (OB_SUCC(ret)) {
        count_ = i + 1; ObSessionVariable value;
        value.meta_ = values.at(i).get_meta(); value.value_ = values.at(i);
        if (!share::AiColumnOutbox::durable_value(value.value_)) { ret = OB_INVALID_ARGUMENT; }
        else { ret = session_.ObBasicSessionInfo::replace_user_variable(name, value, false); }
      }
    }
    return ret;
  }
  int close()
  {
    int ret = OB_SUCCESS;
    for (int64_t i = count_ - 1; i >= 0; --i) {
      const ObString name = ObString::make_string(names_[i]);
      int tmp = existed_[i] ? session_.ObBasicSessionInfo::replace_user_variable(name, saved_[i], false)
          : session_.remove_user_variable(name);
      if (tmp == OB_ERR_USER_VARIABLE_UNKNOWN && !existed_[i]) { tmp = OB_SUCCESS; }
      if (OB_SUCC(ret) && tmp != OB_SUCCESS) { ret = tmp; }
    }
    count_ = 0; return ret;
  }
private:
  ObSQLSessionInfo &session_; ObArenaAllocator allocator_; int64_t count_ = 0;
  char names_[AiColumnSql::MAX_PARAMETERS][40] = {};
  ObSessionVariable saved_[AiColumnSql::MAX_PARAMETERS];
  bool existed_[AiColumnSql::MAX_PARAMETERS] = {};
};
class AiColumnQueryExecutor : public sqlclient::ObIExecutor
{
public:
  AiColumnQueryExecutor(const ObString &query, ObIAllocator &allocator,
      ObIArray<AiColumnSqlRow> *rows, int64_t max_rows)
      : query_(query), allocator_(allocator), rows_(rows), max_rows_(max_rows) {}
  int execute(ObSql &engine, ObSqlCtx &ctx, ObResultSet &result) override
  {
    int ret = OB_SUCCESS; ObString query;
    if (query_.empty() || query_.length() > share::AiColumnOutbox::MAX_BYTES) { ret = OB_INVALID_ARGUMENT; }
    else if (OB_FAIL(ob_write_string(result.get_mem_pool(), query_, query))) {}
    else {
      result.get_session().store_query_string(query);
      ret = engine.stmt_query(query, ctx, result);
    }
    return ret;
  }
  int process_result(ObResultSet &result) override
  {
    int ret = OB_SUCCESS; affected_rows_ = result.get_affected_rows();
    if (rows_ == nullptr) { return result.is_with_rows() ? OB_ERR_UNEXPECTED : OB_SUCCESS; }
    const ObNewRow *row = nullptr; int64_t bytes = 0;
    while (OB_SUCC(ret) && OB_SUCC(ret = result.get_next_row(row))) {
      if (row == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (rows_->count() >= max_rows_) { ret = OB_SIZE_OVERFLOW; }
      else {
        AiColumnSqlRow copy;
        const int64_t count = row->projector_ == nullptr ? row->count_ : row->projector_size_;
        if (count < 0 || count > share::AiColumnOutbox::MAX_ROWKEY + share::AiColumnOutbox::MAX_COLUMNS + 2) {
          ret = OB_SIZE_OVERFLOW;
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          int64_t index = row->projector_ == nullptr ? i : row->projector_[i];
          ObObj value, durable;
          if (index < 0 || index >= row->count_) { ret = OB_ERR_UNEXPECTED; }
          else {
            const ObObj &source = row->cells_[index]; value = source;
            if (!source.is_null() && (ob_is_string_type(source.get_type()) || source.is_json())) {
              ObString data;
              if (OB_FAIL(ObTextStringHelper::read_real_string_data(result.get_exec_context(), &allocator_, source, data))) {}
              else if (data.length() > share::AiColumnOutbox::MAX_BYTES) { ret = OB_SIZE_OVERFLOW; }
              else {
                value.set_string(source.get_type(), data); value.set_collation_type(source.get_collation_type());
                value.set_collation_level(source.get_collation_level()); value.set_inrow();
              }
            }
          }
          if (OB_SUCC(ret) && (!share::AiColumnOutbox::durable_value(value)
              || (bytes += value.get_serialize_size()) > max_rows_ * share::AiColumnOutbox::MAX_BYTES * 2)) { ret = OB_SIZE_OVERFLOW; }
          if (OB_SUCC(ret)) { ret = ob_write_obj(allocator_, value, durable); }
          if (OB_SUCC(ret)) { ret = copy.values_.push_back(durable); }
        }
        if (OB_SUCC(ret)) { ret = rows_->push_back(copy); }
      }
    }
    if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    return ret;
  }
  int64_t affected_rows_ = 0;
private:
  ObString query_; ObIAllocator &allocator_; ObIArray<AiColumnSqlRow> *rows_; int64_t max_rows_;
};
}
int AiColumnSql::read(sqlclient::ObISQLConnection &connection, ObSQLSessionInfo &session,
    const ObString &query, const ObIArray<ObObj> &parameters, ObIAllocator &allocator,
    ObIArray<AiColumnSqlRow> &rows, int64_t max_rows)
{
  int ret = OB_SUCCESS; rows.reuse();
  if (max_rows <= 0 || max_rows > 64) { return OB_INVALID_ARGUMENT; }
  AiColumnInternalWriteGuard permission(session); BoundAiColumnValues bound(session);
  if (OB_FAIL(bound.bind(parameters))) {}
  else {
    AiColumnQueryExecutor executor(query, allocator, &rows, max_rows);
    ret = connection.execute(executor);
  }
  const int cleanup = bound.close(); if (OB_SUCC(ret)) { ret = cleanup; }
  if (OB_FAIL(ret)) { rows.reuse(); }
  return ret;
}
int AiColumnSql::write(sqlclient::ObISQLConnection &connection, ObSQLSessionInfo &session,
    const ObString &query, const ObIArray<ObObj> &parameters, int64_t &affected_rows, bool internal_write)
{
  int ret = OB_SUCCESS; affected_rows = 0; ObArenaAllocator allocator("AiColWrite");
  AiColumnInternalWriteGuard permission(session, internal_write); BoundAiColumnValues bound(session);
  if (OB_FAIL(bound.bind(parameters))) {}
  else {
    AiColumnQueryExecutor executor(query, allocator, nullptr, 0);
    if (OB_FAIL(connection.execute(executor))) {}
    else { affected_rows = executor.affected_rows_; }
  }
  const int cleanup = bound.close(); if (OB_SUCC(ret)) { ret = cleanup; }
  return ret;
}
namespace {
int schema_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name[i] == '\0') { ret = OB_INVALID_ARGUMENT; }
    else if (name[i] == '`') { ret = sql.append("``"); }
    else { ret = sql.append(name.ptr()+i, 1); }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); } return ret;
}
}
int AiColumnSql::validate_schema_change(const share::schema::ObTableSchema &before,
    const share::schema::ObTableSchema &after)
{
  int ret = OB_SUCCESS; int64_t count = 0; ObArenaAllocator allocator("AiColDDL");
  for (auto it = before.column_begin(); OB_SUCC(ret) && it != before.column_end(); ++it) {
    const auto *current = after.get_column_schema((*it)->get_column_id());
    if ((*it)->is_ai_column() && current != nullptr && !current->is_ai_column()) { ret = OB_NOT_SUPPORTED; }
  }
  for (auto it = after.column_begin(); OB_SUCC(ret) && it != after.column_end(); ++it) {
    if (!(*it)->is_ai_column()) { continue; }
    share::AiColumnDefinition definition;
    if (++count > share::AiColumnOutbox::MAX_COLUMNS) { ret = OB_SIZE_OVERFLOW; }
    else if (OB_FAIL(definition.decode(allocator, (*it)->get_ai_column_info()))) {}
    for (int64_t i = 0; OB_SUCC(ret) && i < definition.dependencies_.count(); ++i) {
      const uint64_t id = definition.dependencies_.at(i);
      const auto *source = after.get_column_schema(id); const auto *old = before.get_column_schema(id);
      if (source == nullptr || source->is_ai_column() || source->is_hidden() || source->is_generated_column()) {
        ret = OB_INVALID_ARGUMENT;
      } else if (old != nullptr && old->get_column_name_str() != source->get_column_name_str()) { ret = OB_NOT_SUPPORTED; }
    }
  }
  return ret;
}
int AiColumnSql::append_schema_change(const share::schema::ObColumnSchemaV2 &column,
    ObIArray<AiColumnSchemaChange> &changes)
{
  int ret = OB_SUCCESS; ObArenaAllocator allocator("AiColDDL"); share::AiColumnDefinition definition;
  AiColumnSchemaChange change; change.column_id_ = column.get_column_id();
  if (column.is_ai_column()) {
    if (OB_FAIL(definition.decode(allocator, column.get_ai_column_info()))) { return ret; }
    if (column.get_schema_version() <= 0) { return OB_INVALID_DATA; }
    change.definition_version_ = column.get_schema_version(); change.max_retries_ = definition.max_retries_;
  }
  return changes.push_back(change);
}
int AiColumnSql::fence_schema(sqlclient::ObISQLConnection &connection, ObSQLSessionInfo &session,
    const share::schema::ObTableSchema &before, const ObString &database,
    const ObIArray<AiColumnSchemaChange> &changes)
{
  int ret = OB_SUCCESS; const share::schema::ObColumnSchemaV2 *outbox = nullptr;
  for (auto it = before.column_begin(); it != before.column_end(); ++it) {
    if ((*it)->is_ai_column_outbox()) { outbox = *it; }
  }
  if (outbox == nullptr || changes.empty()) { return OB_SUCCESS; }
  if (changes.count() > share::AiColumnOutbox::MAX_COLUMNS * 2) { return OB_SIZE_OVERFLOW; }
  ObSqlString sql; bool any = false;
  if (OB_FAIL(sql.append("UPDATE "))) {}
  else if (OB_FAIL(schema_identifier(sql, database))) {}
  else if (OB_FAIL(sql.append("."))) {}
  else if (OB_FAIL(schema_identifier(sql, before.get_table_name_str()))) {}
  else { ret = sql.append(" SET "); }
  for (int64_t i = 0; OB_SUCC(ret) && i < changes.count(); ++i) {
    const auto &change = changes.at(i); const auto *column = before.get_column_schema(change.column_id_);
    if (change.definition_version_ == 0 || column == nullptr) { continue; }
    if (any && OB_FAIL(sql.append(","))) {}
    else if (OB_FAIL(schema_identifier(sql, column->get_column_name_str()))) {}
    else if (OB_FAIL(sql.append("=NULL"))) {}
    else { any = true; }
  }
  if (OB_SUCC(ret) && any) { ret = sql.append(","); }
  if (OB_SUCC(ret)) { ret = schema_identifier(sql, outbox->get_column_name_str()); }
  if (OB_SUCC(ret)) { ret = sql.append("=__ai_column_capture("); }
  if (OB_SUCC(ret)) { ret = schema_identifier(sql, outbox->get_column_name_str()); }
  if (OB_SUCC(ret)) { ret = sql.append_fmt(",2,%ld", before.get_rowkey_info().get_size()); }
  for (int64_t i = 0; OB_SUCC(ret) && i < before.get_rowkey_info().get_size(); ++i) {
    uint64_t id = 0; const share::schema::ObColumnSchemaV2 *column = nullptr;
    if (OB_FAIL(before.get_rowkey_info().get_column_id(i, id))) {}
    else if ((column = before.get_column_schema(id)) == nullptr) { ret = OB_INVALID_DATA; }
    else if (OB_FAIL(sql.append(","))) {}
    else { ret = schema_identifier(sql, column->get_column_name_str()); }
  }
  if (OB_SUCC(ret)) { ret = sql.append_fmt(",%ld", changes.count()); }
  // Remove dropped slots before adding definitions, so a valid final schema
  // cannot overflow transiently when replacing columns at the 16-column cap.
  for (int pass = 0; OB_SUCC(ret) && pass < 2; ++pass) {
    for (int64_t i = 0; OB_SUCC(ret) && i < changes.count(); ++i) {
      const auto &change = changes.at(i);
      if ((change.definition_version_ == 0) == (pass == 0)) {
        ret = sql.append_fmt(",%lu,%ld,%ld,0", change.column_id_, change.definition_version_, change.max_retries_);
      }
    }
  }
  if (OB_SUCC(ret)) { ret = sql.append(")"); }
  ObSEArray<ObObj, 1> parameters; int64_t affected = 0;
  if (OB_SUCC(ret)) { ret = write(connection, session, sql.string(), parameters, affected); }
  return ret;
}

}
