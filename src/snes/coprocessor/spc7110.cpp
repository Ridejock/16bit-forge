// SPDX-License-Identifier: MIT
#include "snes/coprocessor/spc7110.hpp"
#include "snes/memory/rom_mirror.hpp"

#include "utils/logging.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <string>

namespace snes::coprocessor {

namespace {

using utils::LogChannel;
using utils::Logger;

// Log keys (LogLimiter slots): 0-$3F = first write to each register, then these.
enum : size_t { kLogSramEnable = 0x40, kLogDcu, kLogDcuInvalid, kLogSramRead, kLogSramWrite, kLogSramDisabled,
                kLogCheckRead, kLogCheckWrite, kLogRtc, kLogPort, kLogNoRam };

auto sign16(uint32_t value) -> uint32_t { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value))); }

} // namespace

// ---- Decompressor -------------------------------------------------------------------------------

// Tables from fullsnes ("SNES Cart SPC7110 Decompression Algorithm").
const std::array<Byte, 53> SPC7110Decompressor::kProbability = {
  90, 37, 17,  8,  3,  1, 90, 63, 44, 32, 23, 17, 12,  9,  7,  5,  4,  3,  2,
  90, 72, 58, 46, 38, 31, 25, 21, 17, 14, 11,  9,  8,  7,  5,  4,  4,  3,  2,
   2, 88, 77, 67, 59, 52, 46, 41, 37, 86, 79, 71, 65, 60, 55,
};
const std::array<Byte, 53> SPC7110Decompressor::kNextLps = {
   1,  6,  8, 10, 12, 15,  7, 19, 21, 22, 23, 25, 26, 28, 29, 31, 32, 34, 35,
  20, 39, 40, 42, 44, 45, 46, 25, 26, 26, 27, 28, 29, 30, 31, 33, 33, 34, 35,
  36, 39, 47, 48, 49, 50, 51, 44, 45, 47, 47, 48, 49, 50, 51,
};
const std::array<Byte, 53> SPC7110Decompressor::kNextMps = {
   1,  2,  3,  4,  5,  5,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18,  5,
  20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
   5, 40, 41, 42, 43, 44, 45, 46, 24, 48, 49, 50, 51, 52, 43,
};
const std::array<Byte, 15> SPC7110Decompressor::kMode2Context = {0, 0, 0, 15, 17, 19, 21, 23, 25, 25, 25, 25, 25, 27, 29};

auto SPC7110Decompressor::neighbour_class(unsigned a, unsigned b, unsigned c) -> unsigned {
  if (a == b && b == c) return 0;
  if (a == b) return 1;
  if (b == c) return 2;
  if (a == c) return 3;
  return 4;
}

auto SPC7110Decompressor::initialize(unsigned mode, uint32_t origin) -> void {
  mode_ = mode & 3;
  if (mode_ == 3) mode_ = 2;
  state_.fill(0);
  invert_.fill(0);
  top_ = 255;
  source_ = origin;
  window_ = static_cast<uint32_t>(read_(source_)) << 8 | read_(source_ + 1);
  source_ += 2;
  window_bits_ = 8;
  for (unsigned i = 0; i < 16; i++) order_[i] = static_cast<Byte>(i);
  out_ = 0;
  c_ = 0;
}

// One binary decision in `context`; the decoded bit is shifted into `decoded`.
auto SPC7110Decompressor::decode_bit(unsigned context, unsigned& decoded) -> void {
  const Byte state = state_[context];
  const unsigned probability = kProbability[state];
  unsigned bit = invert_[context];
  top_ -= probability;
  if ((window_ >> 8) > top_) {  // the less probable symbol
    window_ -= (top_ + 1) << 8;
    top_ = probability - 1;
    if (top_ > 79) invert_[context] ^= 1;
    bit ^= 1;
    state_[context] = kNextLps[state];
  } else if (top_ <= 126) {
    state_[context] = kNextMps[state];
  }
  while (top_ <= 126) {  // renormalise
    top_ = (top_ << 1) | 1;
    window_ = (window_ << 1) & 0xFFFF;
    if (--window_bits_ == 0) {
      window_ |= read_(source_++);
      window_bits_ = 8;
    }
  }
  decoded = (decoded << 1) | bit;
}

