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

#include "ob_temp_table_transformation_op.h"
#include "share/rc/ob_server_runtime.h"

namespace oceanbase
{
using namespace common;
using namespace storage;
using namespace share;
using namespace share::schema;
namespace sql
{
DEF_TO_STRING(ObTempTableTransformationOpSpec)
{
  int64_t pos = 0;
  J_OBJ_START();
  J_NAME("op_spec");
  J_COLON();
  pos += ObOpSpec::to_string(buf + pos, buf_len - pos);
  J_COMMA();
  J_OBJ_END();
  return pos;
}

OB_SERIALIZE_MEMBER_INHERIT(ObTempTableTransformationOpSpec, ObOpSpec, ai_column_strict_read_);

int ObTempTableTransformationOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else { /*do nothing.*/ }
  return ret;
}

OperatorOpenOrder ObTempTableTransformationOp::get_operator_open_order() const
{
  return static_cast<const ObTempTableTransformationOpSpec &>(spec_).ai_column_strict_read_
      ? OPEN_SELF_ONLY : OPEN_CHILDREN_FIRST;
}

int ObTempTableTransformationOp::drain_ai_controls()
{
  int ret = OB_SUCCESS;
  for (int64_t i=0; OB_SUCC(ret) && i<get_child_cnt()-1; ++i) {
    ObOperator *control = children_[i];
    if (control == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (control->get_spec().is_vectorized()) {
      bool end = false;
      while (OB_SUCC(ret) && !end) {
        const ObBatchRows *rows = nullptr;
        if (OB_FAIL(check_status())) {}
        else if (OB_FAIL(control->get_next_batch(256, rows))) {}
        else if (rows == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else {
          // Force outputs even if a future child uses lazy projection.
          for (int64_t j=0; OB_SUCC(ret) && rows->size_>0 && j<control->get_spec().output_.count(); ++j) {
            ret = control->get_spec().output_.at(j)->eval_batch(control->get_eval_ctx(), *rows->skip_, rows->size_);
          }
          end = rows->end_;
        }
      }
    } else {
      while (OB_SUCC(ret)) {
        if (OB_FAIL(check_status())) {}
        else if (OB_FAIL(control->get_next_row())) {}
        else {
          for (int64_t j=0; OB_SUCC(ret) && j<control->get_spec().output_.count(); ++j) {
            ObDatum *value = nullptr;
            ret = control->get_spec().output_.at(j)->eval(control->get_eval_ctx(), value);
          }
        }
      }
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    }
  }
  return ret;
}

int ObTempTableTransformationOp::inner_open()
{
  int ret = OB_SUCCESS;
  if (static_cast<const ObTempTableTransformationOpSpec &>(spec_).ai_column_strict_read_) {
    init_temp_table_ = false;
    if (get_child_cnt()<2) { ret = OB_ERR_UNEXPECTED; }
    for (int64_t i=0; OB_SUCC(ret) && i<get_child_cnt()-1; ++i) {
      ai_opened_children_ = i+1; ret = children_[i]->open();
    }
    if (OB_SUCC(ret)) { ret = drain_ai_controls(); }
    if (OB_SUCC(ret)) {
      ai_opened_children_ = get_child_cnt(); ret = children_[get_child_cnt()-1]->open();
    }
  }
  return ret;
}

int ObTempTableTransformationOp::rescan()
{
  if (!static_cast<const ObTempTableTransformationOpSpec &>(spec_).ai_column_strict_read_) {
    return ObOperator::rescan();
  }
  int ret = OB_SUCCESS;
  for (int64_t i=0; OB_SUCC(ret) && i<get_child_cnt()-1; ++i) { ret = children_[i]->rescan(); }
  if (OB_SUCC(ret)) { ret = drain_ai_controls(); }
  if (OB_SUCC(ret)) { ret = children_[get_child_cnt()-1]->rescan(); }
  if (OB_SUCC(ret)) { ret = ObOperator::inner_rescan(); }
  return ret;
}

int ObTempTableTransformationOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  ObExecContext &ctx = get_exec_ctx();
  if (init_temp_table_) {
    for (int64_t i = 0; OB_SUCC(ret) && i < get_child_cnt() - 1; ++i) {
      int64_t temp_table_count = ctx.get_temp_table_ctx().count();
      if (OB_ISNULL(children_[i])) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FALSE_IT(ret = children_[i]->get_next_row())) {
      } else if (ret != OB_ITER_END) {
      } else {
        ret = OB_SUCCESS;
        while(OB_SUCC(ret) && ctx.get_temp_table_ctx().count() <= temp_table_count) {
          if (OB_FAIL(check_status())) {
          } else {
            ob_usleep(1000);
          }
        }
      }
    }
    init_temp_table_ = false;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(children_[get_child_cnt() - 1])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(children_[get_child_cnt() - 1]->get_next_row())) {
    if (ret != OB_ITER_END) {
    } else { /*do nothing.*/ }
  } else { /*do nothing.*/ }
  return ret;
}

int ObTempTableTransformationOp::inner_get_next_batch(const int64_t max_row_cnt)
{
  int ret = OB_SUCCESS;
  const ObBatchRows *child_brs = nullptr;
  clear_evaluated_flag();
  clear_datum_eval_flag();
  if (init_temp_table_) {
    for (int64_t i = 0; OB_SUCC(ret) && i < get_child_cnt() - 1; ++i) {
      if (OB_ISNULL(children_[i])) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(children_[i]->get_next_batch(max_row_cnt, child_brs))) {
      }
    }
    init_temp_table_ = false;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(children_[get_child_cnt() - 1])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(children_[get_child_cnt() - 1]->get_next_batch(
                 max_row_cnt, child_brs))) {
  } else { /*do nothing.*/
  }
  if (OB_SUCC(ret) && child_brs != nullptr) { (void)brs_.copy(child_brs); }
  return ret;
}

