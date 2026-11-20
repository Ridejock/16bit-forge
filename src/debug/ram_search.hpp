// SPDX-License-Identifier: MIT
#pragma once
// A byte-wise search over one memory image. Reset captures every address that fits the value
// size and remembers those bytes. Each filter keeps the addresses whose new bytes pass, then
// that image becomes the "previous" one. Undo puts the last filter back.
#include "debug/memory_value.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace bitforge::debug {

class RamSearch {
 public:
  auto started() const -> bool { return started_; }
  auto memory_bytes() const -> size_t { return memory_bytes_; }
  auto type() const -> ValueType { return type_; }
  auto count() const -> size_t { return candidates_.size(); }
  auto steps() const -> size_t { return steps_; }
  auto can_undo() const -> bool { return !undo_.empty(); }
  auto candidates() const -> std::span<const uint32_t> { return candidates_; }
  auto previous(uint32_t offset) const -> uint32_t;

  // Every fitting address is a candidate. `bytes` is the previous value of each one.
  auto reset(std::span<const uint8_t> bytes, ValueType type) -> void;
  // Keep candidates that pass `compare` against `previous`. False when `memory` changed size
  // (the search has to be reset). `value` is the typed number for compares that take one.
  auto filter(std::span<const uint8_t> memory, Compare compare, uint32_t value) -> bool;
  auto undo() -> void;

 private:
  struct Step {
    std::vector<uint32_t> candidates;
    std::vector<uint8_t> snapshot;
  };

  bool started_ = false;
  size_t memory_bytes_ = 0;
  size_t steps_ = 0;
  ValueType type_{};
  std::vector<uint32_t> candidates_;
  std::vector<uint8_t> snapshot_;
  std::vector<Step> undo_;
};

}  // namespace bitforge::debug