// Maps a decoded rank to a colour: the most recently used colours, re-ranked so the
// neighbours a, b and c (in that priority) come first, then shifts the pixel in.
auto SPC7110Decompressor::place_pixel(unsigned a, unsigned b, unsigned decoded) -> void {
  auto to_front = [](auto& list, unsigned colour) {
    unsigned i = 0;
    while (list[i] != colour) i++;
    for (; i > 0; i--) list[i] = list[i - 1];
    list[0] = static_cast<Byte>(colour);
  };
  to_front(order_, a);
  std::array<Byte, 16> ranked = order_;
  to_front(ranked, c_);
  to_front(ranked, b);
  to_front(ranked, a);
  const unsigned shift = 1u << mode_;
  out_ = (out_ << shift) | ranked[decoded];
  c_ = b;
}

auto SPC7110Decompressor::decode() -> void {
  switch (mode_) {
    case 0:  // 1 bpp: two nibbles, each a 4-level binary tree, XORed with the pixels 2 rows up
      for (unsigned half = 0; half < 2; half++) {
        const unsigned base = half * 15;
        unsigned decoded = 0;
        decode_bit(base, decoded);
        decode_bit(base + 1 + decoded, decoded);
        decode_bit(base + 3 + decoded, decoded);
        decode_bit(base + 7 + decoded, decoded);
        out_ = (out_ << 4) ^ (((out_ >> 12) ^ decoded) & 0xF);
      }
      break;
    case 1:  // 2 bpp
      for (unsigned pixel = 0; pixel < 8; pixel++) {
        const unsigned a = (out_ >> 2) & 3, b = (out_ >> 14) & 3;
        const unsigned k = neighbour_class(a, b, c_);
        unsigned decoded = 0;
        decode_bit(k, decoded);
        decode_bit(k * 2 + 5 + decoded, decoded);
        place_pixel(a, b, decoded);
      }
      break;
    default:  // 4 bpp
      for (unsigned pixel = 0; pixel < 8; pixel++) {
        const unsigned a = out_ & 15, b = (out_ >> 28) & 15;
        unsigned decoded = 0;
        decode_bit(0, decoded);
        decode_bit(decoded + 1, decoded);
        unsigned context = decoded >= 2 ? decoded + 11 : neighbour_class(a, b, c_) + 3 + decoded * 5;
        decode_bit(context, decoded);
        decode_bit(kMode2Context[context] + (decoded & 1), decoded);
        place_pixel(a, b, decoded);
      }
      break;
  }
}

auto SPC7110Decompressor::result() const -> uint32_t {
  if (mode_ == 0) return out_;
  const unsigned planes = 1u << mode_;
  uint32_t planar = 0;
  for (unsigned plane = 0; plane < planes; plane++) {
    unsigned row = 0;
    for (unsigned pixel = 0; pixel < 8; pixel++) {
      row = (row << 1) | ((out_ >> ((7 - pixel) * planes + (planes - 1 - plane))) & 1);
    }
    planar |= row << (8 * plane);
  }
  return planar;
}

namespace {

// The save-state layout predates this implementation and is kept so existing states load. It
// stores contexts as 5 neighbourhood sets x 15 tree positions, the interval as a range
// (top + 1), and the pixel history as a 64-bit shift register. Sets are ordered
// {all equal, b=c, a=c, a=b, all different}; this maps a context number to (set, position).
struct LegacySlot { Byte set; Byte position; };
constexpr std::array<Byte, 5> kLegacySet = {0, 3, 1, 2, 4};  // neighbour_class -> legacy set

constexpr auto legacy_slot(unsigned mode, unsigned context) -> LegacySlot {
  if (mode == 0) return {static_cast<Byte>(context / 15), static_cast<Byte>(context % 15)};
  if (mode == 1) {
    if (context < 5) return {kLegacySet[context], 0};
    const unsigned k = (context - 5) / 2;
    return {kLegacySet[k], static_cast<Byte>(1 + (context - 5) % 2)};
  }
  if (context < 3) return {0, static_cast<Byte>(context)};
  if (context < 13) return {kLegacySet[(context - 3) % 5], static_cast<Byte>(3 + (context - 3) / 5)};
  if (context < 15) return {0, static_cast<Byte>(context - 8)};
  if (context < 25) return {kLegacySet[(context - 15) / 2], static_cast<Byte>(7 + (context - 15) % 2)};
  return {0, static_cast<Byte>(context - 16)};
}

} // namespace

