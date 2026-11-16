// SPDX-License-Identifier: MIT
#include "snes/coprocessor/sdd1.hpp"

#include "utils/logging.hpp"

namespace snes::coprocessor {

// Tables from fullsnes ("SNES Cart S-DD1 Decompression Algorithm").
const std::array<Byte, 33> SDD1Decompressor::kCodeSize = {
  0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
  4, 4, 5, 5, 6, 6, 7, 7, 0, 1, 2, 3, 4, 5, 6, 7,
};
const std::array<Byte, 33> SDD1Decompressor::kMpsNext = {
  25, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
  18, 19, 20, 21, 22, 23, 24, 24, 26, 27, 28, 29, 30, 31, 32, 24,
};
const std::array<Byte, 33> SDD1Decompressor::kLpsNext = {
  25, 1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
  16, 17, 18, 19, 20, 21, 22, 23, 1, 2, 4, 8, 12, 16, 18, 22,
};
const std::array<Byte, 128> SDD1Decompressor::kRunTable = {
  128, 64, 96, 32, 112, 48, 80, 16, 120, 56, 88, 24, 104, 40, 72,  8,
  124, 60, 92, 28, 108, 44, 76, 12, 116, 52, 84, 20, 100, 36, 68,  4,
  126, 62, 94, 30, 110, 46, 78, 14, 118, 54, 86, 22, 102, 38, 70,  6,
  122, 58, 90, 26, 106, 42, 74, 10, 114, 50, 82, 18,  98, 34, 66,  2,
  127, 63, 95, 31, 111, 47, 79, 15, 119, 55, 87, 23, 103, 39, 71,  7,
  123, 59, 91, 27, 107, 43, 75, 11, 115, 51, 83, 19,  99, 35, 67,  3,
  125, 61, 93, 29, 109, 45, 77, 13, 117, 53, 85, 21, 101, 37, 69,  5,
  121, 57, 89, 25, 105, 41, 73,  9, 113, 49, 81, 17,  97, 33, 65,  1,
};

auto SDD1Decompressor::init(uint32_t address) -> void {
  const Byte header = read_(address);
  static constexpr Byte kPlanes[4] = {2, 8, 4, 0};
  static constexpr uint16_t kHigh[4] = {0x01C0, 0x0180, 0x00C0, 0x0180};
  static constexpr uint16_t kLow[4] = {0x0001, 0x0001, 0x0001, 0x0003};
  planes_ = kPlanes[header >> 6];
  high_context_mask_ = kHigh[(header >> 4) & 3];
  low_context_mask_ = kLow[(header >> 4) & 3];
  bit_address_ = address * 8 + 4;  // the stream starts right after the 4 header bits
  counter_.fill(0);
  state_.fill(0);
  mps_.fill(0);
  history_.fill(0);
  plane_ = 0;
  row_ = 0;
}

auto SDD1Decompressor::input_bit() -> unsigned {
  const Byte byte = read_(bit_address_ >> 3);
  const unsigned bit = (byte >> (7 - (bit_address_ & 7))) & 1;
  bit_address_++;
  return bit;
}

// A codeword is "0" (2^size MPS symbols) or "1" followed by `size` bits that index the run
// table. The returned counter is decremented once per symbol.
auto SDD1Decompressor::load_counter(Byte code_size) -> Byte {
  if (!input_bit()) return static_cast<Byte>(0x80 + (1 << code_size));  // 0x80 + 128 wraps to 0
  unsigned index = 0;
  for (Byte i = 0; i < code_size; i++) index = (index << 1) | input_bit();
  index = (index << (7 - code_size)) | (0x7Fu >> code_size);
  return kRunTable[index];
}

auto SDD1Decompressor::decode_symbol(Byte context) -> unsigned {
  const Byte state = state_[context];
  Byte& counter = counter_[kCodeSize[state]];
  if ((counter & 0x7F) == 0) counter = load_counter(kCodeSize[state]);
  unsigned symbol = mps_[context];
  counter--;
  if (counter == 0) {  // the run ended in the less probable symbol
    symbol ^= 1;
    if (state < 2) mps_[context] = static_cast<Byte>(symbol);
    state_[context] = kLpsNext[state];
  } else if (counter == 0x80) {  // a run of only MPS symbols is used up
    state_[context] = kMpsNext[state];
  }
  return symbol;
}

auto SDD1Decompressor::decode_plane_bit(Byte plane) -> unsigned {
  uint16_t& history = history_[plane];
  const auto context = static_cast<Byte>(((plane & 1) << 4) | ((history & high_context_mask_) >> 5) |
                                         (history & low_context_mask_));
  const unsigned bit = decode_symbol(context);
  history = static_cast<uint16_t>((history << 1) | bit);
  return bit;
}

auto SDD1Decompressor::read() -> Byte {
  if (planes_ == 0) {  // eight planes, one bit each, packed LSB first
    unsigned value = 0;
    for (Byte plane = 0; plane < 8; plane++) value |= decode_plane_bit(plane) << plane;
    return static_cast<Byte>(value);
  }
  if ((plane_ & 1) == 0) {  // decode a row of two interleaved planes, return the first
    for (int i = 0; i < 8; i++) {
      decode_plane_bit(plane_);
      decode_plane_bit(static_cast<Byte>(plane_ + 1));
    }
    return static_cast<Byte>(history_[plane_++]);
  }
  const auto value = static_cast<Byte>(history_[plane_--]);  // the second plane's byte
  if (++row_ == 8) {
    row_ = 0;
    plane_ = static_cast<Byte>((plane_ + 2) & (planes_ - 1));
  }
  return value;
}

// The save-state layout predates this implementation and is kept so existing states load:
// byte offset and bit, per-code-size (MPS run, LPS pending), per-context state and MPS, the
// header mode bits, a running bit count, the last plane used, plane histories, and the
// pending-byte flag/value of the 2-plane output.
auto SDD1Decompressor::serialize(Archive& ar) -> void {
  uint32_t offset = bit_address_ >> 3;
  auto bit = static_cast<Byte>(bit_address_ & 7);
  std::array<Byte, 8> run{};
  std::array<bool, 8> lps{};
  for (size_t i = 0; i < 8; i++) {
    const Byte c = counter_[i];
    if (c & 0x80) run[i] = c & 0x7F;              // MPS-only run, 0x80 = finished
    else if (c != 0) { run[i] = static_cast<Byte>(c - 1); lps[i] = true; }
  }
  Byte mode = planes_ == 2 ? 0x00 : planes_ == 8 ? 0x40 : planes_ == 4 ? 0x80 : 0xC0;
  Byte context_mode = high_context_mask_ == 0x01C0 ? 0x00
                    : high_context_mask_ == 0x00C0 ? 0x20
                    : low_context_mask_ == 0x0003 ? 0x30 : 0x10;
  Byte bit_count = 0, last_plane = 0, pending = 0xFF, pending_value = 0, scratch = 0;
  if (planes_ == 0) {
    last_plane = 7;
  } else if (plane_ & 1) {  // second byte waiting
    pending = 0;
    pending_value = static_cast<Byte>(history_[plane_]);
    last_plane = plane_;
    bit_count = static_cast<Byte>(((row_ + 1) * 16) & 0x7F);
  } else {
    bit_count = static_cast<Byte>(row_ * 16);
    if (row_ != 0) last_plane = static_cast<Byte>(plane_ | 1);
    else if (planes_ == 2) last_plane = 1;
    else if (planes_ == 4) last_plane = static_cast<Byte>(plane_ ^ 3);
    else last_plane = static_cast<Byte>(((plane_ + 6) & 7) | 1);
  }
  ar(offset, bit, run, lps, state_, mps_, mode, context_mode, bit_count, last_plane, history_, pending, scratch,
     pending_value);
  if (!ar.loading()) return;
  bit_address_ = offset * 8 + (bit & 7);
  for (size_t i = 0; i < 8; i++) {
    if (lps[i]) counter_[i] = static_cast<Byte>(run[i] + 1);
    else counter_[i] = run[i] ? static_cast<Byte>(0x80 | run[i]) : Byte{0};
  }
  for (auto& s : state_) if (s > 32) { s = 0; ar.fail(); }
  for (auto& m : mps_) m &= 1;
  static constexpr Byte kPlanesOf[4] = {2, 8, 4, 0};
  planes_ = kPlanesOf[(mode >> 6) & 3];
  static constexpr uint16_t kHigh[4] = {0x01C0, 0x0180, 0x00C0, 0x0180};
  static constexpr uint16_t kLow[4] = {0x0001, 0x0001, 0x0001, 0x0003};
  high_context_mask_ = kHigh[(context_mode >> 4) & 3];
  low_context_mask_ = kLow[(context_mode >> 4) & 3];
  const unsigned within = bit_count & 0x7F;
  if (planes_ == 0) {
    plane_ = 0;
    row_ = 0;
  } else if (pending == 0) {
    plane_ = static_cast<Byte>(last_plane | 1);
    row_ = static_cast<Byte>(((within >> 4) + 7) & 7);
  } else {
    row_ = static_cast<Byte>(within >> 4);
    unsigned next = (last_plane ^ 1u) & 7;
    if (row_ == 0) next = planes_ == 4 ? (next ^ 2u) : (next + 2u);
    plane_ = static_cast<Byte>(next & (planes_ - 1u) & ~1u);
  }
}

// ---- The chip -----------------------------------------------------------------------------------

namespace {

using utils::LogChannel;
using utils::Logger;

// Log keys (LogLimiter slots).
enum : size_t { kLogDecompress, kLogMmc, kLogEnable, kLogRamWindow, kLogRamBank, kLogRamMissing };

} // namespace

SDD1::SDD1(const std::vector<Byte>& rom, std::vector<Byte>& ram)
    : rom_(rom), ram_(ram), decompressor_([this](uint32_t address) { return mmc_read(address); }) {
  reset();
  Logger::info(LogChannel::Rom, "S-DD1: %zu KB ROM, %zu bytes save RAM at $00-$3F:6000-7FFF and $70-$73", rom_.size() / 1024,
               ram_.size());
}

// $00-$3F|$80-$BF:6000-7FFF (8 KB per bank) and $70-$73:0000-FFFF, mirrored over the RAM size.
auto SDD1::ram_offset(Address address) -> std::optional<size_t> {
  const Byte bank = static_cast<Byte>(address >> 16);
  const auto offset = static_cast<Word>(address);
  size_t linear = 0;
  if (!(bank & 0x40) && offset >= 0x6000 && offset < 0x8000) {
    linear = static_cast<size_t>(bank & 0x3F) * 0x2000 + (offset - 0x6000u);
  } else if (bank >= 0x70 && bank <= 0x73) {
    linear = static_cast<size_t>(bank - 0x70) << 16 | offset;
  } else {
    return std::nullopt;
  }
  if (ram_.empty()) {
    if (log_.allow(kLogRamMissing, 1)) {
      Logger::warning(LogChannel::Rom, "S-DD1: save RAM access at $%06X but the header gives no RAM", address & 0xFFFFFF);
    }
    return std::nullopt;
  }
  if (log_.allow(bank >= 0x70 && bank <= 0x73 ? kLogRamBank : kLogRamWindow, 1)) {
    Logger::info(LogChannel::Rom, "S-DD1: first save RAM access at $%06X", address & 0xFFFFFF);
  }
  return linear % ram_.size();
}

auto SDD1::reset() -> void {
  regs_ = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03};
  dma_ = {};
  dma_ready_ = false;
}

