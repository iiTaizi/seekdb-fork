/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SERVER_OMT
#include "observer/ai_service/ai_column_worker.h"
#include "observer/ob_inner_sql_connection.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/ai_column/ai_column_state.h"
#include "share/ai_column/ai_column_definition.h"
#include "sql/engine/ai_column/ai_column_sql.h"
#include "sql/engine/ai_column/ai_column_model_evaluator.h"
#include "lib/time/ob_time_utility.h"
#include <openssl/rand.h>
#include <algorithm>

namespace oceanbase::observer {
using namespace common;
using namespace share;
using namespace share::schema;
using namespace sql;
namespace {
constexpr int64_t BATCH_ROWS = 16;
constexpr int64_t LEASE_US = 75 * 1000 * 1000;
int quote_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '\0') { ret = OB_INVALID_ARGUMENT; }
    else if (name.ptr()[i] == '`') { ret = sql.append("``"); }
    else { ret = sql.append(name.ptr()+i, 1); }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); } return ret;
}
int table_name(ObSchemaGetterGuard &schema, const ObTableSchema &table, ObSqlString &name)
{
  int ret = OB_SUCCESS; const ObDatabaseSchema *database = nullptr;
  if (OB_FAIL(schema.get_database_schema(table.get_database_id(), database))) {}
  else if (database == nullptr) { ret = OB_TABLE_NOT_EXIST; }
  else if (OB_FAIL(quote_identifier(name, database->get_database_name_str()))) {}
  else if (OB_FAIL(name.append("."))) {}
  else { ret = quote_identifier(name, table.get_table_name_str()); } return ret;
}
int parameter(ObSqlString &sql, ObIArray<ObObj> &parameters, const ObObj &value)
{
  int ret = OB_SUCCESS;
  if (parameters.count() >= AiColumnSql::MAX_PARAMETERS) { ret = OB_SIZE_OVERFLOW; }
  else if (OB_FAIL(sql.append_fmt("@%s%ld", AiColumnSql::PARAMETER_PREFIX, parameters.count()))) {}
  else { ret = parameters.push_back(value); } return ret;
}
int key_predicate(const ObTableSchema &table, const ObIArray<ObObj> &key,
    ObSqlString &sql, ObIArray<ObObj> &parameters)
{
  int ret = OB_SUCCESS;
  if (key.empty() || key.count() != table.get_rowkey_info().get_size()) { return OB_INVALID_DATA; }
  for (int64_t i = 0; OB_SUCC(ret) && i < key.count(); ++i) {
    uint64_t id = 0; const ObColumnSchemaV2 *column = nullptr;
    if (i > 0 && OB_FAIL(sql.append(" AND "))) {}
    else if (OB_FAIL(table.get_rowkey_info().get_column_id(i, id))) {}
    else if ((column = table.get_column_schema(id)) == nullptr || key.at(i).is_null()) { ret = OB_INVALID_DATA; }
    else if (OB_FAIL(quote_identifier(sql, column->get_column_name_str()))) {}
    else if (OB_FAIL(sql.append(" <=> "))) {}
    else { ret = parameter(sql, parameters, key.at(i)); }
  }
  return ret;
}
int state_predicate(const ObColumnSchemaV2 &column, const ObObj &old,
    ObSqlString &sql, ObIArray<ObObj> &parameters)
{
  int ret = OB_SUCCESS; const int64_t index = parameters.count();
  if (OB_FAIL(sql.append(" AND (("))) {}
  else if (OB_FAIL(quote_identifier(sql, column.get_column_name_str()))) {}
  else if (OB_FAIL(sql.append(" <=> "))) {}
  else if (OB_FAIL(parameter(sql, parameters, old))) {}
  else if (OB_FAIL(sql.append(") AND (OCTET_LENGTH("))) {}
  else if (OB_FAIL(quote_identifier(sql, column.get_column_name_str()))) {}
  else { ret = sql.append_fmt(") <=> OCTET_LENGTH(@%s%ld)))", AiColumnSql::PARAMETER_PREFIX, index); }
  return ret;
}
int random_token(char (&token)[32])
{
  unsigned char data[16]; constexpr char HEX[] = "0123456789abcdef";
  if (RAND_bytes(data, sizeof(data)) != 1) { return OB_ERR_UNEXPECTED; }
  for (int64_t i = 0; i < 16; ++i) { token[2*i] = HEX[data[i]>>4]; token[2*i+1] = HEX[data[i]&15]; }
  return OB_SUCCESS;
}
int copy_key(const AiColumnSqlRow &row, int64_t count, ObIArray<ObObj> &key)
{
  int ret = OB_SUCCESS;
  if (row.values_.count() != count+1) { return OB_INVALID_DATA; }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) { ret = key.push_back(row.values_.at(i)); }
  return ret;
}
bool same_key(const ObIArray<ObObj> &a, const ObIArray<ObObj> &b)
{
  bool same = a.count() == b.count();
  for (int64_t i = 0; same && i < a.count(); ++i) {
    same = a.at(i).get_type() == b.at(i).get_type() && a.at(i).get_collation_type() == b.at(i).get_collation_type();
    if (same && ob_is_string_type(a.at(i).get_type())) { same = a.at(i).get_string() == b.at(i).get_string(); }
    else if (same) { same = a.at(i) == b.at(i); }
  }
  return same;
}
class RowWorker
{
public:
  RowWorker(sqlclient::ObISQLConnection &connection, ObSQLSessionInfo &session,
      const ObTableSchema &table, const ObColumnSchemaV2 &state_column, const ObString &name,
      const ObIArray<const ObColumnSchemaV2 *> &columns, ObIAllocator &allocator)
      : connection_(connection), session_(session), table_(table), state_column_(state_column),
        name_(name), columns_(columns), allocator_(allocator) {}
  int process(const AiColumnSqlRow &row)
  {
    int ret = OB_SUCCESS; ObSEArray<ObObj, 4> key; AiColumnOutbox state;
    ObSchemaGetterGuard current_schema; const ObTableSchema *current_table = nullptr;
    if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(current_schema))) { return ret; }
    if (OB_FAIL(current_schema.get_table_schema(table_.get_table_id(), current_table))) { return ret; }
    if (current_table == nullptr || current_table->get_schema_version() != table_.get_schema_version()) { return OB_SUCCESS; }
    if (OB_FAIL(copy_key(row, table_.get_rowkey_info().get_size(), key))) { return ret; }
    const ObObj &old = row.values_.at(key.count());
    if (!old.is_null() && OB_FAIL(state.decode(old.get_string()))) { return ret; }
    if (!old.is_null() && !same_key(state.rowkey(), key)) { return OB_INVALID_DATA; }
    ObSEArray<const ObColumnSchemaV2 *, 4> missing;
    for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
      const AiColumnTask *task = old.is_null() ? nullptr : state.find(columns_.at(i)->get_column_id());
      if (task == nullptr || task->definition_version_ != columns_.at(i)->get_schema_version()
          || task->stage_ == static_cast<int64_t>(AiColumnStage::NEEDS_INPUT)) { ret = missing.push_back(columns_.at(i)); }
    }
    if (OB_FAIL(ret)) { return ret; }
    if (!missing.empty()) { return seed(key, old, missing); }
    // A crash after staging terminal results needs publication, not another request.
    for (int64_t i = 0; i < state.count(); ++i) {
      const AiColumnTask &task = state.at(i);
      if ((task.stage_ == static_cast<int64_t>(AiColumnStage::SUCCEEDED)
          || task.stage_ == static_cast<int64_t>(AiColumnStage::EXHAUSTED)) && state.group_ready(task.group_)) {
        return persist(key, old, state, task.group_);
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
      const ObColumnSchemaV2 &column = *columns_.at(i); const AiColumnTask *task = state.find(column.get_column_id());
      if (task == nullptr || task->terminal()) { continue; }
      AiColumnClaim claim; bool acquired = false; char token[32]; ObString encoded;
      if (OB_FAIL(random_token(token))) {}
      else if (OB_FAIL(state.claim(allocator_, column.get_column_id(), ObTimeUtility::current_time(), LEASE_US,
          ObString(sizeof(token), token), claim, acquired))) {}
      else if (OB_FAIL(state.encode(allocator_, encoded))) {}
      if (OB_FAIL(ret)) { return ret; }
      if (encoded == old.get_string()) { continue; }
      int64_t affected = 0;
      if (OB_FAIL(cas_state(key, old, encoded, affected))) { return ret; }
      if (affected == 0) { return OB_SUCCESS; }
      if (affected != 1 || session_.is_in_transaction()) { return OB_ERR_UNEXPECTED; }
      if (!acquired) { return OB_SUCCESS; } // expired final attempt is durable; next tick publishes it
      AiColumnDefinition definition; ObObj output;
      if (OB_FAIL(definition.decode(allocator_, column.get_ai_column_info()))) { return ret; }
      const int model_error = AiColumnModelEvaluator::evaluate(connection_, session_, column, definition,
          *state.find(column.get_column_id()), allocator_, output);
      return complete(key, claim, model_error, output);
    }
    return ret;
  }
