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
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include <algorithm>
#include "common/json_type/ob_json_common.h"
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  int ret = OB_SUCCESS;
  ObDatum *model_arg = nullptr;
  ObDatum *query_arg = nullptr;
  ObDatum *docs_arg = nullptr;
  ObDatum *key_arg = nullptr;
  if (expr.arg_cnt_ == 4 ? OB_FAIL(expr.eval_param_value(ctx, model_arg, query_arg, docs_arg, key_arg))
                         : OB_FAIL(expr.eval_param_value(ctx, model_arg, query_arg, docs_arg))) {
  } else if (model_arg->is_null() || query_arg->is_null() || docs_arg->is_null()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObEvalCtx::TempAllocGuard alloc_guard(ctx);
    MultimodeAlloctor allocator(alloc_guard.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObIJsonBase *json = nullptr;
    bool is_null = false;
    ObJsonArray *documents = nullptr;
    ObJsonArray *texts = nullptr;
    ObJsonArray *output = nullptr;
    const bool object_mode = nullptr != key_arg && !key_arg->is_null();
    ObString model_name = model_arg->get_string();
    ObString query;
    ObString key;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator,
            *query_arg, expr.args_[QUERY_IDX]->datum_meta_,
            expr.args_[QUERY_IDX]->obj_meta_.has_lob_header(), query))) {
    } else if (model_name.empty() || query.empty()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (object_mode && !ob_is_string_type(expr.args_[DOC_KEY_IDX]->datum_meta_.type_)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (object_mode && OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator,
            *key_arg, expr.args_[DOC_KEY_IDX]->datum_meta_,
            expr.args_[DOC_KEY_IDX]->obj_meta_.has_lob_header(), key))) {
    } else if (object_mode && key.empty()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, allocator, DOCUMENTS_IDX, json, is_null))) {
    } else if (is_null || nullptr == json || json->json_type() != ObJsonNodeType::J_ARRAY
               || json->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, texts))) {
    } else {
      documents = static_cast<ObJsonArray *>(json);
      for (int64_t i = 0; OB_SUCC(ret) && i < documents->element_count(); ++i) {
        ObJsonNode *text = documents->get_value(i);
        if (object_mode) {
          if (nullptr == text || text->json_type() != ObJsonNodeType::J_OBJECT) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            text = static_cast<ObJsonObject *>(text)->get_value(key);
          }
        }
        if (OB_FAIL(ret)) {
        } else if (nullptr == text || text->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(texts->append(text->clone(&allocator, true)))) {
        }
      }
    }
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo endpoint;
    query::ObIAiEndpointResolver *resolver =
        share::server_service<query::ObIAiEndpointResolver>();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_name, info))) {
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (nullptr == resolver) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(resolver->resolve_by_model_name(model_name, allocator, endpoint))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, output))) {
    } else {
      struct RerankEntry {
        int64_t index_;
        double score_;
        ObJsonObject *response_;
      };
      ObSEArray<RerankEntry, 20> entries;
      ObAIFuncModel model(allocator, *info, endpoint);
      constexpr int64_t batch_limit = 20;
      for (int64_t base = 0; OB_SUCC(ret) && base < texts->element_count(); base += batch_limit) {
        ObJsonArray *batch = nullptr;
        ObJsonArray *response = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, batch))) {
        }
        const int64_t end = std::min<int64_t>(base + batch_limit, texts->element_count());
        for (int64_t i = base; OB_SUCC(ret) && i < end; ++i) {
          ret = batch->append(texts->get_value(i)->clone(&allocator, true));
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(model.call_rerank(query, batch, response))) {
        } else {
          // call_rerank validates count, every local index, and all scores.
          for (int64_t i = 0; OB_SUCC(ret) && i < response->element_count(); ++i) {
            ObJsonObject *item = static_cast<ObJsonObject *>(response->get_value(i));
            RerankEntry entry;
            int64_t local_index = 0;
            entry.response_ = item;
            if (OB_FAIL(ObAIFuncUtils::get_response_index(item->get_value(INDEX_KEY), end - base, local_index))) {
            } else if (OB_FAIL(ObAIFuncUtils::get_finite_number(item->get_value(SCORE_KEY), entry.score_))) {
            } else {
              entry.index_ = base + local_index;
              ret = entries.push_back(entry);
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        std::sort(entries.begin(), entries.end(), [](const RerankEntry &a, const RerankEntry &b) {
          return a.score_ > b.score_ || (a.score_ == b.score_ && a.index_ < b.index_);
        });
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < entries.count(); ++i) {
        const RerankEntry &entry = entries.at(i);
        ObJsonObject *item = object_mode
            ? static_cast<ObJsonObject *>(documents->get_value(entry.index_)->clone(&allocator, true))
            : static_cast<ObJsonObject *>(entry.response_->clone(&allocator, true));
        if (nullptr == item) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (object_mode) {
          ObJsonNode *score = entry.response_->get_value(SCORE_KEY)->clone(&allocator, true);
          if (nullptr == score) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(item->add(MODEL_SCORE_KEY, score))) {
          }
        } else {
          ObJsonInt *index = nullptr;
          if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, entry.index_, index))) {
          } else if (OB_FAIL(item->add(INDEX_KEY, index))) {
          }
        }
        if (OB_SUCC(ret)) {
          ret = output->append(item);
        }
      }
    }
    if (OB_SUCC(ret)) {
      ObString binary;
      if (OB_FAIL(output->get_raw_binary(binary, &allocator))) {
      } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, binary))) {
      }
    }
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
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
  //   bool got_data = false;
  //   ObObj const_data;
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
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
