/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "sql/engine/expr/ob_expr_operator.h"

namespace oceanbase::sql {
// Constructed only by the native DML resolver. This expression never calls AI.
class AiColumnCaptureExpr : public ObFuncExprOperator
{
public:
  explicit AiColumnCaptureExpr(common::ObIAllocator &allocator);
  int calc_result_typeN(ObExprResType &type, ObExprResType *types, int64_t count,
                       common::ObExprTypeCtx &ctx) const override;
  int cg_expr(ObExprCGCtx &ctx, const ObRawExpr &raw, ObExpr &expr) const override;
  static int eval_capture(const ObExpr &expr, ObEvalCtx &ctx, common::ObDatum &result);
private:
  DISALLOW_COPY_AND_ASSIGN(AiColumnCaptureExpr);
};
}
