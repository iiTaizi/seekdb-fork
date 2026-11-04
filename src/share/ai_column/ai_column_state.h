/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "common/object/ob_object.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase::share {
struct AiColumnInput
{
  OB_UNIS_VERSION(1);
public:
  uint64_t argument_position_ = 0;
  common::ObObj value_;
};

enum class AiColumnStage : int64_t { QUEUED, CLAIMED, SUCCEEDED, EXHAUSTED, PUBLISHED, NEEDS_INPUT };
enum class AiColumnStatus : int64_t { PENDING, READY, FAILED };

struct AiColumnTask
{
  OB_UNIS_VERSION(1);
public:
  uint64_t column_id_ = 0;
  int64_t definition_version_ = 0;
  int64_t request_ = 0;
  int64_t group_ = 0;
  int64_t stage_ = static_cast<int64_t>(AiColumnStage::QUEUED);
  int64_t attempts_ = 0;
  int64_t max_retries_ = 0;
  int64_t lease_deadline_ = 0;
  common::ObString lease_token_;
  int64_t last_error_ = 0;
  int64_t retry_at_ = 0;
  common::ObObj result_;
  // Sorted by stable argument position; LOB data must already be materialized.
  common::ObSEArray<AiColumnInput, 4> inputs_;
  void reset();
  bool terminal() const;
  AiColumnStatus status() const;
  bool valid() const;
};

struct AiColumnClaim
{
  common::ObString row_identity_;
  uint64_t column_id_ = 0;
  int64_t definition_version_ = 0;
  int64_t request_ = 0;
  int64_t attempt_ = 0;
  common::ObString lease_token_;
};

// All mutations must be persisted by the caller with a CAS against the old row
// state. No helper in this class substitutes for a database transaction.
class AiColumnOutbox
{
  OB_UNIS_VERSION(1);
public:
  static constexpr int64_t MAX_COLUMNS = 16;
  static constexpr int64_t MAX_INPUTS = 4096;
  static constexpr int64_t MAX_ROWKEY = 64;
  static constexpr int64_t MAX_BYTES = 1024 * 1024;
  static constexpr int64_t INPUT_BYTES = MAX_BYTES - 4096;
  static constexpr int64_t MAX_RETRIES = 100;
  void reset();
  int init(common::ObIAllocator &allocator, const common::ObString &row_identity,
           const common::ObIArray<common::ObObj> &rowkey);
  int start_group(int64_t &group);
  int erase(common::ObIAllocator &allocator, uint64_t column);
  int enqueue(common::ObIAllocator &allocator, uint64_t column, int64_t definition_version,
              int64_t group, int64_t max_retries, const common::ObIArray<AiColumnInput> &inputs);
  int enqueue_deferred(common::ObIAllocator &allocator, uint64_t column, int64_t definition_version,
                       int64_t group, int64_t max_retries);
  int fill_inputs(common::ObIAllocator &allocator, uint64_t column, int64_t definition_version,
                  int64_t request, const common::ObIArray<AiColumnInput> &inputs, bool &accepted);
  int claim(common::ObIAllocator &allocator, uint64_t column, int64_t now, int64_t lease_us,
            const common::ObString &token, AiColumnClaim &claim, bool &acquired);
  int finish(common::ObIAllocator &allocator, const AiColumnClaim &claim, int64_t now,
             int error, const common::ObObj &result, bool &accepted, int64_t retry_delay_us = 0);
  bool group_ready(int64_t group) const;
  int publish(int64_t group);
  int encode(common::ObIAllocator &allocator, common::ObString &encoded) const;
  int decode(const common::ObString &encoded);
  const AiColumnTask *find(uint64_t column) const;
  const AiColumnTask &at(int64_t index) const { return tasks_[index]; }
  int64_t count() const { return count_; }
  const common::ObString &row_identity() const { return row_identity_; }
  const common::ObIArray<common::ObObj> &rowkey() const { return rowkey_; }
  bool valid() const;
  static bool durable_value(const common::ObObj &value);
private:
  AiColumnTask *mutable_task(uint64_t column);
  common::ObString row_identity_;
  common::ObSEArray<common::ObObj, 4> rowkey_;
  int64_t sequence_ = 0;
  int64_t count_ = 0;
  AiColumnTask tasks_[MAX_COLUMNS];
};
}