auto SPC7110Decompressor::serialize(Archive& ar) -> void {
  struct Slot { Byte state = 0; Byte invert = 0; auto serialize(Archive& a) -> void { a(state, invert); } };
  std::array<std::array<Slot, 15>, 5> slots{};
  const unsigned used = mode_ == 0 ? 30 : mode_ == 1 ? 15 : kContexts;
  for (unsigned c = 0; c < used; c++) {
    const LegacySlot at = legacy_slot(mode_, c);
    slots[at.set][at.position] = {state_[c], invert_[c]};
  }
  uint32_t bpp = 1u << mode_;
  uint32_t range = top_ + 1;
  uint32_t output = 0;
  uint64_t pixels = out_;
  if (mode_ == 2) pixels |= static_cast<uint64_t>(c_) << 32;
  uint64_t colormap = 0;
  for (unsigned i = 0; i < 16; i++) colormap |= static_cast<uint64_t>(order_[i] & 15) << (4 * i);
  uint32_t planes = result();
  ar(slots, bpp, source_, window_bits_, range, window_, output, pixels, colormap, planes);
  if (!ar.loading()) return;
  if (bpp != 1 && bpp != 2 && bpp != 4) { ar.fail(); return; }
  mode_ = bpp == 1 ? 0 : bpp == 2 ? 1 : 2;
  state_.fill(0);
  invert_.fill(0);
  for (unsigned c = 0; c < (mode_ == 0 ? 30u : mode_ == 1 ? 15u : kContexts); c++) {
    const LegacySlot at = legacy_slot(mode_, c);
    state_[c] = slots[at.set][at.position].state;
    invert_[c] = slots[at.set][at.position].invert & 1;
    if (state_[c] >= kProbability.size()) { state_[c] = 0; ar.fail(); }
  }
  top_ = (range - 1) & 0xFF;
  window_ &= 0xFFFF;
  if (window_bits_ == 0 || window_bits_ > 8) window_bits_ = 8;
  out_ = static_cast<uint32_t>(pixels);
  c_ = mode_ == 2 ? static_cast<unsigned>(pixels >> 32) & 15 : mode_ == 1 ? static_cast<unsigned>(pixels >> 16) & 3 : 0;
  for (unsigned i = 0; i < 16; i++) order_[i] = static_cast<Byte>((colormap >> (4 * i)) & 15);
}

// ---- The chip -----------------------------------------------------------------------------------

SPC7110::SPC7110(const std::vector<Byte>& rom, std::vector<Byte>& ram, bool rtc)
    : rom_(rom), ram_(ram), rtc_(rtc ? std::make_unique<RTC4513>() : nullptr),
      decompressor_([this](uint32_t address) { return data_rom_read(address); }) {
  reset();
  Logger::info(LogChannel::Rom, "SPC7110: program ROM %zu KB, data ROM %zu KB, save RAM %zu bytes, %s",
               std::min<size_t>(rom_.size(), kProgramRomSize) / 1024,
               rom_.size() > kProgramRomSize ? (rom_.size() - kProgramRomSize) / 1024 : size_t{0}, ram_.size(),
               rtc_ ? "RTC-4513" : "no RTC");
}

// $00-$3F|$80-$BF:6000-7FFF in 8 KB pages; nullopt when the board has no RAM.
auto SPC7110::ram_index(Address address) -> std::optional<uint32_t> {
  if (ram_.empty()) {
    if (log_.allow(kLogNoRam, 1)) {
      Logger::warning(LogChannel::Rom, "SPC7110: save RAM access at $%06X but the header gives no RAM", address & 0xFFFFFF);
    }
    return std::nullopt;
  }
  const auto bank = static_cast<uint32_t>((address >> 16) & 0x3F);
  return rom_mirror(bank * 0x2000 + ((address & 0xFFFF) - 0x6000u), static_cast<uint32_t>(ram_.size()));
}

