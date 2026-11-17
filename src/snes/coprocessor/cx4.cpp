// SPDX-License-Identifier: MIT
#include "snes/coprocessor/cx4.hpp"
#include "snes/memory/rom_mirror.hpp"

#include <algorithm>
#include <utility>

namespace bitforge::coprocessor {

namespace {

// NTSC master clock = 236.25 MHz / 11: one master clock is worth kClockHz * 11 budget units and
// one HG51B clock costs kMasterUnit, so the 20 MHz / 21.477 MHz ratio is exact (no drift).
constexpr int64_t kMasterUnit = 236'250'000;
constexpr uint32_t kMask24 = 0xFFFFFF;
constexpr uint8_t kShifts[4] = {0, 1, 8, 16};

auto sign24(uint32_t value) -> int64_t {
  value &= kMask24;
  return (value & 0x800000) ? static_cast<int64_t>(value) - 0x1000000 : static_cast<int64_t>(value);
}

// Byte `n` (0 = low) of a multi-byte register.
constexpr auto byte_of(uint32_t value, unsigned n) -> Byte { return static_cast<Byte>(value >> (8 * n)); }
template <class T>
auto set_byte_of(T& field, unsigned n, Byte value) -> void {
  field = static_cast<T>((field & ~(T{0xFF} << (8 * n))) | (static_cast<T>(value) << (8 * n)));
}

} // namespace

Cx4::Cx4(const std::vector<Byte>& rom, std::vector<Byte>& ram, std::span<const Byte> data_rom)
    : rom_(rom), ram_(ram) {
  for (size_t n = 0; n < data_rom_.size() && n * 3 + 2 < data_rom.size(); n++) {
    data_rom_[n] = data_rom[n * 3] | (data_rom[n * 3 + 1] << 8) | (data_rom[n * 3 + 2] << 16);
  }
  reset();
}

auto Cx4::reset() -> void {
  pages_ = {};
  page_ = 0;
  load_pending_ = false;
  load_forced_ = false;
  code_base_ = 0;
  start_bank_ = 0;
  start_pc_ = 0;
  pb_ = 0; pc_ = 0;
  n_ = z_ = c_ = v_ = i_ = false;
  a_ = 0; p_ = 0; mul_ = 0;
  mdr_ = rom_buffer_ = ram_buffer_ = mar_ = dpr_ = 0;
  gpr_ = {};
  stack_ = {};
  locked_up_ = false;
  halted_ = true;
  irq_disable_ = false;
  irq_line_ = false;
  rom_config_ = 1;
  vector_ = {};
  rom_wait_ = 3; ram_wait_ = 3;
  sleeping_ = false;
  sleep_clocks_ = 0;
  copy_ = {};
  ext_ = {};
  clock_budget_ = 0;
  unit_clocks_ = 0;
  clocks_ = 0;
}

// ---- HG51B external bus ------------------------------------------------------------------------

auto Cx4::rom_linear(uint32_t address) const -> std::optional<uint32_t> {
  // 00-3F,80-BF:8000-FFFF and C0-FF:0000-FFFF, LoROM layout.
  if ((address & 0x408000) == 0x008000 || (address & 0xC00000) == 0xC00000) {
    return (((address & 0x3F0000) >> 1) | (address & 0x7FFF)) & 0x1FFFFF;
  }
  return std::nullopt;
}

auto Cx4::ram_linear(uint32_t address) const -> std::optional<uint32_t> {
  // 70-77:0000-7FFF
  if ((address & 0xF88000) == 0x700000) return (((address & 0x070000) >> 1) | (address & 0x7FFF)) & 0x3FFFF;
  return std::nullopt;
}

// Clocks one external byte access costs: 1 plus the programmed wait states ($7F50).
auto Cx4::access_clocks(uint32_t address) const -> uint32_t {
  if (rom_linear(address)) return 1u + rom_wait_;
  if (ram_linear(address)) return 1u + ram_wait_;
  return 0;
}

auto Cx4::bus_read(uint32_t address) -> Byte {
  address &= kMask24;
  if (auto at = rom_linear(address)) return rom_.empty() ? 0 : rom_[rom_mirror(*at, static_cast<uint32_t>(rom_.size()))];
  if (auto at = ram_linear(address)) return ram_.empty() ? 0 : ram_[rom_mirror(*at, static_cast<uint32_t>(ram_.size()))];
  if ((address & 0x40E000) == 0x006000) {
    if ((address & 0x0C00) != 0x0C00) return dram_[address & 0xFFF];
    return io_read(static_cast<Word>(address));
  }
  return 0x00;
}

auto Cx4::bus_write(uint32_t address, Byte value) -> void {
  address &= kMask24;
  if (rom_linear(address)) return;
  if (auto at = ram_linear(address)) {
    if (!ram_.empty()) ram_[rom_mirror(*at, static_cast<uint32_t>(ram_.size()))] = value;
    return;
  }
  if ((address & 0x40E000) == 0x006000) {
    if ((address & 0x0C00) != 0x0C00) dram_[address & 0xFFF] = value;
    else io_write(static_cast<Word>(address), value);
  }
}

// Lets `clocks` HG51B clocks pass. An external MAR/MDR access started through registers $2E/$2F
// lands when its wait states have run out.
auto Cx4::elapse(uint32_t clocks) -> void {
  unit_clocks_ += clocks;
  clocks_ += clocks;
  if (ext_.kind == External::Idle) return;
  if (ext_.wait > clocks) {
    ext_.wait = static_cast<uint8_t>(ext_.wait - clocks);
    return;
  }
  const External::Kind kind = ext_.kind;
  ext_.kind = External::Idle;  // the address register keeps its value
  ext_.wait = 0;
  if (kind == External::Read) mdr_ = bus_read(ext_.address);
  else bus_write(ext_.address, static_cast<Byte>(mdr_));
}

auto Cx4::begin_external(External::Kind kind, unsigned reg) -> void {
  ext_.kind = kind;
  ext_.wait = reg == 0x2E ? rom_wait_ : ram_wait_;
  ext_.address = mar_;
}

// Program end, or a stop: raise the IRQ unless it's masked ($7F51).
auto Cx4::halt() -> void {
  halted_ = true;
  if (irq_disable_) return;
  i_ = true;
  irq_line_ = true;
}

// ---- scheduling --------------------------------------------------------------------------------

auto Cx4::step(uint32_t master_clocks) -> void {
  clock_budget_ += static_cast<int64_t>(master_clocks) * kClockHz * 11;
  while (clock_budget_ > 0) {
    if (!locked_up_ && !sleeping_ && !running()) {  // idle: nothing changes until the S-CPU acts
      const int64_t idle = (clock_budget_ + kMasterUnit - 1) / kMasterUnit;
      clocks_ += static_cast<uint64_t>(idle);
      clock_budget_ -= idle * kMasterUnit;
      break;
    }
    clock_budget_ -= static_cast<int64_t>(run_one()) * kMasterUnit;
  }
}

// One unit of work, in priority order: a locked-up chip and a suspended one only burn clocks;
// then a requested page load, a block copy, and finally the program itself.
auto Cx4::run_one() -> uint32_t {
  unit_clocks_ = 0;
  if (locked_up_) {
    elapse(1);
  } else if (sleeping_) {
    if (sleep_clocks_ == 0) {
      elapse(1);  // suspended until $7F5D
    } else {
      elapse(sleep_clocks_);
      sleep_clocks_ = 0;
      sleeping_ = false;
    }
  } else if (load_pending_) {
    ensure_page();
  } else if (copy_.active) {
    block_copy();
  } else if (halted_) {
    elapse(1);
  } else {
    execute();
  }
  return unit_clocks_;
}

// Steps the PC. Running off the end of the first page continues in the second page with bank P;
// running off the second page (or into a locked or unloadable one) ends the program.
auto Cx4::next_pc() -> void {
  pc_ = static_cast<uint8_t>(pc_ + 1);
  if (pc_ != 0) return;
  const bool can_continue = page_ == 0 && !pages_[1].locked;
  page_ = 1;
  if (!can_continue) return halt();
  pb_ = p_;
  if (!ensure_page()) halt();
}

auto Cx4::code_origin() const -> uint32_t { return (code_base_ + pb_ * 512u) & kMask24; }

// Index of the page already holding `origin`, starting with the one in use.
auto Cx4::find_page(uint32_t origin) const -> std::optional<uint8_t> {
  for (uint8_t k : {page_, static_cast<uint8_t>(page_ ^ 1)}) {
    if (pages_[k].origin == origin) return k;
  }
  return std::nullopt;
}

// Copies 256 instruction words (512 bytes, each byte paying the bus wait states) into a page.
auto Cx4::load_page(uint8_t index, uint32_t origin) -> void {
  ProgramPage& page = pages_[index];
  page.origin = origin;
  uint32_t address = origin;
  for (uint16_t& word : page.code) {
    Byte bytes[2];
    for (Byte& b : bytes) {
      elapse(access_clocks(address));
      b = bus_read(address);
      address = (address + 1) & kMask24;
    }
    word = static_cast<uint16_t>(bytes[0] | bytes[1] << 8);
  }
}

// Makes the current program bank (PB) executable. A hit in either page just switches to it; on
// a miss the bank replaces the page not in use, or the current one if the other is locked.
// Fails when both pages are locked. A forced load ($7F48) skips the lookup.
auto Cx4::ensure_page() -> bool {
  const uint32_t origin = code_origin();
  bool ok = true;
  if (load_forced_) {
    load_forced_ = false;
    load_page(page_, origin);  // busy() stays true while the load runs
  } else if (const auto hit = find_page(origin)) {
    page_ = *hit;
  } else {
    const uint8_t other = static_cast<uint8_t>(page_ ^ 1);
    if (!pages_[other].locked) page_ = other;
    if (pages_[page_].locked) ok = false;
    else load_page(page_, origin);
  }
  load_pending_ = false;
  return ok;
}

// DMA: copy `count` bytes between the cartridge buses and data RAM. ROM->ROM and RAM->RAM
// copies can't be done on one bus; the chip locks up until $7F53.
auto Cx4::block_copy() -> void {
  for (uint32_t i = 0; i < copy_.count; i++) {
    const uint32_t from = (copy_.from + i) & kMask24;
    const uint32_t to = (copy_.to + i) & kMask24;
    const bool same_bus = (rom_linear(from) && rom_linear(to)) || (ram_linear(from) && ram_linear(to));
    if (same_bus) {
      locked_up_ = true;
      return;
    }
    elapse(access_clocks(from));
    const Byte data = bus_read(from);
    elapse(access_clocks(to));
    bus_write(to, data);
  }
  copy_.active = false;
}

// ---- registers ---------------------------------------------------------------------------------

// Fixed values readable as registers $50-$5F.
constexpr std::array<uint32_t, 16> kConstants = {
  0x000000, 0xFFFFFF, 0x00FF00, 0xFF0000, 0x00FFFF, 0xFFFF00, 0x800000, 0x7FFFFF,
  0x008000, 0x007FFF, 0xFF7FFF, 0xFFFF7F, 0x010000, 0xFEFFFF, 0x000100, 0x00FEFF,
};

auto Cx4::reg_read(unsigned r) -> uint32_t {
  r &= 0x7F;
  if (r >= 0x60) return gpr_[r & 15];
  if (r >= 0x50) return kConstants[r & 15];
  switch (r) {
    case 0x01: return static_cast<uint32_t>(mul_ >> 24) & kMask24;
    case 0x02: return static_cast<uint32_t>(mul_) & kMask24;
    case 0x03: return mdr_;
    case 0x08: return rom_buffer_;
    case 0x0C: return ram_buffer_;
    case 0x13: return mar_;
    case 0x1C: return dpr_;
    case 0x20: return pc_;
    case 0x28: return p_;
    case 0x2E: case 0x2F:  // reading these starts an external read from MAR into MDR
      begin_external(External::Read, r);
      return 0;
    default: return 0;
  }
}

auto Cx4::reg_write(unsigned r, uint32_t value) -> void {
  r &= 0x7F;
  value &= kMask24;
  if (r >= 0x60) {
    gpr_[r & 15] = value;
    return;
  }
  switch (r) {
    case 0x01: mul_ = (mul_ & 0xFFFFFFull) | (static_cast<uint64_t>(value) << 24); break;
    case 0x02: mul_ = (mul_ & ~0xFFFFFFull) | value; break;
    case 0x03: mdr_ = value; break;
    case 0x08: rom_buffer_ = value; break;
    case 0x0C: ram_buffer_ = value; break;
    case 0x13: mar_ = value; break;
    case 0x1C: dpr_ = value; break;
    case 0x20: pc_ = static_cast<uint8_t>(value); break;
    case 0x28: p_ = static_cast<uint16_t>(value & 0x7FFF); break;
    case 0x2E: case 0x2F: begin_external(External::Write, r); break;  // MDR -> [MAR]
    default: break;
  }
}

// Eight-entry return stack of (bank << 8 | pc); the oldest entry falls off.
auto Cx4::push() -> void {
  std::copy_backward(stack_.begin(), stack_.end() - 1, stack_.end());
  stack_[0] = (static_cast<uint32_t>(pb_) << 8) | pc_;
}

auto Cx4::pull() -> void {
  const uint32_t top = stack_[0];
  std::copy(stack_.begin() + 1, stack_.end(), stack_.begin());
  stack_.back() = 0;
  pb_ = static_cast<uint16_t>((top >> 8) & 0x7FFF);
  pc_ = static_cast<uint8_t>(top);
}

// ---- ALU ---------------------------------------------------------------------------------------

auto Cx4::set_nz(uint32_t value) -> uint32_t {
  value &= kMask24;
  n_ = value & 0x800000;
  z_ = value == 0;
  return value;
}

auto Cx4::add(uint32_t x, uint32_t y) -> uint32_t {
  x &= kMask24; y &= kMask24;
  const uint32_t z = x + y;
  c_ = z > kMask24;
  v_ = (~(x ^ y) & (x ^ z) & 0x800000) != 0;
  return set_nz(z);
}

auto Cx4::sub(uint32_t x, uint32_t y) -> uint32_t {
  x &= kMask24; y &= kMask24;
  const int32_t z = static_cast<int32_t>(x) - static_cast<int32_t>(y);
  c_ = z >= 0;  // no borrow
  v_ = (~(x ^ y) & (x ^ static_cast<uint32_t>(z)) & 0x800000) != 0;
  return set_nz(static_cast<uint32_t>(z));
}

// Data RAM is 3 KB; addresses $C00-$FFF fold back onto $800-$BFF.
auto Cx4::dram_index(uint32_t address) const -> uint32_t {
  address &= 0xFFF;
  return address >= 0xC00 ? address - 0x400 : address;
}

// ---- instruction set ---------------------------------------------------------------------------
//
// 16-bit opcodes, decoded on the top six bits (and the next two where a group splits):
//   reg forms take a 7-bit register number (o & $7F): $01/$02 multiplier hi/lo, $03 MDR, $08 ROM
//   buffer, $0C RAM buffer, $13 MAR, $1C DPR, $20 PC, $28 P, $2E/$2F bus transfer, $50-$5F
//   constants, $60-$7F GPRs. imm forms take 8 bits. ALU ops shift A left by 0/1/8/16 (bits 8-9)
//   before the operation.
auto Cx4::execute() -> void {
  if (!ensure_page()) return halt();
  const uint16_t o = pages_[page_].code[pc_];
  next_pc();
  elapse(1);

  const unsigned group = o >> 10;
  const unsigned sub8 = (o >> 8) & 3;
  const unsigned reg = o & 0x7F;
  const uint32_t imm = o & 0xFF;
  const uint32_t shifted = (a_ << kShifts[sub8]) & kMask24;
  auto operand = [&](bool immediate) { return immediate ? imm : reg_read(reg); };
  auto jump = [&](bool take, bool call) {
    if (!take) return;
    if (call) push();
    if (o & 0x200) pb_ = p_;  // far: the bank comes from P
    pc_ = static_cast<uint8_t>(imm);
    elapse(2);
  };
  auto skip = [&](bool flag) {  // skip the next instruction when flag == bit 0
    if (flag != static_cast<bool>(o & 1)) return;
    next_pc();
    elapse(1);
  };
  auto shift_amount = [&](bool immediate) {
    unsigned s = (immediate ? o : reg_read(reg)) & 31;
    return s > 24 ? 0u : s;
  };
  auto rdram = [&](uint32_t address) {
    const unsigned byte = (o >> 8) & 3;
    ram_buffer_ = (ram_buffer_ & ~(0xFFu << (byte * 8))) | (static_cast<uint32_t>(dram_[dram_index(address)]) << (byte * 8));
  };
  auto wrram = [&](uint32_t address) {
    const unsigned byte = (o >> 8) & 3;
    dram_[dram_index(address)] = static_cast<Byte>(ram_buffer_ >> (byte * 8));
  };

  switch (group) {
    case 0x02: jump(true, false); break;
    case 0x03: jump(z_, false); break;
    case 0x04: jump(c_, false); break;
    case 0x05: jump(n_, false); break;
    case 0x06: jump(v_, false); break;
    case 0x07: if (ext_.kind != External::Idle) elapse(ext_.wait); break;  // WAIT for the bus transfer
    case 0x09:
      switch (sub8) {
        case 0: skip(v_); break;
        case 1: skip(c_); break;
        case 2: skip(z_); break;
        case 3: skip(n_); break;
      }
      break;
    case 0x0A: jump(true, true); break;
    case 0x0B: jump(z_, true); break;
    case 0x0C: jump(c_, true); break;
    case 0x0D: jump(n_, true); break;
    case 0x0E: jump(v_, true); break;
    case 0x0F: pull(); elapse(2); break;  // RTS
    case 0x10: mar_ = (mar_ + 1) & kMask24; break;  // INC MAR
    case 0x12: case 0x13: sub(operand(group & 1), shifted); break;  // CMPR
    case 0x14: case 0x15: sub(shifted, operand(group & 1)); break;  // CMP
    case 0x16:
      if (sub8 == 1) a_ = set_nz(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(a_))));    // SXB
      if (sub8 == 2) a_ = set_nz(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(a_))));   // SXW
      break;
    case 0x18:
      switch (sub8) {
        case 0: a_ = reg_read(reg) & kMask24; break;
        case 1: mdr_ = reg_read(reg) & kMask24; break;
        case 2: mar_ = reg_read(reg) & kMask24; break;
        case 3: p_ = static_cast<uint16_t>(gpr_[o & 15] & 0x7FFF); break;
      }
      break;
    case 0x19:
      switch (sub8) {
        case 0: a_ = imm; break;
        case 1: mdr_ = imm; break;
        case 2: mar_ = imm; break;
        case 3: p_ = static_cast<uint16_t>(imm); break;
      }
      break;
    case 0x1A: if (sub8 != 3) rdram(a_); break;
    case 0x1B: if (sub8 != 3) rdram(dpr_ + imm); break;
    case 0x1C: rom_buffer_ = data_rom_[a_ & 0x3FF]; break;
    case 0x1D: rom_buffer_ = data_rom_[o & 0x3FF]; break;
    case 0x1F:
      if (sub8 == 0) p_ = static_cast<uint16_t>((p_ & 0x7F00) | imm);                    // LDL
      if (sub8 == 1) p_ = static_cast<uint16_t>((p_ & 0x00FF) | ((o & 0x7F) << 8));      // LDH
      break;
    case 0x20: case 0x21: a_ = add(shifted, operand(group & 1)); break;
    case 0x22: case 0x23: a_ = sub(operand(group & 1), shifted); break;  // SUBR
    case 0x24: case 0x25: a_ = sub(shifted, operand(group & 1)); break;  // SUB
    case 0x26: case 0x27:  // MUL: signed 24 x 24 -> 48 (an immediate is unsigned 8-bit)
      mul_ = static_cast<uint64_t>(sign24(a_) * sign24(operand(group & 1))) & 0xFFFFFFFFFFFFull;
      break;
    case 0x28: case 0x29: a_ = set_nz(~shifted ^ operand(group & 1)); break;  // XNOR
    case 0x2A: case 0x2B: a_ = set_nz(shifted ^ operand(group & 1)); break;
    case 0x2C: case 0x2D: a_ = set_nz(shifted & operand(group & 1)); break;
    case 0x2E: case 0x2F: a_ = set_nz(shifted | operand(group & 1)); break;
    case 0x30: case 0x31: a_ = set_nz(a_ >> shift_amount(group & 1)); break;  // SHR
    case 0x32: case 0x33: a_ = set_nz(static_cast<uint32_t>(sign24(a_) >> shift_amount(group & 1))); break;  // ASR
    case 0x34: case 0x35: {  // ROR
      const unsigned s = shift_amount(group & 1);
      a_ = set_nz(s ? (a_ >> s) | (a_ << (24 - s)) : a_);
      break;
    }
    case 0x36: case 0x37: a_ = set_nz(a_ << shift_amount(group & 1)); break;  // SHL
    case 0x38:
      if (sub8 == 0) reg_write(reg, a_);
      if (sub8 == 1) reg_write(reg, mdr_);
      break;
    case 0x3A: if (sub8 != 3) wrram(a_); break;
    case 0x3B: if (sub8 != 3) wrram(dpr_ + imm); break;
    case 0x3C: std::swap(a_, gpr_[o & 15]); break;
    case 0x3E: a_ = 0; p_ = 0; ram_buffer_ = 0; dpr_ = 0; break;  // CLEAR
    case 0x3F: halt(); break;
    default: break;  // NOP ($00-$01, $08, $11, $17, $1E, $39, $3D and the gaps above)
  }
}

