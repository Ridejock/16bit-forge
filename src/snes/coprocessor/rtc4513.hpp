// SPDX-License-Identifier: MIT
#pragma once
// Epson RTC-4513 real-time clock (SPC7110 boards: Tengai Makyou Zero / Far East of Eden Zero).
// The S-CPU talks to it through three ports in the SPC7110's register block:
//   $4840  chip select (1 = selected; anything else resets the serial state)
//   $4841  data: a mode nibble ($03 write, $0C read), a register index, then nibbles that
//          auto-increment through the 16 registers
//   $4842  bit 7 = ready
// The 16 registers are BCD nibbles: seconds, minutes, hours, day, month, year (lo/hi each),
// weekday, and the D/E/F control registers (hold, 12/24 h, stop, ...). Time advances with the
// emulated master clock; at power-on it starts from the host's local time (24-hour mode,
// calendar on). Register semantics follow the Epson RTC-4513 datasheet and fullsnes ("SNES
// Cart S-RTC / RTC-4513"); accesses are ready instantly.
#include "snes/serialize.hpp"
#include "snes/types.hpp"

#include <array>
#include <cstdint>
#include <ctime>
#include <span>

namespace snes::coprocessor {

class RTC4513 {
public:
  RTC4513() { power_on(std::time(nullptr)); }

  auto power_on(std::time_t now) -> void;  // registers from `now` (local time), 24-hour mode
  auto reset() -> void;                     // serial interface only; the clock keeps running
  auto read(int port) -> Byte;              // port 0-2 = $4840-$4842
  auto write(int port, Byte value) -> void;
  auto step(uint32_t master_clocks) -> void;
  // Battery file (<rom>.rtc), in the layout other emulators use, for save compatibility: 8 bytes of register nibble pairs (0|1<<4, 2|3<<4,
  // ... E|F<<4) and the host's Unix time at saving, 8 bytes little-endian. load() advances the
  // clock by the real time since then (not while STOP or PAUSE is set).
  static constexpr size_t kFileSize = 16;
  auto save_file(int64_t now) const -> std::array<Byte, kFileSize>;
  auto load_file(std::span<const Byte> data, int64_t now) -> bool;

  auto serialize(Archive& ar) -> void { ar(regs_, chip_select_, state_, offset_, last_, ready_, clocks_, hold_tick_, resync_); }

  // For tests and tools.
  auto reg(int index) const -> Byte { return regs_[static_cast<size_t>(index & 15)]; }
  auto tick_second() -> void;

  static constexpr uint64_t kMasterClock = 21'477'272;

private:
  enum : Byte { kMode, kSeek, kRead, kWrite };
  std::array<Byte, 16> regs_{};
  Byte chip_select_ = 0;
  Byte state_ = kMode;
  Byte offset_ = 0;
  Byte last_ = 0;
  bool ready_ = false;
  uint64_t clocks_ = 0;
  bool hold_tick_ = false;  // a second passed while HOLD was set
  bool resync_ = false;     // a second ticked since chip select: registers 3/5/7/9/C bit 3

  auto read_reg(int index) -> Byte;
  auto write_reg(int index, Byte value) -> void;
  auto field(int lo, Byte hi_mask) const -> int;  // BCD pair lo/lo+1 (tens digit masked)
  auto set_field(int lo, Byte hi_mask, int value) -> void;
  auto tick_minute() -> void;
  auto tick_hour() -> void;
  auto tick_day() -> void;
};

} // namespace snes::coprocessor