auto SDD1::mmc_read(uint32_t address) const -> Byte {
  const Byte bank = regs_[4 + ((address >> 20) & 3)] & 0x0F;
  return rom_at(static_cast<uint32_t>(bank) << 20 | (address & 0x0FFFFF));
}

auto SDD1::read(Address address) -> std::optional<Byte> {
  const Byte bank = static_cast<Byte>(address >> 16);
  const auto offset = static_cast<Word>(address);
  if (auto at = ram_offset(address)) return ram_[*at];
  if (!(bank & 0x40)) {  // $00-$3F, $80-$BF
    if (offset >= 0x4800 && offset <= 0x480F) {  // unused registers read the ROM byte
      const unsigned reg = offset & 0xF;
      return reg < 8 && reg != 2 && reg != 3 ? regs_[reg] : rom_at(offset);
    }
    if (offset < 0x8000) return std::nullopt;
    uint32_t a = address & 0xFFFFFF;
    // $20-$3F (bit 7 of $4805) and $A0-$BF ($4807) fold onto the first 2 MB.
    if ((a & 0x200000) && (((a & 0x800000) == 0 && (regs_[5] & 0x80)) || ((a & 0x800000) && (regs_[7] & 0x80)))) {
      a &= ~0x200000u;
    }
    return rom_at(((a >> 16) & 0x3F) << 15 | (a & 0x7FFF));
  }
  if (bank < 0xC0) return std::nullopt;  // $40-$6F, $74-$7D: the cartridge's LoROM map
  const uint32_t a = address & 0xFFFFFF;
  if (regs_[0] & regs_[1]) {
    for (int n = 0; n < 8; n++) {
      const Byte bit = static_cast<Byte>(1 << n);
      if (!(regs_[0] & regs_[1] & bit) || a != dma_[static_cast<size_t>(n)].address) continue;
      if (!dma_ready_) {
        decompressor_.init(a);
        dma_ready_ = true;
        bool last = false;
        if (log_.allow(kLogDecompress, 64, &last)) {
          const Byte header = mmc_read(a);
          Logger::info(LogChannel::Rom, "S-DD1: DMA ch%d decompresses $%06X (ROM $%06X, banks %X/%X/%X/%X), %u bytes, mode %d/%d%s", n, a,
                       static_cast<unsigned>((regs_[4 + ((a >> 20) & 3)] & 0x0F)) << 20 | (a & 0x0FFFFF),
                       regs_[4] & 0x0F, regs_[5] & 0x0F, regs_[6] & 0x0F, regs_[7] & 0x0F,
                       dma_[static_cast<size_t>(n)].size ? dma_[static_cast<size_t>(n)].size : 0x10000u, header >> 6,
                       (header >> 4) & 3, last ? " (further starts not logged)" : "");
        }
      }
      const Byte data = decompressor_.read();
      if (--dma_[static_cast<size_t>(n)].size == 0) {
        dma_ready_ = false;
        regs_[1] &= static_cast<Byte>(~bit);
      }
      return data;
    }
  }
  return mmc_read(a);
}

