// SPDX-License-Identifier: MIT
#include "snes/coprocessor/rtc4513.hpp"

#include <algorithm>

namespace bitforge::coprocessor {

namespace {

enum : int { kSecLo, kSecHi, kMinLo, kMinHi, kHourLo, kHourHi, kDayLo, kDayHi, kMonLo, kMonHi, kYearLo, kYearHi,
             kWeekday, kControlD, kControlE, kControlF };

// Stored bits of each register. Extra bits beside the BCD tens digit: 1 bit 3 battery failure,
// 5 bit 2 PM (12-hour mode), 7 bit 2 and 9 bits 1-2 spare RAM. Register D bit 2 (IRQ flag)
// isn't writable; registers 3/5/7/9/C read bit 3 as the resync flag.
constexpr std::array<Byte, 16> kMask = {0xF, 0xF, 0xF, 0x7, 0xF, 0x7, 0xF, 0x7, 0xF, 0x7, 0xF, 0xF, 0x7, 0xB, 0xF, 0xF};
constexpr std::array<bool, 16> kResync = {false, false, false, true, false, true, false, true,
                                          false, true, false, false, true, false, false, false};

} // namespace

auto RTC4513::power_on(std::time_t now) -> void {
  regs_.fill(0);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  set_field(kSecLo, 0x7, std::min(local.tm_sec, 59));
  set_field(kMinLo, 0x7, local.tm_min);
  set_field(kHourLo, 0x3, local.tm_hour);
  set_field(kDayLo, 0x3, local.tm_mday);
  set_field(kMonLo, 0x1, local.tm_mon + 1);
  set_field(kYearLo, 0xF, local.tm_year % 100);
  regs_[kWeekday] = static_cast<Byte>(local.tm_wday);
  regs_[kControlD] = 0x2;  // calendar on
  regs_[kControlF] = 0x4;  // 24-hour mode, running
  clocks_ = 0;
  hold_tick_ = false;
  resync_ = false;
  chip_select_ = 0;
  reset();
}

auto RTC4513::reset() -> void {
  ready_ = true;  // accesses complete instantly
  state_ = kMode;
  offset_ = 0;
  last_ = 0;
  resync_ = false;
  regs_[kControlF] &= 0x6;  // chip deselect clears PAUSE and TEST
}

auto RTC4513::field(int lo, Byte hi_mask) const -> int {
  return (regs_[static_cast<size_t>(lo + 1)] & hi_mask) * 10 + (regs_[static_cast<size_t>(lo)] & 0xF);
}

auto RTC4513::set_field(int lo, Byte hi_mask, int value) -> void {
  regs_[static_cast<size_t>(lo)] = static_cast<Byte>(value % 10);
  Byte& hi = regs_[static_cast<size_t>(lo + 1)];
  hi = static_cast<Byte>((hi & ~hi_mask) | ((value / 10) & hi_mask));
}

auto RTC4513::read(int port) -> Byte {
  switch (port) {
    case 0: return chip_select_;
    case 1:
      if (chip_select_ != 1 || !ready_) return 0;
      if (state_ == kWrite) return last_;
      if (state_ != kRead) return 0;
      return read_reg(offset_++ & 15);
    case 2: return static_cast<Byte>(ready_ ? 0x80 : 0x00);
    default: return 0;
  }
}

auto RTC4513::write(int port, Byte value) -> void {
  value &= 0x0F;
  if (port == 0) {
    chip_select_ = value;
    if (chip_select_ != 1) reset();
    ready_ = true;
    return;
  }
  if (port != 1 || chip_select_ != 1 || !ready_) return;
  switch (state_) {
    case kMode:
      if (value != 0x03 && value != 0x0C) return;
      state_ = kSeek;
      break;
    case kSeek:
      state_ = last_ == 0x03 ? kWrite : kRead;
      offset_ = value;
      break;
    case kWrite:
      write_reg(offset_++ & 15, value);
      break;
    default: break;
  }
  last_ = value;
}

auto RTC4513::read_reg(int index) -> Byte {
  const auto i = static_cast<size_t>(index);
  return static_cast<Byte>(regs_[i] | (kResync[i] && resync_ ? 0x8 : 0x0));
}

auto RTC4513::write_reg(int index, Byte value) -> void {
  const bool was_held = regs_[kControlD] & 1;
  regs_[static_cast<size_t>(index)] = value & kMask[static_cast<size_t>(index)];
  if (index == kControlF && (value & 0x1)) {  // PAUSE zeroes the seconds
    regs_[kSecLo] = 0;
    regs_[kSecHi] &= 0x8;
  }
  if (index == kControlF || index == kHourHi) regs_[kHourHi] = static_cast<Byte>(regs_[kHourHi] & (regs_[kControlF] & 0x4 ? 0x3 : 0x5));  // bit 2 = PM in 12 h mode
  if (index == kControlD) {
    if (value & 0x8) {  // round to the nearest minute
      regs_[kControlD] &= 0x7;
      if (field(kSecLo, 0x7) >= 30) tick_minute();
      set_field(kSecLo, 0x7, 0);
    }
    if (was_held && !(value & 1) && hold_tick_) {  // a second passed during HOLD: catch up
      hold_tick_ = false;
      tick_second();
    }
  }
}

auto RTC4513::step(uint32_t master_clocks) -> void {
  clocks_ += master_clocks;
  while (clocks_ >= kMasterClock) {
    clocks_ -= kMasterClock;
    if (regs_[kControlF] & 0x3) continue;                       // PAUSE or STOP
    if (regs_[kControlD] & 1) { hold_tick_ = true; continue; }  // HOLD: catch up on release
    resync_ = true;  // the time changed under a reader: it should start over
    tick_second();
  }
}

auto RTC4513::save_file(int64_t now) const -> std::array<Byte, kFileSize> {
  std::array<Byte, kFileSize> data{};
  for (size_t i = 0; i < 8; i++) {
    data[i] = static_cast<Byte>((regs_[2 * i] & kMask[2 * i]) | (regs_[2 * i + 1] & kMask[2 * i + 1]) << 4);
  }
  auto stamp = static_cast<uint64_t>(now);
  for (size_t i = 0; i < 8; i++, stamp >>= 8) data[8 + i] = static_cast<Byte>(stamp);
  return data;
}

auto RTC4513::load_file(std::span<const Byte> data, int64_t now) -> bool {
  if (data.size() != kFileSize) return false;
  for (size_t i = 0; i < 8; i++) {
    regs_[2 * i] = data[i] & 0x0F & kMask[2 * i];
    regs_[2 * i + 1] = static_cast<Byte>(data[i] >> 4) & kMask[2 * i + 1];
  }
  uint64_t stamp = 0;
  for (size_t i = 0; i < 8; i++) stamp |= static_cast<uint64_t>(data[8 + i]) << (8 * i);
  hold_tick_ = resync_ = false;
  clocks_ = 0;
  if (regs_[kControlF] & 0x3) return true;  // stopped or paused while the emulator was closed
  int64_t elapsed = now - static_cast<int64_t>(stamp);
  if (elapsed <= 0) return true;  // the host clock went back: keep the saved time
  constexpr int64_t kDay = 86400, kHour = 3600, kMinute = 60;
  for (; elapsed >= kDay; elapsed -= kDay) tick_day();
  for (; elapsed >= kHour; elapsed -= kHour) tick_hour();
  for (; elapsed >= kMinute; elapsed -= kMinute) tick_minute();
  for (; elapsed > 0; elapsed--) tick_second();
  return true;
}

auto RTC4513::tick_second() -> void {
  const int second = field(kSecLo, 0x7) + 1;
  if (second < 60) return set_field(kSecLo, 0x7, second);
  set_field(kSecLo, 0x7, 0);
  tick_minute();
}

auto RTC4513::tick_minute() -> void {
  const int minute = field(kMinLo, 0x7) + 1;
  if (minute < 60) return set_field(kMinLo, 0x7, minute);
  set_field(kMinLo, 0x7, 0);
  tick_hour();
}

auto RTC4513::tick_hour() -> void {
  if (regs_[kControlF] & 0x4) {  // 24-hour: 0-23
    const int hour = field(kHourLo, 0x3) + 1;
    if (hour < 24) return set_field(kHourLo, 0x3, hour);
    set_field(kHourLo, 0x3, 0);
    return tick_day();
  }
  // 12-hour: 12, 1, ..., 11 with the PM bit flipping at 11 -> 12; midnight starts a new day.
  const int hour = field(kHourLo, 0x1);
  if (hour == 11) {
    regs_[kHourHi] ^= 0x4;
    set_field(kHourLo, 0x1, 12);
    if (!(regs_[kHourHi] & 0x4)) tick_day();
    return;
  }
  set_field(kHourLo, 0x1, hour >= 12 ? 1 : hour + 1);
}

auto RTC4513::tick_day() -> void {
  if (!(regs_[kControlD] & 0x2)) return;  // calendar off: the date doesn't advance
  regs_[kWeekday] = static_cast<Byte>((regs_[kWeekday] + 1) % 7);
  static constexpr std::array<int, 13> kDays = {31, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int month = field(kMonLo, 0x1);
  const int year = field(kYearLo, 0xF);
  const int days = month == 2 && year % 4 == 0 ? 29 : kDays[static_cast<size_t>(month <= 12 ? month : 0)];
  const int day = field(kDayLo, 0x3) + 1;
  if (day <= days) return set_field(kDayLo, 0x3, day);
  set_field(kDayLo, 0x3, 1);
  if (month < 12) return set_field(kMonLo, 0x1, month + 1);
  set_field(kMonLo, 0x1, 1);
  set_field(kYearLo, 0xF, (year + 1) % 100);
}

} // namespace bitforge::coprocessor