private:
  int seed(const ObIArray<ObObj> &key, const ObObj &old, const ObIArray<const ObColumnSchemaV2 *> &missing)
  {
    int ret = OB_SUCCESS; ObSqlString sql; ObSEArray<ObObj, 8> parameters;
    if (OB_FAIL(sql.append_fmt("UPDATE %.*s SET ", name_.length(), name_.ptr()))) {}
    for (int64_t i = 0; OB_SUCC(ret) && i < missing.count(); ++i) {
      if (i > 0 && OB_FAIL(sql.append(","))) {}
      else if (OB_FAIL(quote_identifier(sql, missing.at(i)->get_column_name_str()))) {}
      else { ret = sql.append("=DEFAULT"); }
    }
    if (OB_SUCC(ret)) { ret = sql.append(" WHERE "); }
    if (OB_SUCC(ret)) { ret = key_predicate(table_, key, sql, parameters); }
    if (OB_SUCC(ret)) { ret = state_predicate(state_column_, old, sql, parameters); }
    int64_t affected = 0;
    if (OB_SUCC(ret)) { ret = AiColumnSql::write(connection_, session_, sql.string(), parameters, affected, false); }
    if (OB_SUCC(ret) && affected > 1) { ret = OB_ERR_UNEXPECTED; } return ret;
  }
  int cas_state(const ObIArray<ObObj> &key, const ObObj &old, const ObString &encoded, int64_t &affected)
  {
    int ret = OB_SUCCESS; ObSqlString sql; ObSEArray<ObObj, 8> parameters; ObObj value;
    value.set_varchar(encoded); value.set_collation_type(CS_TYPE_BINARY);
    if (OB_FAIL(sql.append_fmt("UPDATE %.*s SET ", name_.length(), name_.ptr()))) {}
    else if (OB_FAIL(quote_identifier(sql, state_column_.get_column_name_str()))) {}
    else if (OB_FAIL(sql.append("="))) {}
    else if (OB_FAIL(parameter(sql, parameters, value))) {}
    else if (OB_FAIL(sql.append(" WHERE "))) {}
    else if (OB_FAIL(key_predicate(table_, key, sql, parameters))) {}
    else if (OB_FAIL(state_predicate(state_column_, old, sql, parameters))) {}
    else { ret = AiColumnSql::write(connection_, session_, sql.string(), parameters, affected); }
    return ret;
  }
  int persist(const ObIArray<ObObj> &key, const ObObj &old, AiColumnOutbox &state, int64_t group)
  {
    int ret = OB_SUCCESS; ObSqlString sql; ObSEArray<ObObj, 24> parameters;
    bool publish = group > 0 && state.group_ready(group); bool any = false;
    ObSchemaGetterGuard fresh; const ObTableSchema *current = nullptr;
    if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(fresh))) { return ret; }
    if (OB_FAIL(fresh.get_table_schema(table_.get_table_id(), current))) { return ret; }
    if (current == nullptr) { return OB_SUCCESS; }
    const auto *current_state = current->get_column_schema(state_column_.get_column_id());
    if (current_state == nullptr || !current_state->is_ai_column_outbox()) { return OB_INVALID_DATA; }
    ObSqlString current_name;
    if (OB_FAIL(table_name(fresh, *current, current_name))) { return ret; }
    if (OB_FAIL(sql.append_fmt("UPDATE %.*s SET ", current_name.length(), current_name.ptr()))) {}
    for (int64_t i = 0; OB_SUCC(ret) && publish && i < state.count(); ++i) {
      const AiColumnTask &task = state.at(i);
      if (task.group_ == group) {
        const ObColumnSchemaV2 *column = current->get_column_schema(task.column_id_); ObObj value;
        if (column == nullptr || !column->is_ai_column() || column->get_schema_version() != task.definition_version_) {
          return OB_EAGAIN; // re-read the DDL fence before completing still-valid members
        }
        if (task.stage_ == static_cast<int64_t>(AiColumnStage::SUCCEEDED)) { value = task.result_; }
        if (any && OB_FAIL(sql.append(","))) {}
        else if (OB_FAIL(quote_identifier(sql, column->get_column_name_str()))) {}
        else if (OB_FAIL(sql.append("="))) {}
        else if (OB_FAIL(parameter(sql, parameters, value))) {}
        else { any = true; }
      }
    }
    ObString encoded; ObObj value;
    if (OB_SUCC(ret) && publish) { ret = state.publish(group); }
    if (OB_SUCC(ret)) { ret = state.encode(allocator_, encoded); }
    value.set_varchar(encoded); value.set_collation_type(CS_TYPE_BINARY);
    if (OB_SUCC(ret) && any) { ret = sql.append(","); }
    if (OB_SUCC(ret)) { ret = quote_identifier(sql, current_state->get_column_name_str()); }
    if (OB_SUCC(ret)) { ret = sql.append("="); }
    if (OB_SUCC(ret)) { ret = parameter(sql, parameters, value); }
    if (OB_SUCC(ret)) { ret = sql.append(" WHERE "); }
    if (OB_SUCC(ret)) { ret = key_predicate(*current, key, sql, parameters); }
    if (OB_SUCC(ret)) { ret = state_predicate(*current_state, old, sql, parameters); }
    int64_t affected = 0;
    if (OB_SUCC(ret)) { ret = AiColumnSql::write(connection_, session_, sql.string(), parameters, affected); }
    if (OB_SUCC(ret) && affected > 1) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_SUCC(ret) && affected == 0) { ret = OB_EAGAIN; }
    return ret;
  }
  int complete(const ObIArray<ObObj> &key, const AiColumnClaim &claim, int model_error, const ObObj &output)
  {
    int ret = OB_EAGAIN;
    for (int64_t attempt = 0; ret == OB_EAGAIN && attempt < 32; ++attempt) {
      // Retain the model response. A different member may have staged its
      // result since our read; that does not invalidate this member's claim.
      ret = complete_once(key, claim, model_error, output);
    }
    return ret;
  }
  int complete_once(const ObIArray<ObObj> &key, const AiColumnClaim &claim, int model_error, const ObObj &output)
  {
    int ret = OB_SUCCESS; ObSchemaGetterGuard fresh; const ObTableSchema *table = nullptr;
    if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(fresh))) { return ret; }
    if (OB_FAIL(fresh.get_table_schema(table_.get_table_id(), table))) { return ret; }
    const ObColumnSchemaV2 *column = table == nullptr ? nullptr : table->get_column_schema(claim.column_id_);
    if (column == nullptr || !column->is_ai_column() || column->get_schema_version() != claim.definition_version_) { return OB_SUCCESS; }
    const auto *current_state = table->get_column_schema(state_column_.get_column_id());
    if (current_state == nullptr || !current_state->is_ai_column_outbox()) { return OB_INVALID_DATA; }
    ObSqlString query, current_name; ObSEArray<ObObj, 8> parameters; ObSEArray<AiColumnSqlRow, 1> rows;
    if (OB_FAIL(table_name(fresh, *table, current_name))) {}
    else if (OB_FAIL(query.append("SELECT "))) {}
    else if (OB_FAIL(quote_identifier(query, current_state->get_column_name_str()))) {}
    else if (OB_FAIL(query.append_fmt(" FROM %.*s WHERE ", current_name.length(), current_name.ptr()))) {}
    else if (OB_FAIL(key_predicate(*table, key, query, parameters))) {}
    else if (OB_FAIL(AiColumnSql::read(connection_, session_, query.string(), parameters, allocator_, rows, 1))) {}
    if (OB_FAIL(ret) || rows.empty()) { return ret; }
    if (rows.at(0).values_.count() != 1 || rows.at(0).values_.at(0).is_null()) { return OB_SUCCESS; }
    const ObObj &old = rows.at(0).values_.at(0); AiColumnOutbox state; bool accepted = false;
    if (OB_FAIL(state.decode(old.get_string()))) { return ret; }
    const AiColumnTask *task = state.find(claim.column_id_);
    if (task == nullptr) { return OB_SUCCESS; }
    const int64_t group = task->group_;
    const int64_t retry_delay = std::min<int64_t>(5000000, 100000LL << std::min<int64_t>(claim.attempt_-1, 6));
    const int64_t now = ObTimeUtility::current_time();
    if (OB_FAIL(state.finish(allocator_, claim, now, model_error, output, accepted, retry_delay))) {
      if (ret == OB_SIZE_OVERFLOW && OB_SUCC(state.decode(old.get_string()))) {
        ObObj empty; ret = state.finish(allocator_, claim, now, OB_SIZE_OVERFLOW, empty, accepted, retry_delay);
      }
    }
    if (OB_SUCC(ret) && accepted) { ret = persist(key, old, state, group); }
    return ret;
  }
  sqlclient::ObISQLConnection &connection_; ObSQLSessionInfo &session_; const ObTableSchema &table_;
  const ObColumnSchemaV2 &state_column_; ObString name_; const ObIArray<const ObColumnSchemaV2 *> &columns_;
  ObIAllocator &allocator_;
};
int process_table(ObSchemaGetterGuard &schema, const ObTableSchema &table,
    const ObColumnSchemaV2 &state_column, const ObIArray<const ObColumnSchemaV2 *> &columns,
    const AiColumnWorker &worker)
{
  int ret = OB_SUCCESS; ObArenaAllocator allocator("AiColPoll"); ObSqlString name, query;
  sqlclient::ObISQLConnectionGuard connection;
  if (OB_FAIL(table_name(schema, table, name))) {}
  else if (OB_FAIL(create_inner_sql_connection_for_proxy(false, 0, connection))) {}
  else if (connection.get_ptr() == nullptr) { ret = OB_ERR_UNEXPECTED; }
  if (OB_FAIL(ret)) { return ret; }
  ObSQLSessionInfo &session = static_cast<ObInnerSQLConnection *>(connection.get_ptr())->get_session();
  // Model execution may use the provider's normal timeout; the claim outlives it.
  connection->set_user_timeout(65 * 1000 * 1000);
  if (OB_FAIL(query.append("SELECT "))) {}
  for (int64_t i = 0; OB_SUCC(ret) && i < table.get_rowkey_info().get_size(); ++i) {
    uint64_t id = 0; const ObColumnSchemaV2 *column = nullptr;
    if (OB_FAIL(table.get_rowkey_info().get_column_id(i, id))) {}
    else if ((column = table.get_column_schema(id)) == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(quote_identifier(query, column->get_column_name_str()))) {}
    else { ret = query.append(","); }
  }
  if (OB_SUCC(ret)) { ret = quote_identifier(query, state_column.get_column_name_str()); }
  if (OB_SUCC(ret)) { ret = query.append_fmt(" FROM %.*s WHERE ", name.length(), name.ptr()); }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    if (i > 0 && OB_FAIL(query.append(" OR "))) {}
    else if (OB_FAIL(query.append("__ai_column_status("))) {}
    else if (OB_FAIL(quote_identifier(query, state_column.get_column_name_str()))) {}
    else { ret = query.append_fmt(",%lu,%ld)='PENDING'", columns.at(i)->get_column_id(), columns.at(i)->get_schema_version()); }
  }
  if (OB_SUCC(ret)) { ret = query.append_fmt(" LIMIT %ld", BATCH_ROWS); }
  ObSEArray<ObObj, 1> parameters; ObSEArray<AiColumnSqlRow, BATCH_ROWS> rows;
  if (OB_SUCC(ret)) { ret = AiColumnSql::read(*connection, session, query.string(), parameters, allocator, rows, BATCH_ROWS); }
  for (int64_t i = 0; OB_SUCC(ret) && !worker.has_set_stop() && i < rows.count(); ++i) {
    RowWorker job(*connection, session, table, state_column, name.string(), columns, allocator);
    const int job_ret = job.process(rows.at(i));
    if (job_ret != OB_SUCCESS) { LOG_WARN("AI column row task failed", K(job_ret), "table_id", table.get_table_id()); }
  }
  return ret;
}
}
int AiColumnWorker::process_tables()
{
  int ret = OB_SUCCESS; ObSchemaGetterGuard schema; ObSEArray<const ObTableSchema *, 16> tables;
  if (GCTX.schema_service_ == nullptr || GCTX.sql_proxy_ == nullptr) { return OB_NOT_INIT; }
  if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema))) {}
  else if (OB_FAIL(schema.get_table_schemas_in_runtime(tables))) {}
  for (int64_t i = 0; OB_SUCC(ret) && !has_set_stop() && i < tables.count(); ++i) {
    const ObTableSchema *table = tables.at(i);
    if (table == nullptr || !table->is_user_table() || table->is_index_table() || table->is_view_table()) { continue; }
    const ObColumnSchemaV2 *state_column = nullptr; ObSEArray<const ObColumnSchemaV2 *, 4> columns;
    for (auto it = table->column_begin(); OB_SUCC(ret) && it != table->column_end(); ++it) {
      if ((*it)->is_ai_column_outbox()) { state_column = *it; }
      if ((*it)->is_ai_column()) { ret = columns.push_back(*it); }
    }
    if (OB_SUCC(ret) && state_column != nullptr && !columns.empty()) {
      const int table_ret = process_table(schema, *table, *state_column, columns, *this);
      if (table_ret != OB_SUCCESS) { LOG_WARN("AI column table poll failed", K(table_ret), "table_id", table->get_table_id()); }
    }
  }
  return ret;
}
void AiColumnWorker::run1()
{
  lib::set_thread_name("AiColWorker");
  while (!has_set_stop()) {
    const int ret = process_tables();
    if (ret != OB_SUCCESS && ret != OB_NOT_INIT) { LOG_WARN("AI column worker tick failed", K(ret)); }
    ob_usleep(100000);
  }
}
}
