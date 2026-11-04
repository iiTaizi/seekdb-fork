/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/ai_column/ai_column_model_evaluator.h"
#include "sql/engine/ai_column/ai_column_internal_write_guard.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/ob_sql.h"
#include "sql/ob_sql_context.h"
#include "sql/ob_result_set.h"
#include "sql/ob_sql_utils.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "query/vector/ob_vector_index_util.h"

namespace oceanbase::sql {
using namespace common;
using namespace share;
using namespace share::schema;
namespace {
class ModelSessionGuard
{
public:
  ModelSessionGuard(ObSQLSessionInfo &session, const AiColumnDefinition &definition)
      : session_(session), mode_(session.get_sql_mode()), user_(session.get_priv_user_id_allow_invalid()),
        worker_session_(THIS_WORKER.get_session())
  {
    session_.set_sql_mode(definition.sql_mode_ | SMO_STRICT_ALL_TABLES);
    session_.set_priv_user_id(definition.definer_id_);
    THIS_WORKER.set_session(&session_);
  }
  ~ModelSessionGuard()
  {
    THIS_WORKER.set_session(worker_session_); session_.set_priv_user_id(user_); session_.set_sql_mode(mode_);
  }
private:
  ObSQLSessionInfo &session_; ObSQLMode mode_; uint64_t user_; ObSQLSessionInfo *worker_session_;
};
int model_constant(ObRawExprFactory &factory, const ObObj &value, ObRawExpr *&expr)
{
  int ret = OB_SUCCESS; ObConstRawExpr *constant = nullptr;
  if (!AiColumnOutbox::durable_value(value)) { ret = OB_INVALID_ARGUMENT; }
  else if (OB_FAIL(factory.create_raw_expr(static_cast<ObItemType>(value.get_type()), constant))) {}
  else { constant->set_value(value); expr = constant; }
  return ret;
}
const char *format_hint(const ObColumnSchemaV2 &column)
{
  if (column.is_collection() || ob_is_string_type(column.get_data_type())) { return nullptr; }
  if (column.is_json()) { return "Return only valid JSON. Do not include markdown or explanatory text."; }
  if (ob_is_numeric_type(column.get_data_type())) { return "Return only a valid JSON number or null. Do not include markdown or explanatory text."; }
  if (column.get_data_type() == ObDateType) { return "Return only a JSON string in YYYY-MM-DD format or null."; }
  if (ob_is_temporal_type(column.get_data_type())) { return "Return only a JSON string containing a valid SQL date/time value or null."; }
  return "Return only a valid JSON scalar or null. Do not include markdown or explanatory text.";
}
int append_format_hint(ObRawExprFactory &factory, ObIAllocator &allocator, const char *hint, ObRawExpr *&prompt)
{
  int ret = OB_SUCCESS; ObSysFunRawExpr *wrapped = nullptr; ObRawExpr *template_expr = nullptr;
  ObSqlString text; ObString saved; ObObj value;
  if (OB_FAIL(text.append("{0}\n"))) {}
  else if (OB_FAIL(text.append(hint))) {}
  else if (OB_FAIL(ob_write_string(allocator, text.string(), saved))) {}
  else if (OB_FALSE_IT(value.set_varchar(saved))) {}
  else if (OB_FALSE_IT(value.set_collation_type(CS_TYPE_UTF8MB4_BIN))) {}
  else if (OB_FAIL(model_constant(factory, value, template_expr))) {}
  else if (OB_FAIL(factory.create_raw_expr(T_FUN_SYS_AI_PROMPT, wrapped))) {}
  else if (OB_FAIL(wrapped->init_param_exprs(2))) {}
  else if (OB_FAIL(wrapped->add_param_expr(template_expr))) {}
  else if (OB_FAIL(wrapped->add_param_expr(prompt))) {}
  else { wrapped->set_func_name(ObString::make_string("AI_PROMPT")); prompt = wrapped; }
  return ret;
}
int result_source(ObExecContext &ctx, ObIAllocator &allocator, const ObColumnSchemaV2 &column,
    const ObObj &model_result, ObObj &source)
{
  int ret = OB_SUCCESS; ObString text; ObIJsonBase *json = nullptr;
  if (model_result.is_null()) { source.set_null(); return ret; }
  if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx, &allocator, model_result, text))) {}
  else if (text.empty() || text.length() > AiColumnOutbox::MAX_BYTES) { ret = OB_INVALID_DATA; }
  else if (ob_is_string_type(column.get_data_type())) {
    source.set_varchar(text); source.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  } else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, text, ObJsonInType::JSON_TREE,
      ObJsonInType::JSON_TREE, json))) {}
  else if (json == nullptr) { ret = OB_INVALID_DATA; }
  else if (column.is_json()) {
    ObString binary;
    if (OB_FAIL(json->get_raw_binary(binary, &allocator))) {}
    else { source.set_json_value(ObJsonType, binary.ptr(), binary.length()); }
  } else if (json->json_type() == ObJsonNodeType::J_NULL) { source.set_null(); }
  else if (ob_is_temporal_type(column.get_data_type()) && json->json_type() != ObJsonNodeType::J_STRING) {
    ret = OB_INVALID_DATA;
  }
  else if (column.is_collection()) {
    int64_t dimension = 0; ObJsonArray outer(&allocator);
    if (json->json_type() != ObJsonNodeType::J_ARRAY) { ret = OB_INVALID_DATA; }
    else if (OB_FAIL(ObVectorIndexUtil::get_vector_dim_from_extend_type_info(column.get_extended_type_info(), dimension))) {}
    else if (OB_FAIL(outer.append(static_cast<ObJsonNode *>(json)))) {}
    else if (OB_FAIL(ObAIFuncUtils::validate_embeddings(&outer, 1, dimension))) {}
    else { source.set_varchar(text); source.set_collation_type(CS_TYPE_UTF8MB4_BIN); }
  } else if (json->json_type() == ObJsonNodeType::J_STRING) {
    source.set_varchar(ObString(json->get_data_length(), json->get_data()));
    source.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  } else if (json->json_type() == ObJsonNodeType::J_BOOLEAN) { source.set_tinyint(json->get_boolean() ? 1 : 0); }
  else if (json->json_type() == ObJsonNodeType::J_INT || json->json_type() == ObJsonNodeType::J_UINT
      || json->json_type() == ObJsonNodeType::J_DOUBLE || json->json_type() == ObJsonNodeType::J_DECIMAL) {
    if (!ob_is_numeric_type(column.get_data_type())) { ret = OB_INVALID_DATA; }
    else { source.set_varchar(text); source.set_collation_type(CS_TYPE_UTF8MB4_BIN); }
  } else { ret = OB_INVALID_DATA; }
  return ret;
}
class ModelExecutor : public sqlclient::ObIExecutor
{
public:
  ModelExecutor(const ObColumnSchemaV2 &column, const AiColumnDefinition &definition,
      const AiColumnTask &task, ObIAllocator &allocator, ObObj &output)
      : column_(column), definition_(definition), task_(task), allocator_(allocator), output_(output) {}
  int execute(ObSql &engine, ObSqlCtx &ctx, ObResultSet &result) override
  { return engine.stmt_query(ObString::make_string("SELECT 1"), ctx, result); }
  int process_result(ObResultSet &result) override
  {
    int ret = OB_SUCCESS; ObSQLSessionInfo &session = result.get_session(); ObExecContext &ctx = result.get_exec_context();
    LinkExecCtxGuard link(session, ctx); ModelSessionGuard environment(session, definition_);
    ObSchemaGetterGuard *schema = ctx.get_sql_ctx() == nullptr ? nullptr : ctx.get_sql_ctx()->schema_guard_;
    ObSessionPrivInfo privileges; bool allowed = false;
    if (schema == nullptr || definition_.definer_id_ == 0 || definition_.definer_id_ == OB_INVALID_ID) { ret = OB_ERR_NO_PRIVILEGE; }
    else if (OB_FAIL(schema->get_session_priv_info(definition_.definer_id_, session.get_database_name(), privileges))) {}
    else {
      ObAIServiceEndpointPrivUtil checker(*schema);
      ret = checker.check_access_ai_model_priv(allocator_, privileges, allowed);
      if (OB_SUCC(ret) && !allowed) { ret = OB_ERR_NO_PRIVILEGE; }
    }
    ObRawExprFactory factory(allocator_); ObSysFunRawExpr *root = nullptr;
    const bool embed = definition_.function_ == static_cast<int64_t>(AiColumnFunction::EMBED);
    if (OB_SUCC(ret) && (task_.inputs_.count() < 2 || task_.inputs_.count() > 3)) { ret = OB_INVALID_DATA; }
    if (OB_SUCC(ret)) { ret = factory.create_raw_expr(embed ? T_FUN_SYS_AI_EMBED : T_FUN_SYS_AI_COMPLETE, root); }
    if (OB_SUCC(ret)) { ret = root->init_param_exprs(task_.inputs_.count()); }
    if (OB_SUCC(ret)) { root->set_func_name(ObString::make_string(embed ? "AI_EMBED" : "AI_COMPLETE")); }
    for (int64_t i = 0; OB_SUCC(ret) && i < task_.inputs_.count(); ++i) {
      ObRawExpr *arg = nullptr;
      if (task_.inputs_.at(i).argument_position_ != static_cast<uint64_t>(i+1)) { ret = OB_INVALID_DATA; }
      else if (OB_FAIL(model_constant(factory, task_.inputs_.at(i).value_, arg))) {}
      else if (!embed && i == 1 && format_hint(column_) != nullptr) {
        if (task_.inputs_.at(i).value_.is_json()) {
          ObIJsonBase *prompt = nullptr;
          if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator_, task_.inputs_.at(i).value_.get_string(),
              ObJsonInType::JSON_BIN, ObJsonInType::JSON_TREE, prompt))) {}
          else if (prompt == nullptr || prompt->json_type() != ObJsonNodeType::J_OBJECT
              || !ObAIFuncPromptObjectUtils::is_valid_prompt_object(static_cast<ObJsonObject *>(prompt))) {
            ret = OB_INVALID_ARGUMENT;
          }
        }
        if (OB_SUCC(ret)) { ret = append_format_hint(factory, allocator_, format_hint(column_), arg); }
      }
      if (OB_SUCC(ret)) { ret = root->add_param_expr(arg); }
    }
    ObObj raw, source, converted; ObRawExpr *conversion = nullptr; ObColumnRefRawExpr *target = nullptr;
    ParamStore params((ObWrapperAllocator(allocator_)));
    if (OB_SUCC(ret)) { ret = root->formalize(&session); }
    if (OB_SUCC(ret)) { ret = ObSQLUtils::calc_const_expr(&session, *root, raw, allocator_, params, &ctx); }
    if (OB_SUCC(ret)) { ret = result_source(ctx, allocator_, column_, raw, source); }
    if (OB_SUCC(ret)) { ret = model_constant(factory, source, conversion); }
    if (OB_SUCC(ret)) { ret = ObRawExprUtils::build_column_expr(factory, column_, &session, target); }
    if (OB_SUCC(ret)) { ret = ObRawExprUtils::build_column_conv_expr(factory, allocator_, *target, conversion, &session); }
    if (OB_SUCC(ret)) { ret = conversion->formalize(&session); }
    if (OB_SUCC(ret)) { ret = ObSQLUtils::calc_const_expr(&session, *conversion, converted, allocator_, params, &ctx); }
    // Collection subschema IDs belong to the current execution context. Stage
    // the validated source and let publication use the normal target-column cast.
    if (OB_SUCC(ret)) { ret = ob_write_obj(allocator_, source, output_); }
    return ret;
  }
private:
  const ObColumnSchemaV2 &column_; const AiColumnDefinition &definition_; const AiColumnTask &task_;
  ObIAllocator &allocator_; ObObj &output_;
};
}
int AiColumnModelEvaluator::evaluate(sqlclient::ObISQLConnection &connection, ObSQLSessionInfo &session,
    const ObColumnSchemaV2 &column, const AiColumnDefinition &definition, const AiColumnTask &task,
    ObIAllocator &allocator, ObObj &output)
{
  int ret = OB_SUCCESS; output.set_null();
  if (session.is_in_transaction()) { return OB_STATE_NOT_MATCH; }
  if (!definition.valid() || !task.valid() || task.column_id_ != column.get_column_id()
      || task.definition_version_ != column.get_schema_version()
      || task.stage_ != static_cast<int64_t>(AiColumnStage::CLAIMED)) { return OB_INVALID_ARGUMENT; }
  AiColumnInternalWriteGuard permission(session);
  ModelExecutor executor(column, definition, task, allocator, output);
  ret = connection.execute(executor);
  if (OB_FAIL(ret)) { output.set_null(); }
  return ret;
}
}