// ---- S-CPU side --------------------------------------------------------------------------------

auto Cx4::status() const -> Byte {
  return static_cast<Byte>((sleeping_ ? 0x01 : 0) | (i_ ? 0x02 : 0) | (running() ? 0x40 : 0) | (busy() ? 0x80 : 0));
}

auto Cx4::io_read(Word address) -> Byte {
  const unsigned reg = 0x7C00 | (address & 0x03FF);
  if (reg >= 0x7F80 && reg <= 0x7FEF) {  // GPRs, three bytes each ($7FB0-$7FBF reads 0)
    const unsigned index = reg & 0x3F;
    return reg < 0x7FB0 || reg >= 0x7FC0 ? byte_of(gpr_[index / 3], index % 3) : Byte{0};
  }
  if (reg >= 0x7F60 && reg <= 0x7F7F) return vector_[reg & 0x1F];
  if (reg >= 0x7F40 && reg <= 0x7F42) return byte_of(copy_.from, reg - 0x7F40);
  if (reg >= 0x7F43 && reg <= 0x7F44) return byte_of(copy_.count, reg - 0x7F43);
  if (reg >= 0x7F45 && reg <= 0x7F47) return byte_of(copy_.to, reg - 0x7F45);
  if (reg >= 0x7F49 && reg <= 0x7F4B) return byte_of(code_base_, reg - 0x7F49);
  if (reg >= 0x7F4D && reg <= 0x7F4E) return byte_of(start_bank_, reg - 0x7F4D);
  switch (reg) {
    case 0x7F48: return page_;
    case 0x7F4C: return static_cast<Byte>((pages_[0].locked ? 1 : 0) | (pages_[1].locked ? 2 : 0));
    case 0x7F4F: return start_pc_;
    case 0x7F50: return static_cast<Byte>(ram_wait_ | (rom_wait_ << 4));
    case 0x7F51: return irq_disable_ ? 1 : 0;
    case 0x7F52: return rom_config_;
    case 0x7F58: case 0x7F5A: return 0x00;
    default: return reg >= 0x7F53 && reg <= 0x7F5F ? status() : Byte{0};
  }
}

