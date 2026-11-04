/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/ai_column/ai_column_capture_expr.h"
#include "share/ai_column/ai_column_state.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/resolver/expr/ob_raw_expr.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include <openssl/rand.h>
#include <climits>

namespace oceanbase::sql {
using namespace common;
using namespace share;
namespace {
int capture_value(const ObExpr &expr, ObEvalCtx &ctx, ObIAllocator &allocator,
                  int64_t index, ObObj &value)
{
  int ret = OB_SUCCESS; ObDatum *datum = nullptr;
  if (index < 0 || index >= expr.arg_cnt_ || expr.args_[index] == nullptr) { ret = OB_INVALID_ARGUMENT; }
  else {
    const ObExpr &arg = *expr.args_[index];
    if (OB_FAIL(arg.eval(ctx, datum))) {}
    else if (datum == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (datum->is_null()) { value.set_null(); }
    else if (ob_is_string_type(arg.datum_meta_.type_) || ob_is_json(arg.datum_meta_.type_)) {
      ObString bytes;
      if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator, *datum,
          arg.datum_meta_, arg.obj_meta_.has_lob_header(), bytes))) {}
      else if (bytes.length() > AiColumnOutbox::MAX_BYTES) { ret = OB_SIZE_OVERFLOW; }
      else {
        value.set_string(arg.datum_meta_.type_, bytes);
        value.set_collation_type(arg.obj_meta_.get_collation_type());
        value.set_collation_level(arg.obj_meta_.get_collation_level());
        value.set_inrow();
      }
    } else { ret = datum->to_obj(value, arg.obj_meta_, arg.obj_datum_map_); }
    if (OB_SUCC(ret) && !AiColumnOutbox::durable_value(value)) { ret = OB_INVALID_ARGUMENT; }
  }
  return ret;
}
int capture_integer(const ObExpr &expr, ObEvalCtx &ctx, int64_t &position, int64_t &value)
{
  int ret = OB_SUCCESS; ObDatum *datum = nullptr;
  if (position >= expr.arg_cnt_ || expr.args_[position] == nullptr
      || !expr.args_[position]->is_const_expr()
      || !ob_is_integer_type(expr.args_[position]->datum_meta_.type_)) { ret = OB_INVALID_ARGUMENT; }
  else {
    const ObExpr &arg = *expr.args_[position++];
    if (OB_FAIL(arg.eval(ctx, datum))) {}
    else if (datum == nullptr || datum->is_null()) { ret = OB_INVALID_ARGUMENT; }
    else if (ob_is_unsigned_type(arg.datum_meta_.type_) && datum->get_uint() > INT64_MAX) { ret = OB_INVALID_ARGUMENT; }
    else { value = ob_is_unsigned_type(arg.datum_meta_.type_) ? datum->get_uint() : datum->get_int(); }
  }
  return ret;
}
int capture_identity(char (&identity)[32])
{
  unsigned char random[16]; static constexpr char HEX[] = "0123456789abcdef";
  int ret = RAND_bytes(random, sizeof(random)) == 1 ? OB_SUCCESS : OB_ERR_UNEXPECTED;
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; i < 16; ++i) { identity[2*i] = HEX[random[i] >> 4]; identity[2*i+1] = HEX[random[i] & 15]; }
  }
  return ret;
}
}
AiColumnCaptureExpr::AiColumnCaptureExpr(ObIAllocator &allocator)
    : ObFuncExprOperator(allocator, T_FUN_SYS_AI_COLUMN_CAPTURE, "__ai_column_capture",
        MORE_THAN_ZERO, NOT_VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION, true) {}