auto SDD1::write(Address address, Byte value) -> bool {
  if (auto at = ram_offset(address)) {
    ram_[*at] = value;
    return true;
  }
  const auto offset = static_cast<Word>(address);
  if ((address & 0x400000) || offset < 0x4800 || offset > 0x480F) return false;
  const int reg = offset & 0xF;
  if (reg == 0 && value != regs_[0] && log_.allow(kLogEnable, 16)) {
    Logger::info(LogChannel::Rom, "S-DD1: $4800 = $%02X (decompression channels)", value);
  }
  bool last = false;
  if (reg >= 4 && reg < 8 && (value & 0x8F) != regs_[static_cast<size_t>(reg)] && log_.allow(kLogMmc, 64, &last)) {
    Logger::info(LogChannel::Rom, "S-DD1: $%04X = $%02X (bank %d)%s", offset, value, value & 0x0F,
                 last ? " (further bank switches not logged)" : "");
  }
  if (reg == 0 || reg == 1) regs_[static_cast<size_t>(reg)] = value;
  if (reg >= 4 && reg < 8) regs_[static_cast<size_t>(reg)] = value & 0x8F;
  return true;
}

auto SDD1::dma_register_write(Byte channel, Byte reg, Byte value) -> void {
  Dma& d = dma_[channel & 7];
  switch (reg) {
    case 2: d.address = (d.address & 0xFFFF00) | value; break;
    case 3: d.address = (d.address & 0xFF00FF) | static_cast<uint32_t>(value) << 8; break;
    case 4: d.address = (d.address & 0x00FFFF) | static_cast<uint32_t>(value) << 16; break;
    case 5: d.size = static_cast<uint16_t>((d.size & 0xFF00) | value); break;
    case 6: d.size = static_cast<uint16_t>((d.size & 0x00FF) | value << 8); break;
    default: break;
  }
}

auto SDD1::serialize(Archive& ar) -> void {
  ar(regs_, dma_, dma_ready_);
  decompressor_.serialize(ar);
}

} // namespace snes::coprocessor