auto Cx4::io_write(Word address, Byte value) -> void {
  const unsigned reg = 0x7C00 | (address & 0x03FF);
  if ((reg >= 0x7F80 && reg <= 0x7FAF) || (reg >= 0x7FC0 && reg <= 0x7FEF)) {
    const unsigned index = reg & 0x3F;
    set_byte_of(gpr_[index / 3], index % 3, value);
    return;
  }
  if (reg >= 0x7F60 && reg <= 0x7F7F) { vector_[reg & 0x1F] = value; return; }
  if (reg >= 0x7F40 && reg <= 0x7F42) { set_byte_of(copy_.from, reg - 0x7F40, value); return; }
  if (reg >= 0x7F43 && reg <= 0x7F44) { set_byte_of(copy_.count, reg - 0x7F43, value); return; }
  if (reg >= 0x7F45 && reg <= 0x7F47) {
    set_byte_of(copy_.to, reg - 0x7F45, value);
    if (reg == 0x7F47 && halted_) copy_.active = true;  // the last target byte starts the copy
    return;
  }
  if (reg >= 0x7F49 && reg <= 0x7F4B) { set_byte_of(code_base_, reg - 0x7F49, value); return; }
  if (reg >= 0x7F55 && reg <= 0x7F5C) {  // suspend for 0, 32, ..., 224 clocks (0 = until $7F5D)
    sleeping_ = true;
    sleep_clocks_ = static_cast<uint8_t>((reg - 0x7F55) * 32);
    return;
  }
  switch (reg) {
    case 0x7F48:  // select a page; while stopped, also load the start bank into it
      page_ = value & 1;
      if (halted_) {
        pb_ = start_bank_;
        load_forced_ = true;
        load_pending_ = true;
      }
      break;
    case 0x7F4C:
      pages_[0].locked = value & 1;
      pages_[1].locked = value & 2;
      break;
    case 0x7F4D: start_bank_ = static_cast<uint16_t>((start_bank_ & 0x7F00) | value); break;
    case 0x7F4E: start_bank_ = static_cast<uint16_t>((start_bank_ & 0x00FF) | ((value & 0x7F) << 8)); break;
    case 0x7F4F:  // start the program at start bank : value
      start_pc_ = value;
      if (halted_) {
        halted_ = false;
        pb_ = start_bank_;
        pc_ = start_pc_;
      }
      break;
    case 0x7F50: ram_wait_ = value & 7; rom_wait_ = (value >> 4) & 7; break;
    case 0x7F51:
      irq_disable_ = value & 1;
      if (irq_disable_) { i_ = false; irq_line_ = false; }
      break;
    case 0x7F52: rom_config_ = value & 1; break;
    case 0x7F53: locked_up_ = false; halted_ = true; break;  // stop
    case 0x7F5D: sleeping_ = false; break;                   // resume
    case 0x7F5E: i_ = false; break;                           // the S-CPU IRQ line stays as is
    default: break;
  }
}

