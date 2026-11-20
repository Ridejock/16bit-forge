// SPDX-License-Identifier: MIT
#include "snes/cpu/cpu65c816.hpp"
#include "utils/logging.hpp"

#include <array>

namespace bitforge::cpu {

namespace {

using Opcode = CPU65c816::Opcode;
using enum CPU65c816::Op;
using enum CPU65c816::Mode;

// The whole instruction set: operation, addressing mode and base cycle count, indexed by opcode.
constexpr std::array<Opcode, 256> kOpcodes = {{
  // 0x00
  {BRK, Imm8, 8}, {ORA, DpIndX, 6}, {COP, Imm8, 8}, {ORA, Sr, 4},
  {TSB, Dp, 5}, {ORA, Dp, 3}, {ASL, Dp, 5}, {ORA, DpIndLong, 6},
  {PHP, Imp, 3}, {ORA, ImmM, 2}, {ASL, Acc, 2}, {PHD, Imp, 4},
  {TSB, Abs, 6}, {ORA, Abs, 4}, {ASL, Abs, 6}, {ORA, Long, 5},
  // 0x10
  {BPL, Rel8, 2}, {ORA, DpIndY, 5}, {ORA, DpInd, 5}, {ORA, SrIndY, 7},
  {TRB, Dp, 5}, {ORA, DpX, 4}, {ASL, DpX, 6}, {ORA, DpIndLongY, 6},
  {CLC, Imp, 2}, {ORA, AbsY, 4}, {INC, Acc, 2}, {TCS, Imp, 2},
  {TRB, Abs, 6}, {ORA, AbsX, 4}, {ASL, AbsX, 7}, {ORA, LongX, 5},
  // 0x20
  {JSR, Abs, 6}, {AND, DpIndX, 6}, {JSL, Long, 8}, {AND, Sr, 4},
  {BIT, Dp, 3}, {AND, Dp, 3}, {ROL, Dp, 5}, {AND, DpIndLong, 6},
  {PLP, Imp, 4}, {AND, ImmM, 2}, {ROL, Acc, 2}, {PLD, Imp, 5},
  {BIT, Abs, 4}, {AND, Abs, 4}, {ROL, Abs, 6}, {AND, Long, 5},
  // 0x30
  {BMI, Rel8, 2}, {AND, DpIndY, 5}, {AND, DpInd, 5}, {AND, SrIndY, 7},
  {BIT, DpX, 4}, {AND, DpX, 4}, {ROL, DpX, 6}, {AND, DpIndLongY, 6},
  {SEC, Imp, 2}, {AND, AbsY, 4}, {DEC, Acc, 2}, {TSC, Imp, 2},
  {BIT, AbsX, 4}, {AND, AbsX, 4}, {ROL, AbsX, 7}, {AND, LongX, 5},
  // 0x40
  {RTI, Imp, 7}, {EOR, DpIndX, 6}, {WDM, Imm8, 2}, {EOR, Sr, 4},
  {MVP, Move, 7}, {EOR, Dp, 3}, {LSR, Dp, 5}, {EOR, DpIndLong, 6},
  {PHA, Imp, 3}, {EOR, ImmM, 2}, {LSR, Acc, 2}, {PHK, Imp, 3},
  {JMP, Abs, 3}, {EOR, Abs, 4}, {LSR, Abs, 6}, {EOR, Long, 5},
  // 0x50
  {BVC, Rel8, 2}, {EOR, DpIndY, 5}, {EOR, DpInd, 5}, {EOR, SrIndY, 7},
  {MVN, Move, 7}, {EOR, DpX, 4}, {LSR, DpX, 6}, {EOR, DpIndLongY, 6},
  {CLI, Imp, 2}, {EOR, AbsY, 4}, {PHY, Imp, 3}, {TCD, Imp, 2},
  {JML, Long, 4}, {EOR, AbsX, 4}, {LSR, AbsX, 7}, {EOR, LongX, 5},
  // 0x60
  {RTS, Imp, 6}, {ADC, DpIndX, 6}, {PER, Rel16, 6}, {ADC, Sr, 4},
  {STZ, Dp, 3}, {ADC, Dp, 3}, {ROR, Dp, 5}, {ADC, DpIndLong, 6},
  {PLA, Imp, 4}, {ADC, ImmM, 2}, {ROR, Acc, 2}, {RTL, Imp, 6},
  {JMP, AbsInd, 5}, {ADC, Abs, 4}, {ROR, Abs, 6}, {ADC, Long, 5},
  // 0x70
  {BVS, Rel8, 2}, {ADC, DpIndY, 5}, {ADC, DpInd, 5}, {ADC, SrIndY, 7},
  {STZ, DpX, 4}, {ADC, DpX, 4}, {ROR, DpX, 6}, {ADC, DpIndLongY, 6},
  {SEI, Imp, 2}, {ADC, AbsY, 4}, {PLY, Imp, 4}, {TDC, Imp, 2},
  {JMP, AbsIndX, 6}, {ADC, AbsX, 4}, {ROR, AbsX, 7}, {ADC, LongX, 5},
  // 0x80
  {BRA, Rel8, 2}, {STA, DpIndX, 6}, {BRL, Rel16, 4}, {STA, Sr, 4},
  {STY, Dp, 3}, {STA, Dp, 3}, {STX, Dp, 3}, {STA, DpIndLong, 6},
  {DEY, Imp, 2}, {BIT, ImmM, 2}, {TXA, Imp, 2}, {PHB, Imp, 3},
  {STY, Abs, 4}, {STA, Abs, 4}, {STX, Abs, 4}, {STA, Long, 5},
  // 0x90
  {BCC, Rel8, 2}, {STA, DpIndY, 6}, {STA, DpInd, 5}, {STA, SrIndY, 7},
  {STY, DpX, 4}, {STA, DpX, 4}, {STX, DpY, 4}, {STA, DpIndLongY, 6},
  {TYA, Imp, 2}, {STA, AbsY, 5}, {TXS, Imp, 2}, {TXY, Imp, 2},
  {STZ, Abs, 4}, {STA, AbsX, 5}, {STZ, AbsX, 5}, {STA, LongX, 5},
  // 0xA0
  {LDY, ImmX, 2}, {LDA, DpIndX, 6}, {LDX, ImmX, 2}, {LDA, Sr, 4},
  {LDY, Dp, 3}, {LDA, Dp, 3}, {LDX, Dp, 3}, {LDA, DpIndLong, 6},
  {TAY, Imp, 2}, {LDA, ImmM, 2}, {TAX, Imp, 2}, {PLB, Imp, 4},
  {LDY, Abs, 4}, {LDA, Abs, 4}, {LDX, Abs, 4}, {LDA, Long, 5},
  // 0xB0
  {BCS, Rel8, 2}, {LDA, DpIndY, 5}, {LDA, DpInd, 5}, {LDA, SrIndY, 7},
  {LDY, DpX, 4}, {LDA, DpX, 4}, {LDX, DpY, 4}, {LDA, DpIndLongY, 6},
  {CLV, Imp, 2}, {LDA, AbsY, 4}, {TSX, Imp, 2}, {TYX, Imp, 2},
  {LDY, AbsX, 4}, {LDA, AbsX, 4}, {LDX, AbsY, 4}, {LDA, LongX, 5},
  // 0xC0
  {CPY, ImmX, 2}, {CMP, DpIndX, 6}, {REP, Imm8, 3}, {CMP, Sr, 4},
  {CPY, Dp, 3}, {CMP, Dp, 3}, {DEC, Dp, 5}, {CMP, DpIndLong, 6},
  {INY, Imp, 2}, {CMP, ImmM, 2}, {DEX, Imp, 2}, {WAI, Imp, 3},
  {CPY, Abs, 4}, {CMP, Abs, 4}, {DEC, Abs, 6}, {CMP, Long, 5},
  // 0xD0
  {BNE, Rel8, 2}, {CMP, DpIndY, 5}, {CMP, DpInd, 5}, {CMP, SrIndY, 7},
  {PEI, DpIndPei, 6}, {CMP, DpX, 4}, {DEC, DpX, 6}, {CMP, DpIndLongY, 6},
  {CLD, Imp, 2}, {CMP, AbsY, 4}, {PHX, Imp, 3}, {STP, Imp, 3},
  {JML, AbsIndLong, 6}, {CMP, AbsX, 4}, {DEC, AbsX, 7}, {CMP, LongX, 5},
  // 0xE0
  {CPX, ImmX, 2}, {SBC, DpIndX, 6}, {SEP, Imm8, 3}, {SBC, Sr, 4},
  {CPX, Dp, 3}, {SBC, Dp, 3}, {INC, Dp, 5}, {SBC, DpIndLong, 6},
  {INX, Imp, 2}, {SBC, ImmM, 2}, {NOP, Imp, 2}, {XBA, Imp, 3},
  {CPX, Abs, 4}, {SBC, Abs, 4}, {INC, Abs, 6}, {SBC, Long, 5},
  // 0xF0
  {BEQ, Rel8, 2}, {SBC, DpIndY, 5}, {SBC, DpInd, 5}, {SBC, SrIndY, 7},
  {PEA, Abs, 5}, {SBC, DpX, 4}, {INC, DpX, 6}, {SBC, DpIndLongY, 6},
  {SED, Imp, 2}, {SBC, AbsY, 4}, {PLX, Imp, 4}, {XCE, Imp, 2},
  {JSR, AbsIndX, 8}, {SBC, AbsX, 4}, {INC, AbsX, 7}, {SBC, LongX, 5},
}};

// Which register width adds cycles (and how many) when it is 16-bit.
enum class WidthCost : Byte { None, M1, M2, X1 };

constexpr auto width_cost(CPU65c816::Op op, CPU65c816::Mode mode) -> WidthCost {
  switch (op) {
    case ADC: case AND: case BIT: case CMP: case EOR: case LDA: case ORA:
    case SBC: case STA: case STZ: case PHA: case PLA:
      return WidthCost::M1;
    case ASL: case LSR: case ROL: case ROR: case INC: case DEC: case TRB: case TSB:
      return mode == Acc ? WidthCost::None : WidthCost::M2;
    case CPX: case CPY: case LDX: case LDY: case STX: case STY:
    case PHX: case PHY: case PLX: case PLY:
      return WidthCost::X1;
    default:
      return WidthCost::None;
  }
}

// 65816-only stack instructions address the stack with the full 16-bit S even in emulation mode;
// S is forced back into page 1 afterwards. The old 6502 ones wrap inside page 1.
constexpr auto uses_full_stack(CPU65c816::Op op, CPU65c816::Mode mode) -> bool {
  switch (op) {
    case JSL: case RTL: case PEA: case PEI: case PER: case PHD: case PLD: case PLB:
      return true;
    case JSR:
      return mode == AbsIndX;
    default:
      return false;
  }
}

constexpr auto is_direct_page(CPU65c816::Mode mode) -> bool {
  switch (mode) {
    case Dp: case DpX: case DpY: case DpInd: case DpIndPei: case DpIndX: case DpIndY:
    case DpIndLong: case DpIndLongY:
      return true;
    default:
      return false;
  }
}

// Interrupt vectors in bank 0: {native, emulation}.
struct Vectors { Word native; Word emulation; };
constexpr Vectors kCop{0xFFE4, 0xFFF4};
constexpr Vectors kBrk{0xFFE6, 0xFFFE};
constexpr Vectors kNmi{0xFFEA, 0xFFFA};
constexpr Vectors kIrq{0xFFEE, 0xFFFE};
constexpr Word kResetVector = 0xFFFC;
// IRQ/NMI: 8 cycles, 7 in emulation mode (no PBR push). The first is an opcode fetch from PB:PC,
// not an internal cycle as the datasheet says (Anomie; fullsnes counts it as internal).
constexpr int kInterruptCycles = 8;

constexpr Address kBankMask = 0x00FFFF;
constexpr Address kLinearMask = 0xFFFFFF;
constexpr Address kPageMask = 0x0000FF;

constexpr auto bank_addr(Byte bank, Word offset) -> Address {
  return (static_cast<Address>(bank) << 16) | offset;
}

} // namespace

CPU65c816::CPU65c816(CpuBus& bus_ref) : bus(bus_ref) {
  reset();
}

auto CPU65c816::reset() -> void {
  emulation = true;
  halted = false;
  wait_for_interrupt = false;
  irq_masked = true;
  p = M | X | I;
  a = x = y = d = 0;
  dbr = pbr = 0;
  sp = 0x01FF;
  pc = bus.read_vector(kResetVector);
  CPU_TRACE("CPU Reset - PC: $%04X, SP: $%04X", pc, sp);
}

auto CPU65c816::reset_signal() -> void {
  emulation = true;
  halted = false;
  wait_for_interrupt = false;
  irq_masked = true;
  p = static_cast<Byte>((p | M | X | I) & ~D);
  x &= 0x00FF;
  y &= 0x00FF;
  sp = static_cast<Word>(0x0100 | (sp & 0xFF));
  d = 0;
  dbr = pbr = 0;
  pc = bus.read_vector(kResetVector);
}

auto CPU65c816::registers() const -> Registers {
  return Registers{a, x, y, sp, d, pc, p, dbr, pbr, emulation};
}

auto CPU65c816::set_registers(const Registers& r) -> void {
  a = r.a; x = r.x; y = r.y; sp = r.sp; d = r.d; pc = r.pc;
  dbr = r.dbr; pbr = r.pbr; emulation = r.emulation;
  set_p(r.p);
  irq_masked = flag(I);
  if (emulation) sp = static_cast<Word>(0x0100 | (sp & 0xFF));
}

// ===== Flags =====

auto CPU65c816::set_p(Byte value) -> void {
  p = value;
  if (emulation) p |= M | X;  // M and X are hard-wired to 1 in emulation mode
  if (flag(X)) {               // 8-bit index registers lose their high byte
    x &= 0x00FF;
    y &= 0x00FF;
  }
}

auto CPU65c816::set_nz(Word value, bool wide) -> void {
  const Word sign = wide ? 0x8000 : 0x0080;
  const Word mask = wide ? 0xFFFF : 0x00FF;
  set_flag(N, (value & sign) != 0);
  set_flag(Z, (value & mask) == 0);
}

// ===== Memory =====

auto CPU65c816::read8(Address addr) -> Byte { return bus.read(addr & kLinearMask); }
auto CPU65c816::write8(Address addr, Byte value) -> void { bus.write(addr & kLinearMask, value); }

auto CPU65c816::next(EA ea) const -> Address {
  return (ea.addr & ~ea.wrap & kLinearMask) | ((ea.addr + 1) & ea.wrap);
}

auto CPU65c816::read(EA ea, bool wide) -> Word {
  Word value = read8(ea.addr);
  if (wide) value |= static_cast<Word>(read8(next(ea)) << 8);
  return value;
}

auto CPU65c816::write(EA ea, Word value, bool wide) -> void {
  write8(ea.addr, static_cast<Byte>(value));
  if (wide) write8(next(ea), static_cast<Byte>(value >> 8));
}

auto CPU65c816::fetch8() -> Byte {
  return read8(bank_addr(pbr, pc++));  // pc is 16-bit: program fetch wraps inside the bank
}

auto CPU65c816::fetch16() -> Word {
  Word low = fetch8();
  return static_cast<Word>(low | (fetch8() << 8));
}

auto CPU65c816::fetch24() -> Address {
  Address low = fetch16();
  return low | (static_cast<Address>(fetch8()) << 16);
}

// ===== Stack =====

auto CPU65c816::push8(Byte value) -> void {
  write8(sp, value);
  sp = static_cast<Word>(sp - 1);
  if (emulation && !full_stack) sp = static_cast<Word>(0x0100 | (sp & 0xFF));
}

auto CPU65c816::push16(Word value) -> void {
  push8(static_cast<Byte>(value >> 8));
  push8(static_cast<Byte>(value));
}

auto CPU65c816::pull8() -> Byte {
  sp = static_cast<Word>(sp + 1);
  if (emulation && !full_stack) sp = static_cast<Word>(0x0100 | (sp & 0xFF));
  return read8(sp);
}

auto CPU65c816::pull16() -> Word {
  Word low = pull8();
  return static_cast<Word>(low | (pull8() << 8));
}

// ===== Addressing =====

// Direct page lives in bank 0. In emulation mode with DL = 0 the old 6502 modes wrap in the page.
auto CPU65c816::direct(Word offset, bool page_wrap) const -> EA {
  if (page_wrap && emulation && (d & 0xFF) == 0) {
    return EA{static_cast<Address>(d | (offset & 0xFF)), kPageMask};
  }
  return EA{static_cast<Address>((d + offset) & 0xFFFF), kBankMask};
}

auto CPU65c816::effective_address(Mode mode) -> EA {
  const auto indexed = [](Address base, Word index) { return EA{(base + index) & kLinearMask}; };
  const auto long_pointer = [this](EA ptr) {
    Address low = read(ptr, true);
    Address bank = read8(next(EA{next(ptr), ptr.wrap}));
    return low | (bank << 16);
  };

  switch (mode) {
    case Imp: case Acc: case Move:
      return {};
    case ImmM: case ImmX: case Imm8: {
      EA ea{bank_addr(pbr, pc), kBankMask};
      const bool wide = (mode == ImmM && m_wide()) || (mode == ImmX && x_wide());
      pc = static_cast<Word>(pc + (wide ? 2 : 1));
      return ea;
    }
    case Abs:   return EA{bank_addr(dbr, fetch16())};
    case AbsX:  return indexed(bank_addr(dbr, fetch16()), x);
    case AbsY:  return indexed(bank_addr(dbr, fetch16()), y);
    case Long:  return EA{fetch24()};
    case LongX: return indexed(fetch24(), x);
    case Dp:    return direct(fetch8(), true);
    case DpX:   return direct(static_cast<Word>(fetch8() + x), true);
    case DpY:   return direct(static_cast<Word>(fetch8() + y), true);
    case DpInd:  return EA{bank_addr(dbr, read(direct(fetch8(), true), true))};
    case DpIndPei: return EA{bank_addr(dbr, read(direct(fetch8(), false), true))};
    case DpIndX: {
      // Emulation-mode quirk: the pointer's high byte wraps within the page even when DL != 0.
      EA ptr = direct(static_cast<Word>(fetch8() + x), true);
      if (emulation) ptr.wrap = kPageMask;
      return EA{bank_addr(dbr, read(ptr, true))};
    }
    case DpIndY: return indexed(bank_addr(dbr, read(direct(fetch8(), true), true)), y);
    case DpIndLong:  return EA{long_pointer(direct(fetch8(), false))};
    case DpIndLongY: return indexed(long_pointer(direct(fetch8(), false)), y);
    case Sr:     return EA{static_cast<Address>((sp + fetch8()) & 0xFFFF), kBankMask};
    case SrIndY: {
      EA ptr{static_cast<Address>((sp + fetch8()) & 0xFFFF), kBankMask};
      return indexed(bank_addr(dbr, read(ptr, true)), y);
    }
    case Rel8: {
      const auto offset = static_cast<s8>(fetch8());
      return EA{bank_addr(pbr, static_cast<Word>(pc + offset))};
    }
    case Rel16: {
      const Word offset = fetch16();
      return EA{bank_addr(pbr, static_cast<Word>(pc + offset))};
    }
    case AbsInd:  return EA{bank_addr(pbr, read(EA{fetch16(), kBankMask}, true))};
    case AbsIndX: {
      EA ptr{bank_addr(pbr, static_cast<Word>(fetch16() + x)), kBankMask};
      return EA{bank_addr(pbr, read(ptr, true))};
    }
    case AbsIndLong: return EA{long_pointer(EA{fetch16(), kBankMask})};
  }
  return {};
}

// ===== Arithmetic =====

// Binary or BCD add; subtraction adds the one's complement. Nibble-wise BCD matches hardware,
// including its flag results for invalid BCD input.
auto CPU65c816::add(Word lhs, Word rhs, bool wide, bool subtract) -> Word {
  const int bits = wide ? 16 : 8;
  const int mask = wide ? 0xFFFF : 0xFF;
  const int sign = wide ? 0x8000 : 0x80;
  const int l = lhs & mask;
  const int r = (subtract ? ~rhs : rhs) & mask;
  const bool decimal = flag(D);

  // Decimal adjust for the nibble at `shift`: add 6 past 9, or remove 6 on a borrow.
  const auto adjust = [subtract](int value, int shift) {
    if (!subtract && value > ((0xA << shift) - 1)) return value + (0x6 << shift);
    if (subtract && value <= ((0x10 << shift) - 1)) return value - (0x6 << shift);
    return value;
  };

  int result = 0;
  if (!decimal) {
    result = l + r + (flag(C) ? 1 : 0);
  } else {
    int carry = flag(C) ? 1 : 0;
    for (int shift = 0; shift < bits; shift += 4) {
      const int nibble = 0xF << shift;
      result = (l & nibble) + (r & nibble) + (carry << shift) + (result & ((1 << shift) - 1));
      if (shift + 4 < bits) {
        result = adjust(result, shift);
        carry = result > ((0x10 << shift) - 1) ? 1 : 0;
      }
    }
  }
  set_flag(V, (~(l ^ r) & (l ^ result) & sign) != 0);
  if (decimal) result = adjust(result, bits - 4);
  set_flag(C, result > mask);
  set_nz(static_cast<Word>(result), wide);
  return static_cast<Word>(result & mask);
}

auto CPU65c816::compare(Word reg, Word value, bool wide) -> void {
  const Word mask = wide ? 0xFFFF : 0x00FF;
  set_flag(C, (reg & mask) >= (value & mask));
  set_nz(static_cast<Word>(reg - value), wide);
}

auto CPU65c816::branch(bool taken, EA target) -> void {
  if (!taken) return;
  const Word old_pc = pc;
  pc = static_cast<Word>(target.addr);
  extra_cycles += 1;
  if (emulation && (old_pc & 0xFF00) != (pc & 0xFF00)) extra_cycles += 1;
}

// One byte per step; the opcode re-executes until A underflows, like hardware.
auto CPU65c816::block_move(int direction) -> void {
  const Byte dest = fetch8();
  const Byte src = fetch8();
  dbr = dest;
  write8(bank_addr(dest, y), read8(bank_addr(src, x)));
  const Word index_mask = x_wide() ? 0xFFFF : 0x00FF;
  x = static_cast<Word>((x + direction) & index_mask);
  y = static_cast<Word>((y + direction) & index_mask);
  if (a-- != 0) pc = static_cast<Word>(pc - 3);
}

auto CPU65c816::interrupt(Word native_vector, Word emulation_vector, bool software) -> void {
  if (!emulation) push8(pbr);
  push16(pc);
  // Emulation mode has no X flag on the stack; bit 4 is B: set for BRK/COP, clear for IRQ/NMI.
  push8(emulation ? static_cast<Byte>(software ? (p | 0x10) : (p & ~0x10)) : p);
  set_flag(I, true);
  set_flag(D, false);
  pbr = 0;
  pc = bus.read_vector(emulation ? emulation_vector : native_vector);
  wait_for_interrupt = false;
}

// ===== Execute =====

auto CPU65c816::execute(Op op, Mode mode, EA ea) -> void {
  const bool wm = m_wide();
  const bool wx = x_wide();
  const Word mask_m = wm ? 0xFFFF : 0x00FF;
  const Word mask_x = wx ? 0xFFFF : 0x00FF;

  const auto set_a = [&](Word value) {
    a = wm ? value : static_cast<Word>((a & 0xFF00) | (value & 0xFF));
    set_nz(a, wm);
  };
  const auto set_index = [&](Word& reg, Word value) {
    reg = static_cast<Word>(value & mask_x);
    set_nz(reg, wx);
  };
  // Read-modify-write on A or memory, at M width.
  const auto modify = [&](auto&& fn) {
    if (mode == Acc) {
      const Word result = fn(static_cast<Word>(a & mask_m));
      a = wm ? result : static_cast<Word>((a & 0xFF00) | (result & 0xFF));
    } else {
      write(ea, fn(read(ea, wm)), wm);
    }
  };
  const auto shift_nz = [&](Word value) { set_nz(value, wm); return static_cast<Word>(value & mask_m); };
  const Word top = wm ? 0x8000 : 0x0080;

  switch (op) {
    // Loads and stores
    case LDA: set_a(read(ea, wm)); break;
    case LDX: set_index(x, read(ea, wx)); break;
    case LDY: set_index(y, read(ea, wx)); break;
    case STA: write(ea, a, wm); break;
    case STX: write(ea, x, wx); break;
    case STY: write(ea, y, wx); break;
    case STZ: write(ea, 0, wm); break;

    // ALU
    case ADC: a = wm ? add(a, read(ea, wm), true, false)
                     : static_cast<Word>((a & 0xFF00) | add(a, read(ea, wm), false, false)); break;
    case SBC: a = wm ? add(a, read(ea, wm), true, true)
                     : static_cast<Word>((a & 0xFF00) | add(a, read(ea, wm), false, true)); break;
    case AND: set_a(static_cast<Word>(a & read(ea, wm))); break;
    case ORA: set_a(static_cast<Word>(a | read(ea, wm))); break;
    case EOR: set_a(static_cast<Word>(a ^ read(ea, wm))); break;
    case CMP: compare(a, read(ea, wm), wm); break;
    case CPX: compare(x, read(ea, wx), wx); break;
    case CPY: compare(y, read(ea, wx), wx); break;
    case BIT: {
      const Word value = read(ea, wm);
      set_flag(Z, (a & value & mask_m) == 0);
      if (mode != ImmM) {
        set_flag(N, (value & top) != 0);
        set_flag(V, (value & (top >> 1)) != 0);
      }
      break;
    }

    // Read-modify-write
    case ASL: modify([&](Word v) { set_flag(C, (v & top) != 0); return shift_nz(static_cast<Word>(v << 1)); }); break;
    case LSR: modify([&](Word v) { set_flag(C, (v & 1) != 0); return shift_nz(static_cast<Word>(v >> 1)); }); break;
    case ROL: modify([&](Word v) {
      const Word carry = flag(C) ? 1 : 0;
      set_flag(C, (v & top) != 0);
      return shift_nz(static_cast<Word>((v << 1) | carry));
    }); break;
    case ROR: modify([&](Word v) {
      const Word carry = flag(C) ? top : 0;
      set_flag(C, (v & 1) != 0);
      return shift_nz(static_cast<Word>((v >> 1) | carry));
    }); break;
    case INC: modify([&](Word v) { return shift_nz(static_cast<Word>(v + 1)); }); break;
    case DEC: modify([&](Word v) { return shift_nz(static_cast<Word>(v - 1)); }); break;
    case TSB: modify([&](Word v) { set_flag(Z, (v & a & mask_m) == 0); return static_cast<Word>(v | (a & mask_m)); }); break;
    case TRB: modify([&](Word v) { set_flag(Z, (v & a & mask_m) == 0); return static_cast<Word>(v & ~a & mask_m); }); break;
    case INX: set_index(x, static_cast<Word>(x + 1)); break;
    case INY: set_index(y, static_cast<Word>(y + 1)); break;
    case DEX: set_index(x, static_cast<Word>(x - 1)); break;
    case DEY: set_index(y, static_cast<Word>(y - 1)); break;

    // Branches and jumps
    case BPL: branch(!flag(N), ea); break;
    case BMI: branch(flag(N), ea); break;
    case BVC: branch(!flag(V), ea); break;
    case BVS: branch(flag(V), ea); break;
    case BCC: branch(!flag(C), ea); break;
    case BCS: branch(flag(C), ea); break;
    case BNE: branch(!flag(Z), ea); break;
    case BEQ: branch(flag(Z), ea); break;
    case BRA: branch(true, ea); break;
    case BRL: pc = static_cast<Word>(ea.addr); break;
    case JMP: pc = static_cast<Word>(ea.addr); break;  // stays in PBR
    case JML: pc = static_cast<Word>(ea.addr); pbr = static_cast<Byte>(ea.addr >> 16); break;
    case JSR: push16(static_cast<Word>(pc - 1)); pc = static_cast<Word>(ea.addr); break;
    case JSL:
      push8(pbr);
      push16(static_cast<Word>(pc - 1));
      pc = static_cast<Word>(ea.addr);
      pbr = static_cast<Byte>(ea.addr >> 16);
      break;
    case RTS: pc = static_cast<Word>(pull16() + 1); break;
    case RTL: pc = static_cast<Word>(pull16() + 1); pbr = pull8(); break;
    case RTI:
      set_p(pull8());
      pc = pull16();
      if (!emulation) pbr = pull8();
      break;
    case BRK: interrupt(kBrk.native, kBrk.emulation, true); break;
    case COP: interrupt(kCop.native, kCop.emulation, true); break;

    // Stack
    case PHA: if (wm) push16(a); else push8(static_cast<Byte>(a)); break;
    case PHX: if (wx) push16(x); else push8(static_cast<Byte>(x)); break;
    case PHY: if (wx) push16(y); else push8(static_cast<Byte>(y)); break;
    case PHP: push8(p); break;
    case PHB: push8(dbr); break;
    case PHK: push8(pbr); break;
    case PHD: push16(d); break;
    case PEA: case PEI: case PER: push16(static_cast<Word>(ea.addr)); break;
    case PLA: set_a(wm ? pull16() : pull8()); break;
    case PLX: set_index(x, wx ? pull16() : pull8()); break;
    case PLY: set_index(y, wx ? pull16() : pull8()); break;
    case PLP: set_p(pull8()); break;
    case PLB: dbr = pull8(); set_nz(dbr, false); break;
    case PLD: d = pull16(); set_nz(d, true); break;

    // Transfers
    case TAX: set_index(x, a); break;
    case TAY: set_index(y, a); break;
    case TXY: set_index(y, x); break;
    case TYX: set_index(x, y); break;
    case TSX: set_index(x, sp); break;
    case TXA: set_a(x); break;
    case TYA: set_a(y); break;
    case TXS: sp = emulation ? static_cast<Word>(0x0100 | (x & 0xFF)) : x; break;
    case TCS: sp = emulation ? static_cast<Word>(0x0100 | (a & 0xFF)) : a; break;
    case TSC: a = sp; set_nz(a, true); break;
    case TCD: d = a; set_nz(d, true); break;
    case TDC: a = d; set_nz(a, true); break;
    case XBA:
      a = static_cast<Word>((a << 8) | (a >> 8));
      set_nz(a, false);
      break;
    case MVN: block_move(+1); break;
    case MVP: block_move(-1); break;

    // Flags and mode
    case CLC: set_flag(C, false); break;
    case SEC: set_flag(C, true); break;
    case CLI: set_flag(I, false); break;
    case SEI: set_flag(I, true); break;
    case CLD: set_flag(D, false); break;
    case SED: set_flag(D, true); break;
    case CLV: set_flag(V, false); break;
    case REP: set_p(static_cast<Byte>(p & ~read8(ea.addr))); break;
    case SEP: set_p(static_cast<Byte>(p | read8(ea.addr))); break;
    case XCE: {
      const bool carry = flag(C);
      set_flag(C, emulation);
      emulation = carry;
      if (emulation) sp = static_cast<Word>(0x0100 | (sp & 0xFF));
      set_p(p);
      break;
    }

    // Control
    case NOP: case WDM: break;
    case STP: halted = true; break;
    case WAI: wait_for_interrupt = true; break;
  }
}

auto CPU65c816::cycles_for(const Opcode& entry) const -> int {
  int cycles = entry.cycles;
  switch (width_cost(entry.op, entry.mode)) {
    case WidthCost::M1: cycles += m_wide() ? 1 : 0; break;
    case WidthCost::M2: cycles += m_wide() ? 2 : 0; break;
    case WidthCost::X1: cycles += x_wide() ? 1 : 0; break;
    case WidthCost::None: break;
  }
  if (is_direct_page(entry.mode) && (d & 0xFF) != 0) cycles += 1;
  // BRK, COP and RTI skip the PBR byte in emulation mode.
  if (emulation && (entry.op == BRK || entry.op == COP || entry.op == RTI)) cycles -= 1;
  return cycles;
}

auto CPU65c816::hardware_interrupt(Word native_vector, Word emulation_vector) -> int {
  bus.read(bank_addr(pbr, pc));  // the opcode fetch it replaces
  interrupt(native_vector, emulation_vector, false);
  irq_masked = true;
  return emulation ? kInterruptCycles - 1 : kInterruptCycles;
}

auto CPU65c816::step() -> int {
  if (halted) return 1;

  const bool nmi = bus.poll_nmi();
  const bool irq = bus.irq_pending();
  int woke = 0;  // cycles spent leaving WAI
  if (wait_for_interrupt) {
    if (!nmi && !irq) return 1;
    wait_for_interrupt = false;  // a masked IRQ wakes it too, and it carries on with the next opcode
    woke = bus.end_wait();
  }
  if (nmi) return woke + hardware_interrupt(kNmi.native, kNmi.emulation);
  if (irq && !irq_masked) return woke + hardware_interrupt(kIrq.native, kIrq.emulation);

  CPU_TRACE("%02X:%04X  A:%04X X:%04X Y:%04X S:%04X D:%04X DB:%02X P:%02X%s", pbr, pc, a, x, y, sp, d, dbr, p,
            emulation ? " E" : "");
  const Opcode& entry = kOpcodes[fetch8()];
  extra_cycles = 0;
  const int cycles = cycles_for(entry);
  full_stack = uses_full_stack(entry.op, entry.mode);
  const EA ea = effective_address(entry.mode);
  const bool i_before = flag(I);
  execute(entry.op, entry.mode, ea);
  if (full_stack) {
    full_stack = false;
    if (emulation) sp = static_cast<Word>(0x0100 | (sp & 0xFF));
  }
  const bool late_i = entry.op == CLI || entry.op == SEI || entry.op == PLP || entry.op == REP || entry.op == SEP;
  irq_masked = late_i ? i_before : flag(I);
  return woke + cycles + extra_cycles;
}

// ===== Debug =====

auto CPU65c816::print_state() const -> void {
  utils::Logger::info(utils::LogChannel::Cpu, "PC: $%02X:%04X  A: $%04X  X: $%04X  Y: $%04X  SP: $%04X  D: $%04X  DB: $%02X  P: $%02X  E:%d",
                      pbr, pc, a, x, y, sp, d, dbr, p, emulation);
  utils::Logger::info(utils::LogChannel::Cpu, "Flags: %c%c%c%c%c%c%c%c",
                      flag(N) ? 'N' : 'n', flag(V) ? 'V' : 'v', flag(M) ? 'M' : 'm', flag(X) ? 'X' : 'x',
                      flag(D) ? 'D' : 'd', flag(I) ? 'I' : 'i', flag(Z) ? 'Z' : 'z', flag(C) ? 'C' : 'c');
}

} // namespace bitforge::cpu