int ObTempTableTransformationOp::inner_close()
{
  int ret = OB_SUCCESS;
  if (static_cast<const ObTempTableTransformationOpSpec &>(spec_).ai_column_strict_read_) {
    for (int64_t i=ai_opened_children_-1; i>=0; --i) {
      const int code = children_[i]->close();
      if (OB_SUCC(ret) && code!=OB_SUCCESS) { ret = code; }
    }
    ai_opened_children_ = 0;
  } else if (OB_FAIL(destory_interm_results())) {
  }
  return ret;
}

int ObTempTableTransformationOp::destory_interm_results()
{
  int ret = OB_SUCCESS;
  ObExecContext &ctx = get_exec_ctx();
  const int64_t temp_table_count = ctx.get_temp_table_ctx().count();
  for (int64_t i = 0; OB_SUCC(ret) && i < temp_table_count; ++i) {
    ObSqlTempTableCtx &temp_table_ctx = ctx.get_temp_table_ctx().at(i);
    for (int64_t j = 0; OB_SUCC(ret) && j < temp_table_ctx.interm_result_infos_.count(); ++j) {
      ObTempTableResultInfo &result_info = temp_table_ctx.interm_result_infos_.at(j);
      if (OB_FAIL(destory_local_interm_results(result_info.interm_result_ids_))) {
      }
    }
  }
  return ret;
}


int ObTempTableTransformationOp::destory_local_interm_results(ObIArray<uint64_t> &result_ids)
{
  int ret = OB_SUCCESS;
  dtl::ObDTLIntermResultKey dtl_int_key;
  LOG_TRACE("destory interm results", K(get_exec_ctx().get_addr()), K(result_ids));
  for (int64_t i = 0; OB_SUCC(ret) && i < result_ids.count(); ++i) {
    dtl_int_key.channel_id_ = result_ids.at(i);
    if (OB_FAIL(::oceanbase::share::server_service<::oceanbase::sql::dtl::ObDTLIntermResultManager>()->erase_interm_result_info(
                                                                            dtl_int_key))) {
      if (OB_HASH_NOT_EXIST == ret) {
        ret = OB_SUCCESS;
      } else {
      }
    }
  }
  return ret;
}

void ObTempTableTransformationOp::destroy()
{
  ObOperator::destroy();
}

} // end namespace sql
} // end namespace oceanbase