auto Cx4::read(Address address) -> std::optional<Byte> {
  // While the chip owns the bus, the S-CPU's vector fetches at $00:FFC0-FFFF see $7F40-$7F7F.
  if (busy() && (address & 0x7FFFC0) == 0x00FFC0) return io_read(static_cast<Word>(0x7F40 | (address & 0x3F)));
  if ((address & 0x40E000) != 0x006000) return std::nullopt;
  if ((address & 0x0C00) != 0x0C00) return dram_[address & 0xFFF];
  return io_read(static_cast<Word>(address));
}

auto Cx4::write(Address address, Byte value) -> bool {
  if ((address & 0x40E000) != 0x006000) return false;
  if ((address & 0x0C00) != 0x0C00) dram_[address & 0xFFF] = value;
  else io_write(static_cast<Word>(address), value);
  return true;
}

// The layout is fixed by existing save states (format 3 onward).
auto Cx4::serialize(Archive& ar) -> void {
  for (auto& page : pages_) ar(page.code);
  ar(dram_, pb_, pc_, n_, z_, c_, v_, i_, a_, p_, mul_, mdr_, rom_buffer_, ram_buffer_, mar_, dpr_, gpr_, stack_);
  ar(locked_up_, halted_, irq_disable_, irq_line_, rom_config_, vector_, rom_wait_, ram_wait_);
  ar(sleeping_, sleep_clocks_);
  std::array<bool, 2> locks = {pages_[0].locked, pages_[1].locked};
  std::array<uint32_t, 2> origins = {pages_[0].origin, pages_[1].origin};
  ar(load_pending_, page_, load_forced_, locks, origins, code_base_, start_bank_, start_pc_);
  ar(copy_.active, copy_.from, copy_.to, copy_.count);
  bool ext_active = ext_.kind != External::Idle;
  bool ext_read = ext_.kind == External::Read;
  bool ext_write = ext_.kind == External::Write;
  ar(ext_active, ext_read, ext_write, ext_.wait, ext_.address);
  ar(clock_budget_, clocks_);
  if (!ar.loading()) return;
  for (size_t k = 0; k < 2; k++) { pages_[k].locked = locks[k]; pages_[k].origin = origins[k]; }
  ext_.kind = !ext_active ? External::Idle : ext_read ? External::Read : ext_write ? External::Write : External::Idle;
  pb_ &= 0x7FFF; p_ &= 0x7FFF; page_ &= 1;
}

} // namespace bitforge::coprocessor