// The check program keeps its pass marker ("SPC7110 CHECK OK", or $11 x 16 between its two
// test passes) in the last 16 bytes of save RAM.
auto SPC7110::log_check_area(const char* what) const -> void {
  const size_t base = ram_.size() - 16;
  std::string hex, text;
  for (size_t i = 0; i < 16; i++) {
    char b[4];
    std::snprintf(b, sizeof b, "%02X", ram_[base + i]);
    hex += b;
    text += ram_[base + i] >= 0x20 && ram_[base + i] < 0x7F ? static_cast<char>(ram_[base + i]) : '.';
  }
  Logger::info(LogChannel::Rom, "SPC7110: %s save RAM $%04zX-$%04zX: %s \"%s\"", what, base, base + 15, hex.c_str(), text.c_str());
}

auto SPC7110::reset() -> void {
  r_.fill(0);
  reg(0x4832) = 1;
  reg(0x4833) = 2;
  dcu_mode_ = 0;
  dcu_address_ = 0;
  dcu_offset_ = 0;
  dcu_tile_.fill(0);
  if (rtc_) rtc_->reset();
}

auto SPC7110::data_rom_read(uint32_t address) const -> Byte {
  if (rom_.size() <= kProgramRomSize) return 0;
  const unsigned size_select = r_[0x34] & 3;
  const uint32_t mask = (0x100000u << size_select) - 1;
  if (size_select != 3 && (address & 0x400000)) return 0;
  const auto size = static_cast<uint32_t>(rom_.size() - kProgramRomSize);
  return rom_[kProgramRomSize + rom_mirror(address & mask, size)];
}

auto SPC7110::mcu_rom_read(uint32_t linear) const -> Byte {
  const unsigned window = (linear >> 20) & 3;
  const uint32_t offset = linear & 0x0FFFFF;
  const auto program_size = static_cast<uint32_t>(std::min<size_t>(rom_.size(), kProgramRomSize));
  if (window == 0 || (window == 1 && (r_[0x34] & 4))) {  // $4834 bit 2: a 16 Mbit program ROM
    return rom_.empty() ? Byte{0} : rom_[rom_mirror((window == 1 ? 0x100000u : 0u) + offset, program_size)];
  }
  return data_rom_read(static_cast<uint32_t>(r_[0x30 + window] & 7) << 20 | offset);
}

auto SPC7110::read(Address address) -> std::optional<Byte> {
  const Byte bank = static_cast<Byte>(address >> 16);
  const auto offset = static_cast<Word>(address);
  if (bank == 0x50) return io_read(0x4800);
  if (bank == 0x58) return io_read(0x4808);
  if (bank >= 0xC0) return mcu_rom_read((bank & 0x3Fu) << 16 | offset);
  if ((bank & 0xF0) == 0x40) return mcu_rom_read((bank & 0x0Fu) << 16 | offset);
  if (bank & 0x40) return std::nullopt;
  if (offset >= 0x8000) return mcu_rom_read((bank & 0x3Fu) << 16 | offset);
  if (offset >= 0x6000) {
    const auto at = ram_index(address);
    if (!at) return std::nullopt;
    if (!(r_[0x30] & 0x80)) {  // disabled SRAM reads 0
      if (log_.allow(kLogSramDisabled, 4)) {
        Logger::info(LogChannel::Rom, "SPC7110: save RAM read at $%06X while disabled ($4830 = $%02X): 0", address & 0xFFFFFF, r_[0x30]);
      }
      return Byte{0};
    }
    if (log_.allow(kLogSramRead, 1)) Logger::info(LogChannel::Rom, "SPC7110: first save RAM read at $%06X", address & 0xFFFFFF);
    if (*at == ram_.size() - 16 && ram_.size() >= 16 && log_.allow(kLogCheckRead, 4)) log_check_area("reads check marker,");
    return ram_[*at];
  }
  if (offset >= 0x4800 && offset <= 0x483F) return io_read(offset);
  if (rtc_ && offset >= 0x4840 && offset <= 0x4842) return rtc_->read(offset - 0x4840);
  return std::nullopt;
}

