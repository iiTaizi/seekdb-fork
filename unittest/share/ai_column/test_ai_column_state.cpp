/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include "share/ai_column/ai_column_state.h"
#include "share/ai_column/ai_column_definition.h"
#include "lib/allocator/ob_allocator.h"
#include "share/ob_errno.h"

using namespace oceanbase::common;
using namespace oceanbase::share;
namespace {
constexpr const char *ROW = "0123456789abcdef0123456789abcdef";
constexpr const char *OWNER_A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char *OWNER_B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
class AiColumnState : public ::testing::Test
{
public:
  void SetUp() override {
    ObSEArray<ObObj, 4> key; ObObj value; value.set_uint64(UINT64_MAX);
    ASSERT_EQ(OB_SUCCESS, key.push_back(value));
    ASSERT_EQ(OB_SUCCESS, state_.init(allocator_, ROW, key));
  }
  void enqueue(uint64_t column, int64_t group, int64_t retries = 0, int64_t version = 7) {
    ObSEArray<AiColumnInput, 4> inputs; AiColumnInput value;
    value.argument_position_ = 18; value.value_.set_varchar("中文 'quoted'");
    value.value_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, inputs.push_back(value));
    ASSERT_EQ(OB_SUCCESS, state_.enqueue(allocator_, column, version, group, retries, inputs));
  }
  AiColumnClaim claim(uint64_t column, int64_t now = 100, const char *owner = OWNER_A) {
    AiColumnClaim result; bool acquired = false;
    EXPECT_EQ(OB_SUCCESS, state_.claim(allocator_, column, now, 100, owner, result, acquired));
    EXPECT_TRUE(acquired); return result;
  }
  void finish(const AiColumnClaim &claim, int error = OB_SUCCESS, bool expected = true, int64_t now = 150) {
    ObObj value; value.set_null(); bool accepted = false;
    EXPECT_EQ(OB_SUCCESS, state_.finish(allocator_, claim, now, error, value, accepted));
    EXPECT_EQ(expected, accepted);
  }
  ObArenaAllocator allocator_{"AiColumnTest"};
  AiColumnOutbox state_;
};

