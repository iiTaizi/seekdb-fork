/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "sql/engine/expr/ob_expr_operator.h"
namespace oceanbase::sql {
class AiColumnStatusExpr : public ObFuncExprOperator
{
public:
  explicit AiColumnStatusExpr(common::ObIAllocator &allocator);
  int calc_result_type3(ObExprResType &type, ObExprResType &state, ObExprResType &column,
                       ObExprResType &version, common::ObExprTypeCtx &ctx) const override;
  int cg_expr(ObExprCGCtx &ctx, const ObRawExpr &raw, ObExpr &expr) const override;
  static int eval_status(const ObExpr &expr, ObEvalCtx &ctx, common::ObDatum &result);
private:
  DISALLOW_COPY_AND_ASSIGN(AiColumnStatusExpr);
};
}
