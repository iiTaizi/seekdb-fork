/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/ai_column/ai_column_status_expr.h"
#include "sql/engine/ob_exec_context.h"
#include "share/ai_column/ai_column_state.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/resolver/expr/ob_raw_expr.h"
#include "sql/session/ob_sql_session_info.h"
namespace oceanbase::sql {
using namespace common;
using namespace share;
AiColumnStatusExpr::AiColumnStatusExpr(ObIAllocator &allocator)
    : ObFuncExprOperator(allocator, T_FUN_SYS_AI_COLUMN_STATUS, "__ai_column_status", 3,
        NOT_VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION, true) {}
int AiColumnStatusExpr::calc_result_type3(ObExprResType &type, ObExprResType &state,
    ObExprResType &column, ObExprResType &version, ObExprTypeCtx &ctx) const
{
  UNUSED(ctx); int ret = OB_SUCCESS;
  if ((!state.is_null() && !ob_is_string_type(state.get_type()))
      || !ob_is_integer_type(column.get_type()) || !ob_is_integer_type(version.get_type())) { ret = OB_INVALID_ARGUMENT; }
  else {
    column.set_calc_type(ObIntType); version.set_calc_type(ObIntType);
    type.set_varchar(); type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_IMPLICIT); type.set_length(7);
  }
  return ret;
}
int AiColumnStatusExpr::cg_expr(ObExprCGCtx &ctx, const ObRawExpr &raw, ObExpr &expr) const
{
  int ret = raw.has_ai_column_strict_marker() || raw.has_ai_column_status_marker()
      || (ctx.session_ != nullptr && ctx.session_->is_ai_column_internal_read()) ? OB_SUCCESS : OB_NOT_SUPPORTED;
  if (OB_SUCC(ret)) {
    expr.extra_ = raw.has_ai_column_strict_marker() ? 2 : raw.has_ai_column_status_marker() ? 1 : 0;
    expr.eval_func_ = eval_status;
  }
  return ret;
}
int AiColumnStatusExpr::eval_status(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &result)
{
  int ret = OB_SUCCESS; ObDatum *state = nullptr, *column = nullptr, *version = nullptr;
  AiColumnStatus status = AiColumnStatus::PENDING;
  if (expr.extra_ != 2 && expr.extra_ != 1 && (ctx.exec_ctx_.get_my_session() == nullptr
      || !ctx.exec_ctx_.get_my_session()->is_ai_column_internal_read())) { ret = OB_NOT_SUPPORTED; }
  else if (OB_FAIL(expr.eval_param_value(ctx, state, column, version))) {}
  else if (column->is_null() || version->is_null() || column->get_int() <= 0 || version->get_int() <= 0) { ret = OB_INVALID_ARGUMENT; }
  else if (!state->is_null()) {
    ObEvalCtx::TempAllocGuard guard(ctx); ObString data; AiColumnOutbox outbox;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, guard.get_allocator(),
        *state, expr.args_[0]->datum_meta_, expr.args_[0]->obj_meta_.has_lob_header(), data))) {}
    else if (OB_FAIL(outbox.decode(data))) {}
    else {
      const AiColumnTask *task = outbox.find(column->get_int());
      if (task != nullptr && task->definition_version_ == version->get_int()) { status = task->status(); }
    }
  }
  if (OB_SUCC(ret) && expr.extra_ == 2 && status != AiColumnStatus::READY) {
    ret = OB_STATE_NOT_MATCH;
    LOG_USER_ERROR(OB_STATE_NOT_MATCH);
  }
  if (OB_SUCC(ret)) {
    const char *name = status == AiColumnStatus::READY ? "READY" : status == AiColumnStatus::FAILED ? "FAILED" : "PENDING";
    result.set_string(ObString::make_string(name));
  }
  return ret;
}
}