int AiColumnCaptureExpr::calc_result_typeN(ObExprResType &type, ObExprResType *types,
    int64_t count, ObExprTypeCtx &ctx) const
{
  UNUSED(types); UNUSED(ctx);
  int ret = count >= 9 && count <= AiColumnOutbox::MAX_INPUTS + 5 * AiColumnOutbox::MAX_COLUMNS
      + AiColumnOutbox::MAX_ROWKEY + 4 ? OB_SUCCESS : OB_INVALID_ARGUMENT;
  if (OB_SUCC(ret)) {
    type.set_type(ObLongTextType); type.set_collation_type(CS_TYPE_BINARY);
    type.set_collation_level(CS_LEVEL_IMPLICIT);
    type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObLongTextType]);
  }
  return ret;
}
int AiColumnCaptureExpr::cg_expr(ObExprCGCtx &ctx, const ObRawExpr &raw, ObExpr &expr) const
{
  int ret = raw.has_ai_column_capture_marker()
      || (ctx.session_ != nullptr && ctx.session_->is_ai_column_internal_write()) ? OB_SUCCESS : OB_NOT_SUPPORTED;
  if (OB_SUCC(ret)) {
    expr.extra_ = raw.has_ai_column_capture_marker() ? 0 : 2;
    expr.eval_func_ = eval_capture;
  }
  return ret;
}
int AiColumnCaptureExpr::eval_capture(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &result)
{
  int ret = OB_SUCCESS; ObEvalCtx::TempAllocGuard guard(ctx); ObIAllocator &allocator = guard.get_allocator();
  AiColumnOutbox outbox; ObObj old; int64_t position = 1, reset_identity = 0, key_count = 0, task_count = 0, group = 0;
  ObSEArray<ObObj, 4> key;
  if (expr.extra_ == 2 && (ctx.exec_ctx_.get_my_session() == nullptr
      || !ctx.exec_ctx_.get_my_session()->is_ai_column_internal_write())) { ret = OB_NOT_SUPPORTED; }
  else if (OB_FAIL(capture_value(expr, ctx, allocator, 0, old))) {}
  else if (OB_FAIL(capture_integer(expr, ctx, position, reset_identity))) {}
  else if (reset_identity < 0 || reset_identity > 2 || (reset_identity == 2 && expr.extra_ != 2)) { ret = OB_INVALID_ARGUMENT; }
  else if (OB_FAIL(capture_integer(expr, ctx, position, key_count))) {}
  else if (key_count <= 0 || key_count > AiColumnOutbox::MAX_ROWKEY) { ret = OB_INVALID_ARGUMENT; }
  for (int64_t i = 0; OB_SUCC(ret) && i < key_count; ++i) {
    ObObj value;
    if (OB_FAIL(capture_value(expr, ctx, allocator, position++, value))) {}
    else if (value.is_null()) { ret = OB_INVALID_ARGUMENT; }
    else { ret = key.push_back(value); }
  }
  if (OB_SUCC(ret)) {
    if (reset_identity == 1 || old.is_null()) {
      char identity[32];
      if (OB_FAIL(capture_identity(identity))) {}
      else { ret = outbox.init(allocator, ObString(sizeof(identity), identity), key); }
    } else if (!ob_is_string_type(old.get_type())) { ret = OB_INVALID_ARGUMENT; }
    else if (OB_FAIL(outbox.decode(old.get_string()))) {}
    else if (outbox.rowkey().count() != key.count()) { ret = OB_INVALID_ARGUMENT; }
    else {
      for (int64_t i = 0; OB_SUCC(ret) && i < key.count(); ++i) {
        // Exact native values, including type and collation, must identify the same row.
        if (outbox.rowkey().at(i).get_type() != key.at(i).get_type()
            || outbox.rowkey().at(i).get_collation_type() != key.at(i).get_collation_type()
            || !(outbox.rowkey().at(i) == key.at(i))) { ret = OB_INVALID_ARGUMENT; }
      }
    }
  }
  if (OB_SUCC(ret)) { ret = capture_integer(expr, ctx, position, task_count); }
  if (OB_SUCC(ret) && (task_count <= 0 || task_count > AiColumnOutbox::MAX_COLUMNS)) { ret = OB_INVALID_ARGUMENT; }
  if (OB_SUCC(ret)) { ret = outbox.start_group(group); }
  ObSEArray<uint64_t, 4> seen;
  for (int64_t i = 0; OB_SUCC(ret) && i < task_count; ++i) {
    int64_t column = 0, version = 0, retries = 0, count = 0; ObSEArray<AiColumnInput, 4> inputs;
    if (OB_FAIL(capture_integer(expr, ctx, position, column))) {}
    else if (OB_FAIL(capture_integer(expr, ctx, position, version))) {}
    else if (OB_FAIL(capture_integer(expr, ctx, position, retries))) {}
    else if (OB_FAIL(capture_integer(expr, ctx, position, count))) {}
    else if (column <= 0 || retries < 0 || retries > AiColumnOutbox::MAX_RETRIES
        || (reset_identity == 2 ? version < 0 || count != 0 : version <= 0 || count < 2 || count > 3)) {
      ret = OB_INVALID_ARGUMENT;
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < seen.count(); ++j) {
      if (seen.at(j) == static_cast<uint64_t>(column)) { ret = OB_INVALID_ARGUMENT; }
    }
    if (OB_SUCC(ret)) { ret = seen.push_back(column); }
    for (int64_t j = 0; OB_SUCC(ret) && j < count; ++j) {
      AiColumnInput input; input.argument_position_ = j + 1;
      if (OB_FAIL(capture_value(expr, ctx, allocator, position++, input.value_))) {}
      else { ret = inputs.push_back(input); }
    }
    if (OB_SUCC(ret)) {
      ret = reset_identity == 2 ? version == 0 ? outbox.erase(allocator, column)
          : outbox.enqueue_deferred(allocator, column, version, group, retries)
          : outbox.enqueue(allocator, column, version, group, retries, inputs);
    }
  }
  ObString encoded;
  if (OB_SUCC(ret) && position != expr.arg_cnt_) { ret = OB_INVALID_ARGUMENT; }
  if (OB_SUCC(ret)) { ret = outbox.encode(allocator, encoded); }
  if (OB_SUCC(ret)) { ret = ObAIFuncUtils::set_string_result(expr, ctx, result, encoded); }
  return ret;
}
}