auto SPC7110::write(Address address, Byte value) -> bool {
  const Byte bank = static_cast<Byte>(address >> 16);
  const auto offset = static_cast<Word>(address);
  if (bank == 0x50 || bank == 0x58) {
    io_write(bank == 0x50 ? 0x4800 : 0x4808, value);
    return true;
  }
  if (bank & 0x40) return false;
  if (offset >= 0x6000 && offset < 0x8000) {
    const auto at = ram_index(address);
    if (!at) return false;
    if (!(r_[0x30] & 0x80)) {
      if (log_.allow(kLogSramDisabled, 4)) {
        Logger::info(LogChannel::Rom, "SPC7110: save RAM write $%02X at $%06X ignored: disabled ($4830 = $%02X)", value,
                     address & 0xFFFFFF, r_[0x30]);
      }
      return true;
    }
    if (log_.allow(kLogSramWrite, 1)) Logger::info(LogChannel::Rom, "SPC7110: first save RAM write at $%06X", address & 0xFFFFFF);
    ram_[*at] = value;
    if (*at == ram_.size() - 1 && ram_.size() >= 16 && log_.allow(kLogCheckWrite, 8)) log_check_area("wrote check marker,");
    return true;
  }
  if (offset >= 0x4800 && offset <= 0x483F) {
    io_write(offset, value);
    return true;
  }
  if (rtc_ && offset >= 0x4840 && offset <= 0x4842) {
    if (log_.allow(kLogRtc, 1)) Logger::info(LogChannel::Rom, "SPC7110: first RTC-4513 access ($%04X = $%02X)", offset, value);
    rtc_->write(offset - 0x4840, value);
    return true;
  }
  return false;
}

auto SPC7110::step(uint32_t master_clocks) -> void {
  if (rtc_) rtc_->step(master_clocks);
}

auto SPC7110::io_read(unsigned address) -> std::optional<Byte> {
  switch (address & 0x3F) {
    case 0x00: {
      const auto counter = static_cast<uint16_t>((r_[0x09] | r_[0x0A] << 8) - 1);
      r_[0x09] = static_cast<Byte>(counter);
      r_[0x0A] = static_cast<Byte>(counter >> 8);
      return dcu_read();
    }
    case 0x08: return Byte{0};
    case 0x10: {
      const Byte data = r_[0x10];
      data_port_increment_4810();
      return data;
    }
    case 0x1A:
      data_port_increment(3);
      return Byte{0};
    default: break;
  }
  const unsigned r = address & 0x3F;
  if ((r >= 0x01 && r <= 0x0C) || (r >= 0x11 && r <= 0x18) || (r >= 0x20 && r <= 0x2F) || (r >= 0x30 && r <= 0x34)) {
    return r_[r];
  }
  return std::nullopt;
}

auto SPC7110::io_write(unsigned address, Byte value) -> void {
  const unsigned r = address & 0x3F;
  if (log_.allow(r, 1)) Logger::info(LogChannel::Rom, "SPC7110: first write $%04X = $%02X", 0x4800 + r, value);
  if (r == 0x30 && ((value ^ r_[0x30]) & 0x80) && log_.allow(kLogSramEnable, 16)) {
    Logger::info(LogChannel::Rom, "SPC7110: save RAM %s ($4830 = $%02X)", value & 0x80 ? "enabled" : "disabled", value);
  }
  if (r == 0x18 && log_.allow(kLogPort, 16)) {
    Logger::info(LogChannel::Rom, "SPC7110: data port mode $4818 = $%02X, offset $%06X", value & 0x7F, data_offset());
  }
  switch (r) {
    case 0x01: case 0x02: case 0x03: case 0x05: case 0x07: case 0x09: case 0x0A:
    case 0x11: case 0x12: case 0x16: case 0x17:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x26:
      r_[r] = value;
      break;
    case 0x04:  // the table entry is fetched when the index is written
      r_[r] = value;
      dcu_load_address();
      break;
    case 0x06:
      r_[r] = value;
      r_[0x0C] &= 0x7F;
      dcu_begin_transfer();
      break;
    case 0x0B: r_[r] = value & 3; break;
    case 0x13: r_[r] = value; data_port_read(); break;
    case 0x14: r_[r] = value; data_port_increment(1); break;
    case 0x15:
      r_[r] = value;
      if (r_[0x18] & 2) data_port_read();
      data_port_increment(2);
      break;
    case 0x18: r_[r] = value & 0x7F; data_port_read(); break;
    case 0x25: r_[r] = value; r_[0x2F] |= 0x81; alu_multiply(); break;
    case 0x27: r_[r] = value; r_[0x2F] |= 0x80; alu_divide(); break;
    case 0x2E: r_[r] = value & 1; break;
    case 0x30: r_[r] = value & 0x87; break;
    case 0x31: case 0x32: case 0x33: case 0x34: r_[r] = value & 7; break;
    default: break;
  }
}

