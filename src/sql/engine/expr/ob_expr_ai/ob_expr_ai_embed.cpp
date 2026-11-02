/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include <climits>
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (!ob_is_string_type(types_stack[CONTENT_IDX].get_type())) {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    }
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        // Preserve signedness so oversized unsigned dimensions cannot wrap.
        types_stack[DIM_IDX].set_calc_type(types_stack[DIM_IDX].get_type());
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::read_input(const ObExpr &expr, ObEvalCtx &ctx, MultimodeAlloctor &allocator,
    ObString &model, ObString &content, int64_t &dimension, bool &is_null)
{
  int ret = OB_SUCCESS;
  ObDatum *model_arg = nullptr;
  ObDatum *content_arg = nullptr;
  ObDatum *dim_arg = nullptr;
  is_null = false;
  dimension = 0;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, model_arg, content_arg, dim_arg))
                         : OB_FAIL(expr.eval_param_value(ctx, model_arg, content_arg))) {
  } else if (model_arg->is_null()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (content_arg->is_null()) {
    is_null = true;
  } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator,
                 *content_arg, expr.args_[CONTENT_IDX]->datum_meta_,
                 expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
  } else {
    model = model_arg->get_string();
    if (model.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (nullptr != dim_arg) {
      if (dim_arg->is_null()) {
        ret = OB_INVALID_ARGUMENT;
      } else if (ob_is_unsigned_type(expr.args_[DIM_IDX]->datum_meta_.type_)) {
        if (dim_arg->get_uint() == 0 || dim_arg->get_uint() > static_cast<uint64_t>(INT64_MAX)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          dimension = dim_arg->get_uint();
        }
      } else if ((dimension = dim_arg->get_int()) <= 0) {
        ret = OB_INVALID_ARGUMENT;
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::call_embeddings(MultimodeAlloctor &allocator, ObString &model_name,
    ObArray<ObString> &contents, int64_t dimension, ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  ObJsonObject *config = nullptr;
  share::ObAiModelEndpointInfo endpoint;
  query::ObIAiEndpointResolver *resolver = share::server_service<query::ObIAiEndpointResolver>();
  if (dimension > 0) {
    ObJsonInt *dim = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dimension, dim))) {
    } else if (OB_FAIL(config->add("dimensions", dim))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_name, info))) {
  } else if (nullptr == resolver) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(resolver->resolve_by_model_name(model_name, allocator, endpoint))) {
  } else {
    ObAIFuncModel model(allocator, *info, endpoint);
    ret = model.call_dense_embedding_vector_v2(contents, config, results);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  int ret = OB_SUCCESS;
  ObEvalCtx::TempAllocGuard alloc_guard(ctx);
  MultimodeAlloctor allocator(alloc_guard.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
  ObString model;
  ObString content;
  int64_t dimension = 0;
  bool is_null = false;
  ObArray<ObString> contents;
  ObArray<ObString> results;
  if (OB_FAIL(read_input(expr, ctx, allocator, model, content, dimension, is_null))) {
  } else if (is_null) {
    res.set_null();
  } else if (OB_FAIL(contents.push_back(content))) {
  } else if (OB_FAIL(call_embeddings(allocator, model, contents, dimension, results))) {
  } else if (results.count() != 1) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, results.at(0)))) {
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
    const ObBitVector &skip, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  if (batch_size <= 0) {
    return ret;
  }
  ObEvalCtx::TempAllocGuard alloc_guard(ctx);
  MultimodeAlloctor allocator(alloc_guard.get_allocator());
  ObEvalCtx::BatchInfoScopeGuard batch_guard(ctx);
  batch_guard.set_batch_size(batch_size);
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
  ObBitVector &evaluated = expr.get_evaluated_flags(ctx);
  void *bits = allocator.alloc(ObBitVector::memory_size(batch_size));
  ObBitVector *inactive = to_bit_vector(bits);
  struct EmbedRow {
    ObString model_;
    ObString content_;
    ObString result_;
    int64_t dimension_ = 0;
    bool is_null_ = false;
    bool grouped_ = false;
  };
  ObSEArray<EmbedRow, 16> rows;
  if (nullptr == bits) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    inactive->bit_calculate(skip, evaluated, batch_size,
        [](const uint64_t a, const uint64_t b) { return a | b; });
    if (inactive->accumulate_bit_cnt(batch_size) == batch_size) {
      return OB_SUCCESS;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
      ret = rows.push_back(EmbedRow());
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < expr.arg_cnt_; ++i) {
      ret = expr.args_[i]->eval_batch(ctx, *inactive, batch_size);
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
      if (!inactive->at(i)) {
        batch_guard.set_batch_idx(i);
        EmbedRow &row = rows.at(i);
        ret = read_input(expr, ctx, allocator, row.model_, row.content_, row.dimension_, row.is_null_);
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
      EmbedRow &first = rows.at(i);
      if (inactive->at(i) || first.is_null_ || first.grouped_) {
        continue;
      }
      ObArray<ObString> contents;
      ObArray<ObString> results;
      ObSEArray<int64_t, 16> mapping;
      for (int64_t j = i; OB_SUCC(ret) && j < batch_size; ++j) {
        EmbedRow &row = rows.at(j);
        if (!inactive->at(j) && !row.is_null_ && !row.grouped_
            && row.model_ == first.model_ && row.dimension_ == first.dimension_) {
          if (OB_FAIL(contents.push_back(row.content_))) {
          } else if (OB_FAIL(mapping.push_back(j))) {
          } else {
            row.grouped_ = true;
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(call_embeddings(allocator, first.model_, contents, first.dimension_, results))) {
      } else if (results.count() != mapping.count()) {
        ret = OB_INVALID_DATA;
      } else {
        for (int64_t j = 0; j < mapping.count(); ++j) {
          rows.at(mapping.at(j)).result_ = results.at(j);
        }
      }
    }
    // Publish only after every HTTP group and response validation succeeded.
    ObDatumVector output = expr.locate_expr_datumvector(ctx);
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
      if (!inactive->at(i)) {
        batch_guard.set_batch_idx(i);
        if (rows.at(i).is_null_) {
          output.at(i)->set_null();
        } else {
          ret = ObAIFuncUtils::set_string_result(expr, ctx, *output.at(i), rows.at(i).result_);
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; i < batch_size; ++i) {
        if (!inactive->at(i)) {
          evaluated.set(i);
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
