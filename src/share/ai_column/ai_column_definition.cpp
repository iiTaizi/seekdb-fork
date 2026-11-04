/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "share/ai_column/ai_column_definition.h"
#include "share/ai_column/ai_column_state.h"
#include "share/ob_errno.h"
#include "lib/charset/ob_charset.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <cstdio>

namespace oceanbase::share {
using namespace common;
namespace {
constexpr char PREFIX[] = "AI_COLUMN_V1:";
int hex_digit(char ch)
{
  return ch >= '0' && ch <= '9' ? ch-'0' : ch >= 'a' && ch <= 'f' ? ch-'a'+10
      : ch >= 'A' && ch <= 'F' ? ch-'A'+10 : -1;
}
}
void AiColumnDefinition::reset()
{
  expression_.reset(); function_ = static_cast<int64_t>(AiColumnFunction::COMPLETE);
  dimension_ = max_retries_ = 0; sql_mode_ = definer_id_ = 0; dependencies_.reuse();
}
bool AiColumnDefinition::valid() const
{
  int64_t valid_bytes = 0;
  bool ok = !expression_.empty() && expression_.length() <= MAX_EXPRESSION_BYTES
      && memchr(expression_.ptr(), 0, expression_.length()) == nullptr
      && OB_SUCCESS == ObCharset::well_formed_len(CS_TYPE_UTF8MB4_BIN, expression_.ptr(), expression_.length(), valid_bytes)
      && valid_bytes == expression_.length()
      && function_ >= static_cast<int64_t>(AiColumnFunction::COMPLETE)
      && function_ <= static_cast<int64_t>(AiColumnFunction::EMBED)
      && dimension_ >= 0 && (function_ != static_cast<int64_t>(AiColumnFunction::COMPLETE) || dimension_ == 0)
      && max_retries_ >= 0 && max_retries_ <= AiColumnOutbox::MAX_RETRIES
      && dependencies_.count() <= MAX_DEPENDENCIES;
  for (int64_t i = 0; ok && i < dependencies_.count(); ++i) {
    ok = dependencies_.at(i) > 0 && dependencies_.at(i) <= INT64_MAX
        && (i == 0 || dependencies_.at(i-1) < dependencies_.at(i));
  }
  return ok;
}
OB_DEF_SERIALIZE(AiColumnDefinition)
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_ENCODE, expression_, function_, dimension_, max_retries_, sql_mode_);
  OB_UNIS_ENCODE(dependencies_.count());
  for (int64_t i = 0; OB_SUCC(ret) && i < dependencies_.count(); ++i) { OB_UNIS_ENCODE(dependencies_.at(i)); }
  OB_UNIS_ENCODE(definer_id_);
  return ret;
}
OB_DEF_DESERIALIZE(AiColumnDefinition)
{
  int ret = OB_SUCCESS; reset(); int64_t count = 0;
  LST_DO_CODE(OB_UNIS_DECODE, expression_, function_, dimension_, max_retries_, sql_mode_, count);
  if (OB_SUCC(ret) && (count < 0 || count > MAX_DEPENDENCIES)) { ret = OB_INVALID_DATA; }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    uint64_t id = 0; OB_UNIS_DECODE(id);
    if (OB_SUCC(ret)) { ret = dependencies_.push_back(id); }
  }
  OB_UNIS_DECODE(definer_id_);
  if (OB_SUCC(ret) && !valid()) { ret = OB_INVALID_DATA; }
  if (OB_FAIL(ret)) { reset(); } return ret;
}
OB_DEF_SERIALIZE_SIZE(AiColumnDefinition)
{
  int64_t len = 0; LST_DO_CODE(OB_UNIS_ADD_LEN, expression_, function_, dimension_, max_retries_, sql_mode_);
  OB_UNIS_ADD_LEN(dependencies_.count());
  for (int64_t i = 0; i < dependencies_.count(); ++i) { OB_UNIS_ADD_LEN(dependencies_.at(i)); }
  OB_UNIS_ADD_LEN(definer_id_);
  return len;
}
int AiColumnDefinition::encode(ObIAllocator &allocator, ObString &metadata) const
{
  int ret = OB_SUCCESS; metadata.reset();
  if (!valid()) { ret = OB_INVALID_ARGUMENT; }
  else {
    const int64_t binary_size = get_serialize_size();
    const int64_t size = sizeof(PREFIX)-1 + 2*binary_size;
    if (size > MAX_METADATA_BYTES) { ret = OB_SIZE_OVERFLOW; }
    else {
      char *binary = static_cast<char *>(allocator.alloc(binary_size));
      char *text = static_cast<char *>(allocator.alloc(size)); int64_t pos = 0;
      if (binary == nullptr || text == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
      else if (OB_FAIL(serialize(binary, binary_size, pos))) {}
      else {
        memcpy(text, PREFIX, sizeof(PREFIX)-1); const char digits[] = "0123456789abcdef";
        for (int64_t i = 0; i < pos; ++i) {
          const uint8_t byte = static_cast<uint8_t>(binary[i]);
          text[sizeof(PREFIX)-1+2*i] = digits[byte >> 4]; text[sizeof(PREFIX)+2*i] = digits[byte & 15];
        }
        metadata.assign_ptr(text, static_cast<int32_t>(sizeof(PREFIX)-1+2*pos));
      }
    }
  }
  return ret;
}
int AiColumnDefinition::decode(ObIAllocator &allocator, const ObString &metadata)
{
  reset(); int ret = OB_SUCCESS; const int64_t offset = sizeof(PREFIX)-1;
  if (metadata.length() <= offset || metadata.length() > MAX_METADATA_BYTES
      || (metadata.length()-offset)%2 != 0 || memcmp(metadata.ptr(), PREFIX, offset) != 0) { ret = OB_INVALID_DATA; }
  else {
    const int64_t size = (metadata.length()-offset)/2;
    char *binary = static_cast<char *>(allocator.alloc(size));
    if (binary == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
      const int a = hex_digit(metadata[offset+2*i]), b = hex_digit(metadata[offset+2*i+1]);
      if (a < 0 || b < 0) { ret = OB_INVALID_DATA; }
      else { binary[i] = static_cast<char>((a << 4) | b); }
    }
    int64_t pos = 0;
    if (OB_SUCC(ret)) { ret = deserialize(binary, size, pos); }
    if (OB_SUCC(ret) && pos != size) { ret = OB_INVALID_DATA; }
  }
  if (OB_FAIL(ret)) { reset(); } return ret;
}
int AiColumnDefinition::remap_dependencies(const ObIArray<uint64_t> &from, const ObIArray<uint64_t> &to)
{
  int ret = OB_SUCCESS; ObSEArray<uint64_t, 4> remapped;
  if (!valid() || from.count() != to.count() || from.count() > MAX_DEPENDENCIES) { ret = OB_INVALID_ARGUMENT; }
  for (int64_t i = 0; OB_SUCC(ret) && i < dependencies_.count(); ++i) {
    bool found = false; uint64_t id = 0;
    for (int64_t j = 0; OB_SUCC(ret) && j < from.count(); ++j) {
      if (from.at(j) == dependencies_.at(i)) {
        if (found || to.at(j) == 0 || to.at(j) > INT64_MAX) { ret = OB_INVALID_ARGUMENT; }
        else { found = true; id = to.at(j); }
      }
    }
    if (OB_SUCC(ret) && !found) { ret = OB_INVALID_ARGUMENT; }
    if (OB_SUCC(ret)) { ret = remapped.push_back(id); }
  }
  if (OB_SUCC(ret) && remapped.count() > 1) {
    std::sort(remapped.get_data(), remapped.get_data()+remapped.count());
    for (int64_t i = 1; OB_SUCC(ret) && i < remapped.count(); ++i) {
      if (remapped.at(i) == remapped.at(i-1)) { ret = OB_INVALID_ARGUMENT; }
    }
  }
  if (OB_SUCC(ret)) { ret = dependencies_.assign(remapped); }
  return ret;
}
int AiColumnDefinition::format_clause(ObIAllocator &allocator, ObString &clause) const
{
  clause.reset(); int ret = OB_SUCCESS;
  if (!valid()) { ret = OB_INVALID_DATA; }
  else {
    const int64_t size = expression_.length()+128;
    char *text = static_cast<char *>(allocator.alloc(size));
    if (text == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else {
      const int length = snprintf(text, size, "AI COLUMN (%.*s) WITH (sync_mode='async', max_retries=%ld)",
          expression_.length(), expression_.ptr(), static_cast<long>(max_retries_));
      if (length < 0 || length >= size) { ret = OB_SIZE_OVERFLOW; }
      else { clause.assign_ptr(text, length); }
    }
  }
  return ret;
}
}