// ---- Decompression unit -------------------------------------------------------------------------

auto SPC7110::dcu_load_address() -> void {
  const uint32_t table = r_[0x01] | r_[0x02] << 8 | static_cast<uint32_t>(r_[0x03]) << 16;
  const uint32_t entry = table + (static_cast<uint32_t>(r_[0x04]) << 2);
  dcu_mode_ = data_rom_read(entry);
  dcu_address_ = static_cast<uint32_t>(data_rom_read(entry + 1)) << 16 | data_rom_read(entry + 2) << 8 |
                 data_rom_read(entry + 3);
}

auto SPC7110::dcu_begin_transfer() -> void {
  if (dcu_mode_ >= 3) {  // invalid mode: nothing starts
    if (log_.allow(kLogDcuInvalid, 8)) {
      Logger::warning(LogChannel::Rom, "SPC7110: decompression table entry %u has mode %u: not started", r_[0x04], dcu_mode_);
    }
    return;
  }
  bool last = false;
  if (log_.allow(kLogDcu, 32, &last)) {
    Logger::info(LogChannel::Rom, "SPC7110: decompress entry %u (table $%06X): mode %u at $%06X, skip %u, row skip %u%s", r_[0x04],
                 r_[0x01] | r_[0x02] << 8 | static_cast<uint32_t>(r_[0x03]) << 16, dcu_mode_, dcu_address_,
                 r_[0x0B] & 2 ? static_cast<unsigned>(r_[0x05] | r_[0x06] << 8) : 0u, r_[0x0B] & 1 ? r_[0x07] : 1u,
                 last ? " (further starts not logged)" : "");
  }
  decompressor_.initialize(dcu_mode_, dcu_address_);
  decompressor_.decode();
  unsigned seek = r_[0x0B] & 2 ? r_[0x05] | r_[0x06] << 8 : 0;
  while (seek--) decompressor_.decode();
  r_[0x0C] |= 0x80;
  dcu_offset_ = 0;
}

auto SPC7110::dcu_read() -> Byte {
  if (!(r_[0x0C] & 0x80)) return 0;
  const unsigned bpp = decompressor_.bpp();
  if (dcu_offset_ == 0) {
    for (unsigned row = 0; row < 8; row++) {
      const uint32_t result = decompressor_.result();
      if (bpp == 1) {
        dcu_tile_[row] = static_cast<Byte>(result);
      } else {
        dcu_tile_[row * 2 + 0] = static_cast<Byte>(result);
        dcu_tile_[row * 2 + 1] = static_cast<Byte>(result >> 8);
        if (bpp == 4) {
          dcu_tile_[row * 2 + 16] = static_cast<Byte>(result >> 16);
          dcu_tile_[row * 2 + 17] = static_cast<Byte>(result >> 24);
        }
      }
      unsigned seek = r_[0x0B] & 1 ? r_[0x07] : 1;
      while (seek--) decompressor_.decode();
    }
  }
  const Byte data = dcu_tile_[dcu_offset_++];
  dcu_offset_ &= 8 * bpp - 1;
  return data;
}

// ---- Data port ----------------------------------------------------------------------------------

auto SPC7110::data_offset() const -> uint32_t { return r_[0x11] | r_[0x12] << 8 | static_cast<uint32_t>(r_[0x13]) << 16; }
auto SPC7110::data_adjust() const -> uint32_t { return r_[0x14] | r_[0x15] << 8; }
auto SPC7110::data_stride() const -> uint32_t { return r_[0x16] | r_[0x17] << 8; }