TEST_F(AiColumnState, typed_snapshot_and_successful_null_survive_encoding)
{
  int64_t group = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(group)); enqueue(20, group);
  ObString pending; ASSERT_EQ(OB_SUCCESS, state_.encode(allocator_, pending));
  AiColumnOutbox pending_restored; ASSERT_EQ(OB_SUCCESS, pending_restored.decode(pending));
  EXPECT_EQ(ObString("中文 'quoted'"), pending_restored.find(20)->inputs_.at(0).value_.get_string());
  EXPECT_EQ(CS_TYPE_UTF8MB4_BIN, pending_restored.find(20)->inputs_.at(0).value_.get_collation_type());
  finish(claim(20)); EXPECT_EQ(AiColumnStatus::PENDING, state_.find(20)->status());
  ASSERT_EQ(OB_SUCCESS, state_.publish(group)); EXPECT_EQ(AiColumnStatus::READY, state_.find(20)->status());
  ObString data; ASSERT_EQ(OB_SUCCESS, state_.encode(allocator_, data));
  AiColumnOutbox restored; ASSERT_EQ(OB_SUCCESS, restored.decode(data));
  EXPECT_EQ(UINT64_MAX, restored.rowkey().at(0).get_uint64());
  EXPECT_EQ(AiColumnStatus::READY, restored.find(20)->status()); EXPECT_TRUE(restored.find(20)->result_.is_null());
  EXPECT_EQ(0, restored.find(20)->inputs_.count());
}
TEST_F(AiColumnState, group_waits_for_failure_and_publishes_mixed_outcomes)
{
  int64_t group = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(group)); enqueue(20, group); enqueue(21, group);
  finish(claim(20)); EXPECT_FALSE(state_.group_ready(group));
  EXPECT_EQ(OB_STATE_NOT_MATCH, state_.publish(group));
  finish(claim(21), OB_TIMEOUT); EXPECT_TRUE(state_.group_ready(group));
  EXPECT_EQ(AiColumnStatus::PENDING, state_.find(20)->status());
  ASSERT_EQ(OB_SUCCESS, state_.publish(group));
  EXPECT_EQ(AiColumnStatus::READY, state_.find(20)->status()); EXPECT_EQ(AiColumnStatus::FAILED, state_.find(21)->status());
}
TEST_F(AiColumnState, changed_request_and_definition_reject_old_results)
{
  int64_t group = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(group)); enqueue(20, group);
  AiColumnClaim old = claim(20);
  ASSERT_EQ(OB_SUCCESS, state_.start_group(group)); enqueue(20, group, 0, 8);
  finish(old, OB_SUCCESS, false); EXPECT_EQ(AiColumnStatus::PENDING, state_.find(20)->status());
  AiColumnClaim current = claim(20); AiColumnClaim wrong_version = current; wrong_version.definition_version_ = 7;
  finish(wrong_version, OB_SUCCESS, false); finish(current); ASSERT_EQ(OB_SUCCESS, state_.publish(group));
}
TEST_F(AiColumnState, replaced_member_does_not_block_still_valid_old_group)
{
  int64_t first = 0, second = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(first)); enqueue(20, first); enqueue(21, first);
  AiColumnClaim old = claim(20); AiColumnClaim other = claim(21);
  ASSERT_EQ(OB_SUCCESS, state_.start_group(second)); enqueue(20, second);
  finish(old, OB_SUCCESS, false); finish(other);
  EXPECT_TRUE(state_.group_ready(first)); EXPECT_FALSE(state_.group_ready(second));
  ASSERT_EQ(OB_SUCCESS, state_.publish(first));
  EXPECT_EQ(AiColumnStatus::PENDING, state_.find(20)->status()); EXPECT_EQ(AiColumnStatus::READY, state_.find(21)->status());
}
TEST_F(AiColumnState, crash_recovery_changes_lease_and_enforces_attempt_limit)
{
  int64_t group = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(group)); enqueue(20, group, 1);
  AiColumnClaim old = claim(20); bool acquired = true; AiColumnClaim blocked;
  ASSERT_EQ(OB_SUCCESS, state_.claim(allocator_, 20, 150, 100, OWNER_B, blocked, acquired)); EXPECT_FALSE(acquired);
  AiColumnClaim recovered = claim(20, 200, OWNER_B);
  finish(old, OB_SUCCESS, false, 201); finish(recovered, OB_TIMEOUT, true, 250);
  EXPECT_TRUE(state_.group_ready(group));
  ASSERT_EQ(OB_SUCCESS, state_.claim(allocator_, 20, 300, 100, OWNER_A, blocked, acquired)); EXPECT_FALSE(acquired);
  ASSERT_EQ(OB_SUCCESS, state_.publish(group)); EXPECT_EQ(2, state_.find(20)->attempts_);
  EXPECT_EQ(AiColumnStatus::FAILED, state_.find(20)->status());
}
TEST_F(AiColumnState, final_expired_lease_becomes_terminal_without_new_inference)
{
  int64_t group = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(group)); enqueue(20, group);
  AiColumnClaim old = claim(20); finish(old, OB_SUCCESS, false, 200);
  AiColumnClaim unused; bool acquired = true;
  ASSERT_EQ(OB_SUCCESS, state_.claim(allocator_, 20, 200, 100, OWNER_B, unused, acquired)); EXPECT_FALSE(acquired);
  EXPECT_TRUE(state_.group_ready(group)); ASSERT_EQ(OB_SUCCESS, state_.publish(group));
  EXPECT_EQ(AiColumnStatus::FAILED, state_.find(20)->status());
}
TEST_F(AiColumnState, retries_are_bounded_and_manual_retry_can_regenerate_ready)
{
  int64_t group = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(group)); enqueue(20, group, 1);
  finish(claim(20), OB_TIMEOUT); EXPECT_FALSE(state_.group_ready(group));
  finish(claim(20)); ASSERT_EQ(OB_SUCCESS, state_.publish(group));
  ASSERT_EQ(OB_SUCCESS, state_.start_group(group)); enqueue(20, group);
  EXPECT_EQ(0, state_.find(20)->attempts_); EXPECT_EQ(AiColumnStatus::PENDING, state_.find(20)->status());
}
TEST_F(AiColumnState, delete_reinsert_identity_rejects_old_claim_with_same_key_and_sequence)
{
  int64_t group = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(group)); enqueue(20, group); AiColumnClaim old = claim(20);
  ObSEArray<ObObj, 4> key; ObObj value; value.set_uint64(UINT64_MAX); ASSERT_EQ(OB_SUCCESS, key.push_back(value));
  ASSERT_EQ(OB_SUCCESS, state_.init(allocator_, OWNER_B, key));
  ASSERT_EQ(OB_SUCCESS, state_.start_group(group)); enqueue(20, group); AiColumnClaim current = claim(20);
  finish(old, OB_SUCCESS, false); finish(current);
}
TEST_F(AiColumnState, malformed_encoding_resets_state_and_rejects_trailing_bytes)
{
  int64_t group = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(group)); enqueue(20, group);
  ObString data; ASSERT_EQ(OB_SUCCESS, state_.encode(allocator_, data));
  for (int64_t i = 0; i < data.length(); ++i) {
    AiColumnOutbox broken; EXPECT_NE(OB_SUCCESS, broken.decode(ObString(i, data.ptr()))); EXPECT_EQ(0, broken.count());
  }
  std::string trailing(data.ptr(), data.length()); trailing.push_back('x');
  AiColumnOutbox broken; EXPECT_NE(OB_SUCCESS, broken.decode(ObString(trailing.size(), trailing.data())));
  EXPECT_FALSE(broken.valid()); EXPECT_EQ(0, broken.count());
}
TEST_F(AiColumnState, rejects_pointer_values_nonfinite_values_and_oversized_snapshots)
{
  int64_t group = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(group));
  ObSEArray<AiColumnInput, 4> values; AiColumnInput value; value.argument_position_ = 18;
  value.value_.set_double(std::numeric_limits<double>::infinity()); ASSERT_EQ(OB_SUCCESS, values.push_back(value));
  EXPECT_EQ(OB_INVALID_ARGUMENT, state_.enqueue(allocator_, 20, 7, group, 0, values));
  value.value_.set_ext(123); EXPECT_FALSE(AiColumnOutbox::durable_value(value.value_));
  std::string huge(AiColumnOutbox::MAX_BYTES, 'x'); values.reuse(); value.value_.set_varchar(ObString(huge.size(), huge.data()));
  ASSERT_EQ(OB_SUCCESS, values.push_back(value));
  // Failed mutations must be discarded rather than persisted by the caller.
  ObSEArray<ObObj, 4> key; ObObj rowkey; rowkey.set_int(1); ASSERT_EQ(OB_SUCCESS, key.push_back(rowkey));
  ASSERT_EQ(OB_SUCCESS, state_.init(allocator_, ROW, key));
  ASSERT_EQ(OB_SUCCESS, state_.start_group(group));
  EXPECT_EQ(OB_SIZE_OVERFLOW, state_.enqueue(allocator_, 20, 7, group, 0, values));
}
TEST_F(AiColumnState, decoder_rejects_input_count_before_reading_elements)
{
  AiColumnTask task; task.column_id_ = 20; task.definition_version_ = 7; task.request_ = task.group_ = 1;
  for (int64_t i = 0; i <= AiColumnOutbox::MAX_INPUTS; ++i) {
    AiColumnInput value; value.argument_position_ = i+1; value.value_.set_int(i);
    ASSERT_EQ(OB_SUCCESS, task.inputs_.push_back(value));
  }
  std::string data(task.get_serialize_size(), '\0'); int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, task.serialize(data.data(), data.size(), pos));
  AiColumnTask restored; pos = 0;
  EXPECT_EQ(OB_INVALID_DATA, restored.deserialize(data.data(), data.size(), pos));
  EXPECT_EQ(0, restored.inputs_.count());
}
TEST_F(AiColumnState, rejects_capacity_and_lease_overflow_without_acquiring)
{
  int64_t group = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(group));
  for (int64_t i = 0; i < AiColumnOutbox::MAX_COLUMNS; ++i) { enqueue(20+i, group); }
  ObSEArray<AiColumnInput, 4> inputs;
  EXPECT_EQ(OB_SIZE_OVERFLOW, state_.enqueue(allocator_, 100, 7, group, 0, inputs));
  AiColumnClaim result; bool acquired = true;
  EXPECT_EQ(OB_INVALID_ARGUMENT, state_.claim(allocator_, 20, INT64_MAX, 100, OWNER_A, result, acquired));
  EXPECT_FALSE(acquired); EXPECT_EQ(0, state_.find(20)->attempts_);
}
TEST_F(AiColumnState, definition_metadata_is_ascii_and_preserves_expression_and_options)
{
  AiColumnDefinition definition; definition.expression_ = "AI_COMPLETE('model', CONCAT('标签：', content))";
  definition.max_retries_ = 2; definition.sql_mode_ = 512; definition.definer_id_ = 200001;
  ASSERT_EQ(OB_SUCCESS, definition.dependencies_.push_back(18));
  ObString encoded; ASSERT_EQ(OB_SUCCESS, definition.encode(allocator_, encoded));
  for (int64_t i = 0; i < encoded.length(); ++i) { EXPECT_GT(encoded[i], 0); EXPECT_LT(encoded[i], 127); }
  AiColumnDefinition restored; ASSERT_EQ(OB_SUCCESS, restored.decode(allocator_, encoded));
  EXPECT_EQ(definition.expression_, restored.expression_); EXPECT_EQ(2, restored.max_retries_); EXPECT_EQ(512, restored.sql_mode_);
  EXPECT_EQ(200001, restored.definer_id_);
  ASSERT_EQ(1, restored.dependencies_.count()); EXPECT_EQ(18, restored.dependencies_.at(0));
  ObString clause; ASSERT_EQ(OB_SUCCESS, restored.format_clause(allocator_, clause));
  EXPECT_EQ(ObString("AI COLUMN (AI_COMPLETE('model', CONCAT('标签：', content))) WITH (sync_mode='async', max_retries=2)"), clause);
  definition.dimension_ = 4; EXPECT_FALSE(definition.valid());
  definition.function_ = static_cast<int64_t>(AiColumnFunction::EMBED); EXPECT_TRUE(definition.valid());
}
TEST_F(AiColumnState, definition_decoder_rejects_truncation_invalid_hex_and_unknown_version)
{
  AiColumnDefinition definition; definition.expression_ = "AI_EMBED('model', content)";
  definition.function_ = static_cast<int64_t>(AiColumnFunction::EMBED); definition.dimension_ = 4;
  ObString encoded; ASSERT_EQ(OB_SUCCESS, definition.encode(allocator_, encoded));
  AiColumnDefinition restored;
  for (int64_t i = 0; i < encoded.length(); ++i) {
    EXPECT_NE(OB_SUCCESS, restored.decode(allocator_, ObString(i, encoded.ptr()))); EXPECT_FALSE(restored.valid());
  }
  std::string invalid(encoded.ptr(), encoded.length()); invalid.back() = 'g';
  EXPECT_NE(OB_SUCCESS, restored.decode(allocator_, ObString(invalid.size(), invalid.data())));
  invalid.assign(encoded.ptr(), encoded.length()); invalid[10] = '9';
  EXPECT_NE(OB_SUCCESS, restored.decode(allocator_, ObString(invalid.size(), invalid.data())));
}
TEST_F(AiColumnState, dependency_remap_requires_complete_and_unique_target_ids)
{
  AiColumnDefinition definition; definition.expression_ = "AI_COMPLETE('model', CONCAT(a,b))";
  ASSERT_EQ(OB_SUCCESS, definition.dependencies_.push_back(12)); ASSERT_EQ(OB_SUCCESS, definition.dependencies_.push_back(35));
  ObSEArray<uint64_t, 4> from,to; ASSERT_EQ(OB_SUCCESS, from.push_back(12)); ASSERT_EQ(OB_SUCCESS, to.push_back(27));
  EXPECT_EQ(OB_INVALID_ARGUMENT, definition.remap_dependencies(from,to)); EXPECT_EQ(12, definition.dependencies_.at(0));
  ASSERT_EQ(OB_SUCCESS, from.push_back(35)); ASSERT_EQ(OB_SUCCESS, to.push_back(27));
  EXPECT_EQ(OB_INVALID_ARGUMENT, definition.remap_dependencies(from,to)); EXPECT_EQ(12, definition.dependencies_.at(0));
  to.at(1) = 9; ASSERT_EQ(OB_SUCCESS, definition.remap_dependencies(from,to));
  EXPECT_EQ(9, definition.dependencies_.at(0)); EXPECT_EQ(27, definition.dependencies_.at(1));
}
TEST_F(AiColumnState, schema_fence_cannot_be_claimed_until_current_inputs_are_filled)
{
  int64_t group = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(group));
  ASSERT_EQ(OB_SUCCESS, state_.enqueue_deferred(allocator_, 20, 9, group, 1));
  AiColumnClaim blocked; bool acquired = true;
  ASSERT_EQ(OB_SUCCESS, state_.claim(allocator_, 20, 100, 100, OWNER_A, blocked, acquired)); EXPECT_FALSE(acquired);
  EXPECT_FALSE(state_.group_ready(group)); EXPECT_EQ(AiColumnStatus::PENDING, state_.find(20)->status());
  ObString data; ASSERT_EQ(OB_SUCCESS, state_.encode(allocator_, data));
  AiColumnOutbox restored; ASSERT_EQ(OB_SUCCESS, restored.decode(data));
  EXPECT_EQ(static_cast<int64_t>(AiColumnStage::NEEDS_INPUT), restored.find(20)->stage_);
  ObSEArray<AiColumnInput, 4> inputs; AiColumnInput value; value.argument_position_ = 1; value.value_.set_varchar("current");
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(value)); bool accepted = true;
  ASSERT_EQ(OB_SUCCESS, state_.fill_inputs(allocator_, 20, 8, group, inputs, accepted)); EXPECT_FALSE(accepted);
  ASSERT_EQ(OB_SUCCESS, state_.fill_inputs(allocator_, 20, 9, group, inputs, accepted)); EXPECT_TRUE(accepted);
  ASSERT_EQ(OB_SUCCESS, state_.fill_inputs(allocator_, 20, 9, group, inputs, accepted)); EXPECT_FALSE(accepted);
  finish(claim(20)); ASSERT_EQ(OB_SUCCESS, state_.publish(group));
}
TEST_F(AiColumnState, retry_backoff_is_durable_and_honors_exact_deadline)
{
  int64_t group = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(group)); enqueue(20, group, 1);
  AiColumnClaim first = claim(20); bool accepted = false; ObObj result;
  ASSERT_EQ(OB_SUCCESS, state_.finish(allocator_, first, 150, OB_TIMEOUT, result, accepted, 50)); ASSERT_TRUE(accepted);
  ObString data; ASSERT_EQ(OB_SUCCESS, state_.encode(allocator_, data));
  AiColumnOutbox restored; ASSERT_EQ(OB_SUCCESS, restored.decode(data)); EXPECT_EQ(200, restored.find(20)->retry_at_);
  AiColumnClaim next; bool acquired = true;
  ASSERT_EQ(OB_SUCCESS, state_.claim(allocator_, 20, 199, 100, OWNER_B, next, acquired)); EXPECT_FALSE(acquired);
  ASSERT_EQ(OB_SUCCESS, state_.claim(allocator_, 20, 200, 100, OWNER_B, next, acquired)); ASSERT_TRUE(acquired);
  finish(next, OB_SUCCESS, true, 250); ASSERT_EQ(OB_SUCCESS, state_.publish(group));
}
TEST_F(AiColumnState, null_row_keys_are_rejected_instead_of_becoming_unaddressable_tasks)
{
  ObSEArray<ObObj, 4> key; ObObj value; value.set_null(); ASSERT_EQ(OB_SUCCESS, key.push_back(value));
  EXPECT_EQ(OB_INVALID_ARGUMENT, state_.init(allocator_, ROW, key)); EXPECT_FALSE(state_.valid());
}
TEST_F(AiColumnState, near_limit_input_leaves_room_for_all_leases_and_failure_metadata)
{
  int64_t group = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(group));
  std::string payload(AiColumnOutbox::INPUT_BYTES - 2048, 'x');
  ObSEArray<AiColumnInput, 4> inputs; AiColumnInput input;
  input.argument_position_ = 1; input.value_.set_varchar(ObString(payload.size(), payload.data()));
  input.value_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(input));
  ASSERT_EQ(OB_SUCCESS, state_.enqueue(allocator_, 20, 7, group, 1, inputs));
  ObSEArray<AiColumnInput, 4> empty;
  for (int64_t i = 1; i < AiColumnOutbox::MAX_COLUMNS; ++i) {
    ASSERT_EQ(OB_SUCCESS, state_.enqueue(allocator_, 20+i, 7, group, 1, empty));
  }
  AiColumnClaim leases[AiColumnOutbox::MAX_COLUMNS];
  for (int64_t i = 0; i < AiColumnOutbox::MAX_COLUMNS; ++i) { leases[i] = claim(20+i); }
  ObString encoded; ASSERT_EQ(OB_SUCCESS, state_.encode(allocator_, encoded));
  AiColumnOutbox recovered; ASSERT_EQ(OB_SUCCESS, recovered.decode(encoded));
  for (int64_t i = 0; i < AiColumnOutbox::MAX_COLUMNS; ++i) {
    ObObj result; bool accepted = false;
    ASSERT_EQ(OB_SUCCESS, recovered.finish(allocator_, leases[i], 150, OB_TIMEOUT, result, accepted, 5000000));
    ASSERT_TRUE(accepted);
  }
  ASSERT_EQ(OB_SUCCESS, recovered.encode(allocator_, encoded));
  payload.resize(AiColumnOutbox::INPUT_BYTES);
  inputs.at(0).value_.set_varchar(ObString(payload.size(), payload.data()));
  ASSERT_EQ(OB_SUCCESS, recovered.start_group(group));
  EXPECT_EQ(OB_SIZE_OVERFLOW, recovered.enqueue(allocator_, 20, 7, group, 1, inputs));
}

