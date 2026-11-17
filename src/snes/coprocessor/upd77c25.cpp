// SPDX-License-Identifier: MIT
#include "snes/coprocessor/upd77c25.hpp"

#include <algorithm>

namespace bitforge::coprocessor {

namespace {

// Conditional jumps (JP bits 13-21): which flag, and the value that takes the branch.
enum class Cond : uint8_t {
  Always, CA, CB, ZA, ZB, OV0A, OV0B, OV1A, OV1B, S0A, S0B, S1A, S1B,
  DpLowZero, DpLowNotZero, DpLowF, DpLowNotF, SiAck, SoAck, Rqm,
};
enum class JumpKind : uint8_t { Jump, ToSo, LowJump, HighJump, LowCall, HighCall };
struct JumpEntry { uint16_t brch; JumpKind kind; Cond cond; bool want; };
constexpr std::array<JumpEntry, 39> kJumps = {{
  {0x000, JumpKind::ToSo, Cond::Always, true},  // JMPSO
  {0x080, JumpKind::Jump, Cond::CA, false},    {0x082, JumpKind::Jump, Cond::CA, true},     // JNCA / JCA
  {0x084, JumpKind::Jump, Cond::CB, false},    {0x086, JumpKind::Jump, Cond::CB, true},
  {0x088, JumpKind::Jump, Cond::ZA, false},    {0x08A, JumpKind::Jump, Cond::ZA, true},     // JNZA / JZA
  {0x08C, JumpKind::Jump, Cond::ZB, false},    {0x08E, JumpKind::Jump, Cond::ZB, true},
  {0x090, JumpKind::Jump, Cond::OV0A, false},  {0x092, JumpKind::Jump, Cond::OV0A, true},
  {0x094, JumpKind::Jump, Cond::OV0B, false},  {0x096, JumpKind::Jump, Cond::OV0B, true},
  {0x098, JumpKind::Jump, Cond::OV1A, false},  {0x09A, JumpKind::Jump, Cond::OV1A, true},
  {0x09C, JumpKind::Jump, Cond::OV1B, false},  {0x09E, JumpKind::Jump, Cond::OV1B, true},
  {0x0A0, JumpKind::Jump, Cond::S0A, false},   {0x0A2, JumpKind::Jump, Cond::S0A, true},
  {0x0A4, JumpKind::Jump, Cond::S0B, false},   {0x0A6, JumpKind::Jump, Cond::S0B, true},
  {0x0A8, JumpKind::Jump, Cond::S1A, false},   {0x0AA, JumpKind::Jump, Cond::S1A, true},
  {0x0AC, JumpKind::Jump, Cond::S1B, false},   {0x0AE, JumpKind::Jump, Cond::S1B, true},
  {0x0B0, JumpKind::Jump, Cond::DpLowZero, true},  {0x0B1, JumpKind::Jump, Cond::DpLowNotZero, true},
  {0x0B2, JumpKind::Jump, Cond::DpLowF, true},     {0x0B3, JumpKind::Jump, Cond::DpLowNotF, true},
  {0x0B4, JumpKind::Jump, Cond::SiAck, false}, {0x0B6, JumpKind::Jump, Cond::SiAck, true},  // serial ports
  {0x0B8, JumpKind::Jump, Cond::SoAck, false}, {0x0BA, JumpKind::Jump, Cond::SoAck, true},  // aren't wired
  {0x0BC, JumpKind::Jump, Cond::Rqm, false},   {0x0BE, JumpKind::Jump, Cond::Rqm, true},    // JNRQM / JRQM
  {0x100, JumpKind::LowJump, Cond::Always, true},  {0x101, JumpKind::HighJump, Cond::Always, true},  // LJMP/HJMP
  {0x140, JumpKind::LowCall, Cond::Always, true},  {0x141, JumpKind::HighCall, Cond::Always, true},  // LCALL/HCALL
}};

constexpr auto lookup_jump(uint16_t brch) -> const JumpEntry* {
  for (const auto& entry : kJumps) {
    if (entry.brch == brch) return &entry;
  }
  return nullptr;
}

constexpr auto mask_bits(int bits) -> uint16_t { return static_cast<uint16_t>((1u << bits) - 1); }

} // namespace

UPD77C25::UPD77C25(const NecVariant& variant)
    : variant_(&variant),
      pc_mask_(mask_bits(variant.pc_bits)),
      rp_mask_(mask_bits(variant.rp_bits)),
      dp_mask_(mask_bits(variant.dp_bits)),
      sp_mask_(static_cast<uint8_t>(variant.stack_depth - 1)),
      program_(variant.program_words, 0),
      data_rom_(variant.data_rom_words, 0),
      data_ram_(variant.data_ram_words, 0) {
  reset();
}

auto UPD77C25::load_firmware(std::span<const uint8_t> program, std::span<const uint8_t> data) -> void {
  for (size_t i = 0; i < program_.size() && i * 3 + 2 < program.size(); i++) {
    program_[i] = program[i * 3] | (program[i * 3 + 1] << 8) | (program[i * 3 + 2] << 16);
  }
  for (size_t i = 0; i < data_rom_.size() && i * 2 + 1 < data.size(); i++) {
    data_rom_[i] = static_cast<uint16_t>(data[i * 2] | (data[i * 2 + 1] << 8));
  }
}

// Registers only: data RAM keeps its contents over a reset (it is the save RAM on ST010/ST011).
auto UPD77C25::reset() -> void { r_ = Regs{}; }

auto UPD77C25::status(bool raw) const -> uint16_t {
  const Status& s = r_.sr;
  const bool drs = s.drs && (raw || !s.drc);  // reads 0 in 8-bit mode
  return static_cast<uint16_t>(s.p0 | s.p1 << 1 | s.ei << 7 | s.sic << 8 | s.soc << 9 | s.drc << 10 | s.dma << 11 |
                               drs << 12 | s.usf0 << 13 | s.usf1 << 14 | s.rqm << 15);
}

auto UPD77C25::set_status(uint16_t v) -> void {
  Status& s = r_.sr;
  s.p0 = v & 0x0001; s.p1 = v & 0x0002; s.ei = v & 0x0080; s.sic = v & 0x0100; s.soc = v & 0x0200;
  s.drc = v & 0x0400; s.dma = v & 0x0800; s.drs = v & 0x1000; s.usf0 = v & 0x2000; s.usf1 = v & 0x4000;
  s.rqm = v & 0x8000;
}

auto UPD77C25::exec() -> void {
  const uint32_t opcode = program_[(r_.pc & pc_mask_) % program_.size()];
  r_.pc = static_cast<uint16_t>((r_.pc + 1) & pc_mask_);
  switch (opcode >> 22) {
    case 0: exec_op(opcode); break;
    case 1:  // RT: the OP, then pop the return address
      exec_op(opcode);
      r_.sp = static_cast<uint8_t>((r_.sp - 1) & sp_mask_);
      r_.pc = static_cast<uint16_t>(r_.stack[r_.sp] & pc_mask_);
      break;
    case 2: exec_jp(opcode); break;
    case 3: exec_ld(static_cast<uint16_t>(opcode >> 6), opcode & 0x0F); break;
  }
  // The multiplier runs every cycle: K*L as sign + 30 bits, split into M (high) and N (low).
  const int32_t product = static_cast<int32_t>(r_.k) * r_.l;
  r_.m = static_cast<int16_t>(product >> 15);
  r_.n = static_cast<int16_t>(static_cast<uint32_t>(product) << 1);
}

// ---- ALU instructions ---------------------------------------------------------------------------
// Encoding, SRC/DST tables and flag rules: fullsnes, "NEC uPD77C25 - ALU and LD Instructions".

// Value the SRC field puts on the internal data bus. Reading DR (SRC 8) raises RQM.
auto UPD77C25::bus_source(unsigned src) -> uint16_t {
  switch (src) {
    case 0x0: return r_.trb;
    case 0x1: return static_cast<uint16_t>(r_.a);
    case 0x2: return static_cast<uint16_t>(r_.b);
    case 0x3: return r_.tr;
    case 0x4: return r_.dp;
    case 0x5: return r_.rp;
    case 0x6: return data_rom_[r_.rp % data_rom_.size()];  // RO
    case 0x7: return static_cast<uint16_t>(0x8000 - (r_.fa.s1 ? 1 : 0));  // SGN (from flag A)
    case 0x8: r_.sr.rqm = true; return r_.dr;
    case 0x9: return r_.dr;  // DRNF
    case 0xA: return status();
    case 0xD: return static_cast<uint16_t>(r_.k);
    case 0xE: return static_cast<uint16_t>(r_.l);
    case 0xF: return ram(r_.dp);
    default: return r_.si;  // SIM, SIL: the serial input isn't wired
  }
}

// One ALU operation on accumulator `acc` with input `p`; updates the accumulator's flags.
// `other_carry` is the carry flag of the other accumulator (used by SBB/ADC/RCL1).
auto UPD77C25::alu(unsigned op, uint16_t acc, uint16_t p, bool other_carry, Flags& f) -> uint16_t {
  enum class Kind { Logic, Add, Sub, ShiftRight, ShiftLeft };
  const unsigned cin = other_carry ? 1 : 0;
  Kind kind = Kind::Logic;
  uint32_t out = 0;
  switch (op) {
    case 0x1: out = acc | p; break;                              // OR
    case 0x2: out = acc & p; break;                              // AND
    case 0x3: out = acc ^ p; break;                              // XOR
    case 0x4: kind = Kind::Sub; out = acc - p; break;            // SUB
    case 0x5: kind = Kind::Add; out = acc + p; break;            // ADD
    case 0x6: kind = Kind::Sub; out = acc - p - cin; break;      // SBB
    case 0x7: kind = Kind::Add; out = acc + p + cin; break;      // ADC
    case 0x8: kind = Kind::Sub; p = 1; out = acc - 1u; break;    // DEC
    case 0x9: kind = Kind::Add; p = 1; out = acc + 1u; break;    // INC
    case 0xA: out = acc ^ 0xFFFFu; break;                        // CMP (complement)
    case 0xB: kind = Kind::ShiftRight; out = (acc & 0x8000u) | (acc >> 1); break;  // SHR1, signed
    case 0xC: kind = Kind::ShiftLeft; out = (static_cast<uint32_t>(acc) << 1) | cin; break;  // SHL1
    case 0xD: out = (static_cast<uint32_t>(acc) << 2) | 0x3u; break;   // SHL2
    case 0xE: out = (static_cast<uint32_t>(acc) << 4) | 0xFu; break;   // SHL4
    case 0xF: out = (static_cast<uint32_t>(acc) << 8) | (acc >> 8); break;  // XCHG
    default: return acc;
  }
  const auto result = static_cast<uint16_t>(out);

  const bool had_ov1 = f.ov1;
  f.s0 = (result & 0x8000) != 0;
  f.z = result == 0;
  if (!had_ov1) f.s1 = f.s0;
  switch (kind) {
    case Kind::Logic:
      f.c = false;
      f.ov0 = false;
      f.ov1 = false;
      break;
    case Kind::ShiftRight:
    case Kind::ShiftLeft:
      f.c = kind == Kind::ShiftRight ? (acc & 1) != 0 : (acc & 0x8000) != 0;
      f.ov0 = false;
      f.ov1 = false;
      break;
    case Kind::Add:
    case Kind::Sub: {
      const bool same_sign_inputs = ((acc ^ p) & 0x8000) == 0;
      const bool sign_changed = ((acc ^ result) & 0x8000) != 0;
      f.ov0 = sign_changed && (kind == Kind::Add ? same_sign_inputs : !same_sign_inputs);
      f.c = kind == Kind::Add ? result < acc : result > acc;
      if (f.ov0) {
        // A further overflow: OV1 stays set only when it went the same way as the last (S1).
        f.ov1 = had_ov1 ? f.s0 == f.s1 : true;
      }
      break;
    }
  }
  return result;
}

auto UPD77C25::exec_op(uint32_t opcode) -> void {
  auto field = [opcode](unsigned lsb, unsigned width) { return (opcode >> lsb) & ((1u << width) - 1); };
  const unsigned p_select = field(20, 2), op = field(16, 4), dst = field(0, 4);
  const bool on_b = field(15, 1) != 0;

  const uint16_t idb = bus_source(field(4, 4));
  if (op != 0) {
    uint16_t p = idb;
    if (p_select == 0) p = ram(r_.dp);
    if (p_select == 2) p = static_cast<uint16_t>(r_.m);
    if (p_select == 3) p = static_cast<uint16_t>(r_.n);
    int16_t& acc = on_b ? r_.b : r_.a;
    Flags& flags = on_b ? r_.fb : r_.fa;
    const bool other_carry = on_b ? r_.fa.c : r_.fb.c;
    Flags updated = flags;
    acc = static_cast<int16_t>(alu(op, static_cast<uint16_t>(acc), p, other_carry, updated));
    flags = updated;
  }

  exec_ld(idb, dst);

  // Pointer modifiers; a move into the same pointer takes precedence.
  if (dst != 4) {
    const unsigned low = r_.dp & 0x0F;
    unsigned new_low = low;
    switch (field(13, 2)) {
      case 1: new_low = (low + 1) & 0x0F; break;  // DPINC
      case 2: new_low = (low - 1) & 0x0F; break;  // DPDEC
      case 3: new_low = 0; break;                 // DPCLR
    }
    const unsigned high_xor = field(9, 4) << 4;   // M0-MF
    r_.dp = static_cast<uint16_t>((((r_.dp & ~0x0Fu) | new_low) ^ high_xor) & dp_mask_);
  }
  if (dst != 5 && field(8, 1)) r_.rp = static_cast<uint16_t>((r_.rp - 1) & rp_mask_);  // RPDEC
}

auto UPD77C25::exec_jp(uint32_t opcode) -> void {
  const uint16_t brch = static_cast<uint16_t>((opcode >> 13) & 0x1FF);
  const uint16_t na = static_cast<uint16_t>((opcode >> 2) & 0x7FF);
  const uint16_t bank = static_cast<uint16_t>(opcode & 3);
  const uint16_t target = static_cast<uint16_t>(((r_.pc & 0x2000) | (bank << 11) | na) & pc_mask_);

  const JumpEntry* entry = lookup_jump(brch);
  if (!entry) return;  // undefined condition: falls through

  bool flag = true;
  switch (entry->cond) {
    case Cond::Always: flag = true; break;
    case Cond::CA: flag = r_.fa.c; break;
    case Cond::CB: flag = r_.fb.c; break;
    case Cond::ZA: flag = r_.fa.z; break;
    case Cond::ZB: flag = r_.fb.z; break;
    case Cond::OV0A: flag = r_.fa.ov0; break;
    case Cond::OV0B: flag = r_.fb.ov0; break;
    case Cond::OV1A: flag = r_.fa.ov1; break;
    case Cond::OV1B: flag = r_.fb.ov1; break;
    case Cond::S0A: flag = r_.fa.s0; break;
    case Cond::S0B: flag = r_.fb.s0; break;
    case Cond::S1A: flag = r_.fa.s1; break;
    case Cond::S1B: flag = r_.fb.s1; break;
    case Cond::DpLowZero: flag = (r_.dp & 0x0F) == 0x00; break;
    case Cond::DpLowNotZero: flag = (r_.dp & 0x0F) != 0x00; break;
    case Cond::DpLowF: flag = (r_.dp & 0x0F) == 0x0F; break;
    case Cond::DpLowNotF: flag = (r_.dp & 0x0F) != 0x0F; break;
    case Cond::SiAck: case Cond::SoAck: flag = false; break;
    case Cond::Rqm: flag = r_.sr.rqm; break;
  }
  if (flag != entry->want) return;

  auto push = [this] {
    r_.stack[r_.sp] = r_.pc;
    r_.sp = static_cast<uint8_t>((r_.sp + 1) & sp_mask_);
  };
  switch (entry->kind) {
    case JumpKind::Jump: r_.pc = target; break;
    case JumpKind::ToSo: r_.pc = static_cast<uint16_t>(r_.so & pc_mask_); break;
    case JumpKind::LowJump: r_.pc = static_cast<uint16_t>(target & ~0x2000); break;
    case JumpKind::HighJump: r_.pc = static_cast<uint16_t>((target | 0x2000) & pc_mask_); break;
    case JumpKind::LowCall: push(); r_.pc = static_cast<uint16_t>(target & ~0x2000); break;
    case JumpKind::HighCall: push(); r_.pc = static_cast<uint16_t>((target | 0x2000) & pc_mask_); break;
  }
}

auto UPD77C25::exec_ld(uint16_t id, unsigned dst) -> void {
  switch (dst) {
    case 0: break;  // @NON
    case 1: r_.a = static_cast<int16_t>(id); break;
    case 2: r_.b = static_cast<int16_t>(id); break;
    case 3: r_.tr = id; break;
    case 4: r_.dp = static_cast<uint16_t>(id & dp_mask_); break;
    case 5: r_.rp = static_cast<uint16_t>(id & rp_mask_); break;
    case 6: r_.dr = id; r_.sr.rqm = true; break;  // the host may now read/write DR
    case 7: {  // SR: RQM, DRS and the unused bits 6-2 can't be written
      constexpr uint16_t kReadOnly = 0x907C;
      set_status(static_cast<uint16_t>((status(true) & kReadOnly) | (id & ~kReadOnly)));
      break;
    }
    case 8: case 9: r_.so = id; break;           // SOL / SOM
    case 10: r_.k = static_cast<int16_t>(id); break;
    case 11: r_.k = static_cast<int16_t>(id); r_.l = static_cast<int16_t>(data_rom_[r_.rp % data_rom_.size()]); break;
    case 12: r_.l = static_cast<int16_t>(id); r_.k = static_cast<int16_t>(ram(r_.dp | 0x40)); break;
    case 13: r_.l = static_cast<int16_t>(id); break;
    case 14: r_.trb = id; break;
    case 15: ram(r_.dp) = id; break;
  }
}

// ---- host port -------------------------------------------------------------------------------
// In 16-bit mode (DRC=0) the host moves DR a byte at a time, low byte first; DRS marks the
// half-way point and RQM drops once the second byte has gone. In 8-bit mode one byte does it.

auto UPD77C25::read_dr() -> Byte {
  Status& s = r_.sr;
  if (s.drc) {
    s.rqm = false;
    return static_cast<Byte>(r_.dr);
  }
  if (!s.drs) {
    s.drs = true;
    return static_cast<Byte>(r_.dr);
  }
  s.rqm = false;
  s.drs = false;
  return static_cast<Byte>(r_.dr >> 8);
}

auto UPD77C25::write_dr(Byte value) -> void {
  Status& s = r_.sr;
  if (s.drc) {
    s.rqm = false;
    r_.dr = static_cast<uint16_t>((r_.dr & 0xFF00) | value);
    return;
  }
  if (!s.drs) {
    s.drs = true;
    r_.dr = static_cast<uint16_t>((r_.dr & 0xFF00) | value);
    return;
  }
  s.rqm = false;
  s.drs = false;
  r_.dr = static_cast<uint16_t>((value << 8) | (r_.dr & 0x00FF));
}

auto UPD77C25::read_dp(Word address) const -> Byte {
  const uint16_t word = data_ram_[(address >> 1) % data_ram_.size()];
  return static_cast<Byte>((address & 1) ? word >> 8 : word);
}

auto UPD77C25::write_dp(Word address, Byte value) -> void {
  uint16_t& word = data_ram_[(address >> 1) % data_ram_.size()];
  word = (address & 1) ? static_cast<uint16_t>((word & 0x00FF) | (value << 8))
                       : static_cast<uint16_t>((word & 0xFF00) | value);
}

auto UPD77C25::serialize(Archive& ar) -> void {
  r_.serialize(ar);
  for (auto& word : data_ram_) ar(word);
}

} // namespace bitforge::coprocessor
