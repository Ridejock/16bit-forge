// SPDX-License-Identifier: MIT
#include "debug/ram_search.hpp"

namespace bitforge::debug {

namespace {

auto passes(Compare compare, int64_t before, int64_t now, int64_t value) -> bool {
  switch (compare) {
    case Compare::Equal: return now == value;
    case Compare::NotEqual: return now != value;
    case Compare::Less: return now < value;
    case Compare::Greater: return now > value;
    case Compare::Changed: return now != before;
    case Compare::Unchanged: return now == before;
    case Compare::Increased: return now - before == value;
    case Compare::Decreased: return before - now == value;
  }
  return false;
}

}  // namespace

auto RamSearch::previous(uint32_t offset) const -> uint32_t {
  return read_value(snapshot_, offset, type_.size);
}

auto RamSearch::reset(std::span<const uint8_t> bytes, ValueType type) -> void {
  started_ = true;
  memory_bytes_ = bytes.size();
  type_ = type;
  steps_ = 0;
  undo_.clear();
  snapshot_.assign(bytes.begin(), bytes.end());
  candidates_.clear();
  const uint32_t last = bytes.size() > bytes_of(type.size) ? static_cast<uint32_t>(bytes.size() - bytes_of(type.size) + 1) : 0;
  candidates_.reserve(last);
  for (uint32_t offset = 0; offset < last; offset++) candidates_.push_back(offset);
}

auto RamSearch::filter(std::span<const uint8_t> memory, Compare compare, uint32_t value) -> bool {
  if (!started_ || memory.size() != memory_bytes_) return false;
  const int64_t typed = to_number(value & mask_of(type_.size), type_);
  std::vector<uint32_t> next;
  next.reserve(candidates_.size());
  for (const uint32_t offset : candidates_) {
    const int64_t before = to_number(read_value(snapshot_, offset, type_.size), type_);
    const int64_t now = to_number(read_value(memory, offset, type_.size), type_);
    if (passes(compare, before, now, typed)) next.push_back(offset);
  }
  undo_.push_back({candidates_, snapshot_});
  candidates_ = std::move(next);
  snapshot_.assign(memory.begin(), memory.end());
  steps_++;
  return true;
}

auto RamSearch::undo() -> void {
  if (undo_.empty()) return;
  candidates_ = std::move(undo_.back().candidates);
  snapshot_ = std::move(undo_.back().snapshot);
  undo_.pop_back();
  if (steps_ > 0) steps_--;
}

}  // namespace bitforge::debug