auto SPC7110::set_data_offset(uint32_t value) -> void {
  r_[0x11] = static_cast<Byte>(value);
  r_[0x12] = static_cast<Byte>(value >> 8);
  r_[0x13] = static_cast<Byte>(value >> 16);
}

auto SPC7110::set_data_adjust(uint32_t value) -> void {
  r_[0x14] = static_cast<Byte>(value);
  r_[0x15] = static_cast<Byte>(value >> 8);
}

auto SPC7110::data_port_read() -> void {
  uint32_t adjust = r_[0x18] & 2 ? data_adjust() : 0;
  if (r_[0x18] & 8) adjust = sign16(adjust);
  r_[0x10] = data_rom_read((data_offset() + adjust) & 0xFFFFFF);
}

auto SPC7110::data_port_increment_4810() -> void {
  uint32_t stride = r_[0x18] & 1 ? data_stride() : 1;
  uint32_t adjust = data_adjust();
  if (r_[0x18] & 4) stride = sign16(stride);
  if (r_[0x18] & 8) adjust = sign16(adjust);
  if (r_[0x18] & 16) {
    set_data_adjust(adjust + stride);
  } else {
    set_data_offset(data_offset() + stride);
  }
  data_port_read();
}

auto SPC7110::data_port_increment(unsigned trigger) -> void {
  if ((r_[0x18] >> 5) != trigger) return;
  uint32_t adjust = data_adjust();
  if (r_[0x18] & 8) adjust = sign16(adjust);
  set_data_offset(data_offset() + adjust);
  data_port_read();
}

// ---- ALU ----------------------------------------------------------------------------------------

auto SPC7110::alu_multiply() -> void {
  const uint32_t a = r_[0x24] | r_[0x25] << 8;
  const uint32_t b = r_[0x20] | r_[0x21] << 8;
  uint32_t product = 0;
  if (r_[0x2E] & 1) {
    product = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(a)) * static_cast<int32_t>(static_cast<int16_t>(b)));
  } else {
    product = a * b;
  }
  for (unsigned i = 0; i < 4; i++) r_[0x28 + i] = static_cast<Byte>(product >> (8 * i));
  r_[0x2F] &= 0x7F;
}

auto SPC7110::alu_divide() -> void {
  const uint32_t dividend = r_[0x20] | r_[0x21] << 8 | r_[0x22] << 16 | static_cast<uint32_t>(r_[0x23]) << 24;
  const uint32_t divisor = r_[0x26] | r_[0x27] << 8;
  uint32_t quotient = 0;
  uint32_t remainder = dividend;  // division by zero: quotient 0, remainder = dividend
  if (r_[0x2E] & 1) {
    const auto n = static_cast<int32_t>(dividend);
    const auto d = static_cast<int32_t>(static_cast<int16_t>(divisor));
    if (d == -1 && n == INT32_MIN) {
      quotient = dividend;
      remainder = 0;
    } else if (d != 0) {
      quotient = static_cast<uint32_t>(n / d);
      remainder = static_cast<uint32_t>(n % d);
    }
  } else if (divisor != 0) {
    quotient = dividend / divisor;
    remainder = dividend % divisor;
  }
  for (unsigned i = 0; i < 4; i++) r_[0x28 + i] = static_cast<Byte>(quotient >> (8 * i));
  r_[0x2C] = static_cast<Byte>(remainder);
  r_[0x2D] = static_cast<Byte>(remainder >> 8);
  r_[0x2F] &= 0x7F;
}

auto SPC7110::serialize(Archive& ar) -> void {
  ar(r_, dcu_mode_, dcu_address_, dcu_offset_, dcu_tile_);
  decompressor_.serialize(ar);
  bool has_rtc = rtc_ != nullptr;
  ar(has_rtc);
  if (has_rtc != (rtc_ != nullptr)) {  // a state from the other board variant
    ar.fail();
    return;
  }
  if (rtc_) rtc_->serialize(ar);
  if (ar.loading() && dcu_offset_ >= 32) ar.fail();
}

} // namespace snes::coprocessor
