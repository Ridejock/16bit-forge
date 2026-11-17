// SPDX-License-Identifier: MIT
#include "snes/coprocessor/necdsp.hpp"

namespace bitforge::coprocessor {

namespace {
// NTSC master clock = 236.25 MHz / 11. One master clock is worth clock_hz * 11 budget units and
// one DSP instruction costs kMasterUnit, so the ratio stays exact.
constexpr int64_t kMasterUnit = 236'250'000;
}

NecDsp::NecDsp(Chip chip, const NecVariant& variant, const NecBoard& board)
    : chip_(chip), dsp_(variant), board_(board) {}

auto NecDsp::reset() -> void {
  dsp_.reset();
  clock_budget_ = 0;
}

auto NecDsp::read(Address address) -> std::optional<Byte> {
  if (board_.io.contains(address)) {
    return (address & board_.io.select) ? dsp_.read_sr() : dsp_.read_dr();
  }
  if (board_.ram && board_.ram->contains(address)) {
    return dsp_.read_dp(static_cast<Word>(address & board_.ram->select));
  }
  return std::nullopt;
}

auto NecDsp::write(Address address, Byte value) -> bool {
  if (board_.io.contains(address)) {
    if (!(address & board_.io.select)) dsp_.write_dr(value);  // SR is read-only
    return true;
  }
  if (board_.ram && board_.ram->contains(address)) {
    dsp_.write_dp(static_cast<Word>(address & board_.ram->select), value);
    return true;
  }
  return false;
}

auto NecDsp::step(uint32_t master_clocks) -> void {
  clock_budget_ += static_cast<int64_t>(master_clocks) * dsp_.variant().clock_hz * 11;
  while (clock_budget_ > 0) {
    dsp_.exec();
    clock_budget_ -= kMasterUnit;
  }
}

// The uPD96050 boards keep their save data in the DSP's own data RAM.
auto NecDsp::battery_ram() -> std::span<Byte> {
  if (!board_.ram) return {};
  const std::span<uint16_t> words = dsp_.data_ram();
  return {reinterpret_cast<Byte*>(words.data()), words.size_bytes()};  // little-endian host
}

auto NecDsp::serialize(Archive& ar) -> void {
  dsp_.serialize(ar);
  ar(clock_budget_);
}

} // namespace bitforge::coprocessor