TEST_F(AiColumnState, schema_removal_preserves_other_claims_and_fences_readded_columns)
{
  int64_t group = 0; ASSERT_EQ(OB_SUCCESS, state_.start_group(group));
  enqueue(20, group); enqueue(21, group); enqueue(22, group);
  const AiColumnClaim removed = claim(20); const AiColumnClaim retained = claim(22);
  ASSERT_EQ(OB_SUCCESS, state_.erase(allocator_, 20)); EXPECT_EQ(2, state_.count());
  EXPECT_EQ(nullptr, state_.find(20)); finish(removed, OB_SUCCESS, false);
  finish(retained); finish(claim(21)); ASSERT_TRUE(state_.group_ready(group));
  ASSERT_EQ(OB_SUCCESS, state_.publish(group));
  ASSERT_EQ(OB_SUCCESS, state_.start_group(group)); enqueue(20, group, 0, 9);
  finish(removed, OB_SUCCESS, false); EXPECT_EQ(AiColumnStatus::PENDING, state_.find(20)->status());
  ASSERT_EQ(OB_SUCCESS, state_.erase(allocator_, 21)); ASSERT_EQ(OB_SUCCESS, state_.erase(allocator_, 20));
  ASSERT_EQ(OB_SUCCESS, state_.erase(allocator_, 22)); EXPECT_EQ(0, state_.count());
  ASSERT_EQ(OB_SUCCESS, state_.erase(allocator_, 22)); EXPECT_TRUE(state_.valid());
  ObString encoded; ASSERT_EQ(OB_SUCCESS, state_.encode(allocator_, encoded));
  AiColumnOutbox recovered; ASSERT_EQ(OB_SUCCESS, recovered.decode(encoded)); EXPECT_EQ(0, recovered.count());
  EXPECT_EQ(OB_INVALID_ARGUMENT, recovered.erase(allocator_, 0));
}

}
int main(int argc, char **argv) { ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
