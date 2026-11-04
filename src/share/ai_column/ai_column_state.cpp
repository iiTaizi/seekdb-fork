/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "share/ai_column/ai_column_state.h"
#include "share/ob_errno.h"
#include <climits>
#include <cmath>

namespace oceanbase::share {
using namespace common;
OB_SERIALIZE_MEMBER(AiColumnInput, argument_position_, value_);
namespace {
template <class T>
int write_array(char *buf, int64_t length, int64_t &pos, const ObIArray<T> &values)
{
  int ret = serialization::encode_vi64(buf, length, pos, values.count());
  for (int64_t i = 0; OB_SUCC(ret) && i < values.count(); ++i) { ret = values.at(i).serialize(buf, length, pos); }
  return ret;
}
template <class T>
int read_array(const char *buf, int64_t length, int64_t &pos, int64_t limit, ObIArray<T> &values)
{
  values.reuse(); int64_t count = 0;
  int ret = serialization::decode_vi64(buf, length, pos, &count);
  if (OB_SUCC(ret) && (count < 0 || count > limit)) { ret = OB_INVALID_DATA; }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    T value;
    if (OB_FAIL(value.deserialize(buf, length, pos))) {}
    else { ret = values.push_back(value); }
  }
  return ret;
}
template <class T>
int64_t array_size(const ObIArray<T> &values)
{
  int64_t size = serialization::encoded_length_vi64(values.count());
  for (int64_t i = 0; i < values.count(); ++i) { size += values.at(i).get_serialize_size(); }
  return size;
}
}
OB_DEF_SERIALIZE(AiColumnTask)
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_ENCODE, column_id_, definition_version_, request_, group_, stage_, attempts_,
      max_retries_, lease_deadline_, lease_token_, last_error_, result_, retry_at_);
  if (OB_SUCC(ret)) { ret = write_array(buf, buf_len, pos, inputs_); }
  return ret;
}
OB_DEF_DESERIALIZE(AiColumnTask)
{
  int ret = OB_SUCCESS; reset();
  LST_DO_CODE(OB_UNIS_DECODE, column_id_, definition_version_, request_, group_, stage_, attempts_,
      max_retries_, lease_deadline_, lease_token_, last_error_, result_, retry_at_);
  if (OB_SUCC(ret)) { ret = read_array(buf, data_len, pos, AiColumnOutbox::MAX_INPUTS, inputs_); }
  if (OB_SUCC(ret) && !valid()) { ret = OB_INVALID_DATA; }
  return ret;
}
OB_DEF_SERIALIZE_SIZE(AiColumnTask)
{
  int64_t len = 0;
  LST_DO_CODE(OB_UNIS_ADD_LEN, column_id_, definition_version_, request_, group_, stage_, attempts_,
      max_retries_, lease_deadline_, lease_token_, last_error_, result_, retry_at_);
  return len + array_size(inputs_);
}
void AiColumnTask::reset()
{
  column_id_ = 0; definition_version_ = request_ = group_ = attempts_ = max_retries_ = lease_deadline_ = last_error_ = retry_at_ = 0;
  stage_ = static_cast<int64_t>(AiColumnStage::QUEUED); lease_token_.reset(); result_.set_null(); inputs_.reuse();
}
bool AiColumnTask::terminal() const
{
  return stage_ == static_cast<int64_t>(AiColumnStage::SUCCEEDED)
      || stage_ == static_cast<int64_t>(AiColumnStage::EXHAUSTED)
      || stage_ == static_cast<int64_t>(AiColumnStage::PUBLISHED);
}
AiColumnStatus AiColumnTask::status() const
{
  return stage_ != static_cast<int64_t>(AiColumnStage::PUBLISHED) ? AiColumnStatus::PENDING
      : last_error_ == OB_SUCCESS ? AiColumnStatus::READY : AiColumnStatus::FAILED;
}
bool AiColumnOutbox::durable_value(const ObObj &value)
{
  return ob_is_valid_obj_type(value.get_type()) && !value.is_ext() && !value.is_unknown()
      && value.get_type() != ObUserDefinedSQLType && !value.has_lob_header()
      && (!ob_is_double_tc(value.get_type()) || std::isfinite(value.get_double()))
      && (!ob_is_float_tc(value.get_type()) || std::isfinite(value.get_float()));
}
bool AiColumnTask::valid() const
{
  bool ok = column_id_ > 0 && definition_version_ > 0 && request_ > 0 && group_ == request_
      && max_retries_ >= 0 && max_retries_ <= AiColumnOutbox::MAX_RETRIES
      && attempts_ >= 0 && attempts_ <= max_retries_ + 1
      && stage_ >= static_cast<int64_t>(AiColumnStage::QUEUED) && stage_ <= static_cast<int64_t>(AiColumnStage::NEEDS_INPUT)
      && inputs_.count() <= AiColumnOutbox::MAX_INPUTS && AiColumnOutbox::durable_value(result_);
  const bool claimed = stage_ == static_cast<int64_t>(AiColumnStage::CLAIMED);
  ok = ok && (claimed ? attempts_ > 0 && lease_token_.length() == 32 && lease_deadline_ > 0
                     : lease_token_.empty() && lease_deadline_ == 0);
  ok = ok && (stage_ != static_cast<int64_t>(AiColumnStage::SUCCEEDED) || last_error_ == OB_SUCCESS);
  ok = ok && (stage_ != static_cast<int64_t>(AiColumnStage::EXHAUSTED) || last_error_ != OB_SUCCESS);
  ok = ok && (stage_ == static_cast<int64_t>(AiColumnStage::QUEUED) ? retry_at_ >= 0 : retry_at_ == 0);
  ok = ok && (stage_ != static_cast<int64_t>(AiColumnStage::NEEDS_INPUT)
      || (attempts_ == 0 && last_error_ == 0 && inputs_.empty() && result_.is_null()));
  for (int64_t i = 0; ok && i < inputs_.count(); ++i) {
    ok = inputs_.at(i).argument_position_ > 0 && AiColumnOutbox::durable_value(inputs_.at(i).value_)
        && (i == 0 || inputs_.at(i-1).argument_position_ < inputs_.at(i).argument_position_);
  }
  return ok;
}
void AiColumnOutbox::reset()
{
  row_identity_.reset(); rowkey_.reuse(); sequence_ = count_ = 0;
  for (auto &task : tasks_) { task.reset(); }
}
bool AiColumnOutbox::valid() const
{
  bool ok = row_identity_.length() == 32 && sequence_ >= 0 && count_ >= 0 && count_ <= MAX_COLUMNS
      && rowkey_.count() > 0 && rowkey_.count() <= MAX_ROWKEY;
  for (int64_t i = 0; ok && i < rowkey_.count(); ++i) { ok = !rowkey_.at(i).is_null() && durable_value(rowkey_.at(i)); }
  for (int64_t i = 0; ok && i < count_; ++i) {
    ok = tasks_[i].valid() && tasks_[i].request_ <= sequence_;
    for (int64_t j = 0; ok && j < i; ++j) { ok = tasks_[j].column_id_ != tasks_[i].column_id_; }
  }
  return ok;
}
int AiColumnOutbox::init(ObIAllocator &allocator, const ObString &identity, const ObIArray<ObObj> &key)
{
  reset(); int ret = OB_SUCCESS; int64_t bytes = 0;
  if (identity.length() != 32 || key.empty() || key.count() > MAX_ROWKEY) { ret = OB_INVALID_ARGUMENT; }
  else if (OB_FAIL(ob_write_string(allocator, identity, row_identity_))) {}
  for (int64_t i = 0; OB_SUCC(ret) && i < key.count(); ++i) {
    ObObj value;
    if (key.at(i).is_null() || !durable_value(key.at(i))) { ret = OB_INVALID_ARGUMENT; }
    else if ((bytes += key.at(i).get_serialize_size()) > MAX_BYTES) { ret = OB_SIZE_OVERFLOW; }
    else if (OB_FAIL(ob_write_obj(allocator, key.at(i), value))) {}
    else { ret = rowkey_.push_back(value); }
  }
  if (OB_FAIL(ret)) { reset(); } return ret;
}
const AiColumnTask *AiColumnOutbox::find(uint64_t column) const
{
  const AiColumnTask *task = nullptr;
  for (int64_t i = 0; task == nullptr && i < count_; ++i) { if (tasks_[i].column_id_ == column) { task = &tasks_[i]; } }
  return task;
}
AiColumnTask *AiColumnOutbox::mutable_task(uint64_t column)
{ return const_cast<AiColumnTask *>(find(column)); }
int AiColumnOutbox::erase(ObIAllocator &allocator, uint64_t column)
{
  int ret = OB_SUCCESS; int64_t target = -1;
  if (!valid() || column == 0) { return OB_INVALID_ARGUMENT; }
  for (int64_t i = 0; i < count_; ++i) { if (tasks_[i].column_id_ == column) { target = i; } }
  if (target < 0) { return OB_SUCCESS; }
  for (int64_t i = target; OB_SUCC(ret) && i+1 < count_; ++i) {
    // Compact through bounded serialization: copying ObSEArray must not hide
    // an allocation failure, and decoding must not refer to the overwritten slot.
    const int64_t size = tasks_[i+1].get_serialize_size(); int64_t pos = 0;
    char *buffer = static_cast<char *>(allocator.alloc(size));
    if (buffer == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else if (OB_FAIL(tasks_[i+1].serialize(buffer, size, pos))) {}
    else { pos = 0; ret = tasks_[i].deserialize(buffer, size, pos); }
  }
  if (OB_SUCC(ret)) { tasks_[--count_].reset(); }
  return ret;
}
int AiColumnOutbox::start_group(int64_t &group)
{
  int ret = OB_SUCCESS; group = 0;
  if (!valid()) { ret = OB_NOT_INIT; }
  else if (sequence_ == INT64_MAX) { ret = OB_SIZE_OVERFLOW; }
  else { group = ++sequence_; }
  return ret;
}
int AiColumnOutbox::enqueue(ObIAllocator &allocator, uint64_t column, int64_t definition,
    int64_t group, int64_t retries, const ObIArray<AiColumnInput> &inputs)
{
  int ret = OB_SUCCESS; AiColumnTask *task = mutable_task(column);
  if (!valid() || column == 0 || definition <= 0 || group <= 0 || group != sequence_
      || retries < 0 || retries > MAX_RETRIES || inputs.count() > MAX_INPUTS) { ret = OB_INVALID_ARGUMENT; }
  else if (task == nullptr && count_ == MAX_COLUMNS) { ret = OB_SIZE_OVERFLOW; }
  else {
    if (task == nullptr) { task = &tasks_[count_++]; }
    task->reset(); task->column_id_ = column; task->definition_version_ = definition;
    task->request_ = task->group_ = group; task->max_retries_ = retries;
    int64_t bytes = get_serialize_size();
    for (int64_t i = 0; OB_SUCC(ret) && i < inputs.count(); ++i) {
      AiColumnInput value; value.argument_position_ = inputs.at(i).argument_position_;
      if (!durable_value(inputs.at(i).value_) || value.argument_position_ == 0
          || (i > 0 && inputs.at(i-1).argument_position_ >= value.argument_position_)) { ret = OB_INVALID_ARGUMENT; }
      else if ((bytes += inputs.at(i).get_serialize_size()) > INPUT_BYTES) { ret = OB_SIZE_OVERFLOW; }
      else if (OB_FAIL(ob_write_obj(allocator, inputs.at(i).value_, value.value_))) {}
      else { ret = task->inputs_.push_back(value); }
    }
    if (OB_SUCC(ret) && get_serialize_size() > INPUT_BYTES) { ret = OB_SIZE_OVERFLOW; }
  }
  return ret;
}
int AiColumnOutbox::enqueue_deferred(ObIAllocator &allocator, uint64_t column, int64_t definition,
    int64_t group, int64_t retries)
{
  ObSEArray<AiColumnInput, 4> empty;
  int ret = enqueue(allocator, column, definition, group, retries, empty);
  if (OB_SUCC(ret)) { mutable_task(column)->stage_ = static_cast<int64_t>(AiColumnStage::NEEDS_INPUT); }
  return ret;
}
int AiColumnOutbox::fill_inputs(ObIAllocator &allocator, uint64_t column, int64_t definition,
    int64_t request, const ObIArray<AiColumnInput> &inputs, bool &accepted)
{
  int ret = OB_SUCCESS; accepted = false; AiColumnTask *task = mutable_task(column);
  if (!valid() || inputs.empty() || inputs.count() > MAX_INPUTS) { ret = OB_INVALID_ARGUMENT; }
  else if (task == nullptr || task->stage_ != static_cast<int64_t>(AiColumnStage::NEEDS_INPUT)
      || task->definition_version_ != definition || task->request_ != request) {}
  else {
    ObSEArray<AiColumnInput, 4> owned; int64_t bytes = get_serialize_size();
    for (int64_t i = 0; OB_SUCC(ret) && i < inputs.count(); ++i) {
      AiColumnInput value; value.argument_position_ = inputs.at(i).argument_position_;
      if (!durable_value(inputs.at(i).value_) || value.argument_position_ == 0
          || (i > 0 && inputs.at(i-1).argument_position_ >= value.argument_position_)) { ret = OB_INVALID_ARGUMENT; }
      else if ((bytes += inputs.at(i).get_serialize_size()) > INPUT_BYTES) { ret = OB_SIZE_OVERFLOW; }
      else if (OB_FAIL(ob_write_obj(allocator, inputs.at(i).value_, value.value_))) {}
      else { ret = owned.push_back(value); }
    }
    if (OB_SUCC(ret)) { ret = task->inputs_.assign(owned); }
    if (OB_SUCC(ret)) { task->stage_ = static_cast<int64_t>(AiColumnStage::QUEUED); accepted = true; }
  }
  return ret;
}
int AiColumnOutbox::claim(ObIAllocator &allocator, uint64_t column, int64_t now, int64_t lease_us,
    const ObString &token, AiColumnClaim &claim, bool &acquired)
{
  int ret = OB_SUCCESS; acquired = false; claim = AiColumnClaim(); AiColumnTask *task = mutable_task(column);
  if (!valid() || task == nullptr || now <= 0 || lease_us <= 0 || lease_us > INT64_MAX-now || token.length() != 32) {
    ret = OB_INVALID_ARGUMENT;
  } else if (task->terminal() || task->stage_ == static_cast<int64_t>(AiColumnStage::NEEDS_INPUT)
      || (task->stage_ == static_cast<int64_t>(AiColumnStage::QUEUED) && task->retry_at_ > now)
      || (task->stage_ == static_cast<int64_t>(AiColumnStage::CLAIMED) && task->lease_deadline_ > now)) {
    // A failed CAS at the caller must also discard the returned claim.
  } else if (task->attempts_ >= task->max_retries_ + 1) {
    task->stage_ = static_cast<int64_t>(AiColumnStage::EXHAUSTED); task->last_error_ = OB_TIMEOUT;
    task->lease_deadline_ = 0; task->lease_token_.reset();
  } else {
    ObString owned, identity;
    if (OB_FAIL(ob_write_string(allocator, token, owned))) {}
    else if (OB_FAIL(ob_write_string(allocator, row_identity_, identity))) {}
    else {
      task->lease_token_ = owned; task->lease_deadline_ = now+lease_us; ++task->attempts_;
      task->stage_ = static_cast<int64_t>(AiColumnStage::CLAIMED); task->retry_at_ = 0;
      claim.row_identity_ = identity; claim.column_id_ = column; claim.request_ = task->request_;
      claim.definition_version_ = task->definition_version_; claim.attempt_ = task->attempts_;
      claim.lease_token_ = owned; acquired = true;
    }
  }
  return ret;
}
int AiColumnOutbox::finish(ObIAllocator &allocator, const AiColumnClaim &claim, int64_t now,
    int error, const ObObj &result, bool &accepted, int64_t retry_delay_us)
{
  int ret = OB_SUCCESS; accepted = false; AiColumnTask *task = mutable_task(claim.column_id_);
  if (!valid() || now <= 0 || retry_delay_us < 0 || retry_delay_us > INT64_MAX-now) { ret = OB_INVALID_ARGUMENT; }
  else if (task == nullptr || row_identity_ != claim.row_identity_ || task->request_ != claim.request_
      || task->definition_version_ != claim.definition_version_ || task->attempts_ != claim.attempt_
      || task->stage_ != static_cast<int64_t>(AiColumnStage::CLAIMED)
      || task->lease_token_ != claim.lease_token_ || task->lease_deadline_ <= now) {
    // A late, superseded, expired or foreign result is not an SQL error.
  } else if (error == OB_SUCCESS && !durable_value(result)) { ret = OB_INVALID_ARGUMENT; }
  else if (error == OB_SUCCESS && result.get_serialize_size() > MAX_BYTES) { ret = OB_SIZE_OVERFLOW; }
  else {
    ObObj owned;
    if (error == OB_SUCCESS && OB_FAIL(ob_write_obj(allocator, result, owned))) {}
    else {
      task->result_ = owned; task->last_error_ = error;
      task->stage_ = static_cast<int64_t>(error == OB_SUCCESS ? AiColumnStage::SUCCEEDED
          : task->attempts_ < task->max_retries_+1 ? AiColumnStage::QUEUED : AiColumnStage::EXHAUSTED);
      task->lease_token_.reset(); task->lease_deadline_ = 0; accepted = true;
      task->retry_at_ = task->stage_ == static_cast<int64_t>(AiColumnStage::QUEUED) && retry_delay_us > 0
          ? now+retry_delay_us : 0;
      if (get_serialize_size() > MAX_BYTES) { ret = OB_SIZE_OVERFLOW; accepted = false; }
    }
  }
  return ret;
}
bool AiColumnOutbox::group_ready(int64_t group) const
{
  bool found = false;
  for (int64_t i = 0; i < count_; ++i) {
    if (tasks_[i].group_ == group) { found = true; if (!tasks_[i].terminal()) { return false; } }
  }
  return found;
}
int AiColumnOutbox::publish(int64_t group)
{
  int ret = OB_SUCCESS;
  if (!valid() || !group_ready(group)) { ret = OB_STATE_NOT_MATCH; }
  else { for (int64_t i = 0; i < count_; ++i) {
    if (tasks_[i].group_ == group) {
      tasks_[i].stage_ = static_cast<int64_t>(AiColumnStage::PUBLISHED);
      tasks_[i].inputs_.reuse(); tasks_[i].result_.set_null();
    }
  } }
  return ret;
}
OB_DEF_SERIALIZE(AiColumnOutbox)
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_ENCODE, row_identity_, sequence_, count_);
  if (OB_SUCC(ret)) { ret = write_array(buf, buf_len, pos, rowkey_); }
  for (int64_t i = 0; OB_SUCC(ret) && i < count_; ++i) { OB_UNIS_ENCODE(tasks_[i]); }
  return ret;
}
OB_DEF_DESERIALIZE(AiColumnOutbox)
{
  int ret = OB_SUCCESS; reset();
  LST_DO_CODE(OB_UNIS_DECODE, row_identity_, sequence_, count_);
  if (OB_SUCC(ret) && (count_ < 0 || count_ > MAX_COLUMNS || data_len > MAX_BYTES)) { ret = OB_INVALID_DATA; }
  if (OB_SUCC(ret)) { ret = read_array(buf, data_len, pos, MAX_ROWKEY, rowkey_); }
  for (int64_t i = 0; OB_SUCC(ret) && i < count_; ++i) { OB_UNIS_DECODE(tasks_[i]); }
  if (OB_SUCC(ret) && !valid()) { ret = OB_INVALID_DATA; }
  if (OB_FAIL(ret)) { reset(); } return ret;
}
OB_DEF_SERIALIZE_SIZE(AiColumnOutbox)
{
  int64_t len = 0; LST_DO_CODE(OB_UNIS_ADD_LEN, row_identity_, sequence_, count_);
  len += array_size(rowkey_);
  for (int64_t i = 0; i < count_; ++i) { OB_UNIS_ADD_LEN(tasks_[i]); }
  return len;
}
int AiColumnOutbox::encode(ObIAllocator &allocator, ObString &encoded) const
{
  encoded.reset(); int ret = OB_SUCCESS; const int64_t size = get_serialize_size();
  if (!valid()) { ret = OB_INVALID_DATA; }
  else if (size > MAX_BYTES) { ret = OB_SIZE_OVERFLOW; }
  else {
    char *data = static_cast<char *>(allocator.alloc(size)); int64_t pos = 0;
    if (data == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else if (OB_FAIL(serialize(data, size, pos))) {}
    else { encoded.assign_ptr(data, static_cast<int32_t>(pos)); }
  }
  return ret;
}
int AiColumnOutbox::decode(const ObString &encoded)
{
  reset(); int64_t pos = 0;
  int ret = encoded.empty() || encoded.length() > MAX_BYTES ? OB_INVALID_DATA
      : deserialize(encoded.ptr(), encoded.length(), pos);
  if (OB_SUCC(ret) && pos != encoded.length()) { ret = OB_INVALID_DATA; }
  if (OB_FAIL(ret)) { reset(); } return ret;
}
}
