/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"

namespace oceanbase::share {
enum class AiColumnFunction : int64_t { COMPLETE, EMBED };
class AiColumnDefinition
{
  OB_UNIS_VERSION(1);
public:
  static constexpr int64_t MAX_EXPRESSION_BYTES = 1024 * 1024;
  static constexpr int64_t MAX_DEPENDENCIES = 4096;
  static constexpr int64_t MAX_METADATA_BYTES = 4 * 1024 * 1024;
  common::ObString expression_;
  int64_t function_ = static_cast<int64_t>(AiColumnFunction::COMPLETE);
  int64_t dimension_ = 0;
  int64_t max_retries_ = 0;
  uint64_t sql_mode_ = 0;
  uint64_t definer_id_ = 0;
  common::ObSEArray<uint64_t, 4> dependencies_;
  void reset();
  bool valid() const;
  int encode(common::ObIAllocator &allocator, common::ObString &metadata) const;
  int decode(common::ObIAllocator &allocator, const common::ObString &metadata);
  int format_clause(common::ObIAllocator &allocator, common::ObString &clause) const;
  int remap_dependencies(const common::ObIArray<uint64_t> &from, const common::ObIArray<uint64_t> &to);
};
}
