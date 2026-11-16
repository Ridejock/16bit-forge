// SPDX-License-Identifier: MIT
#include "snes/apu/spc700.hpp"

#include <array>

namespace snes::apu {

namespace {

using Opcode = SPC700::Opcode;
using enum SPC700::Op;
using enum SPC700::Mode;

// The whole instruction set: operation, addressing mode and cycle count, indexed by opcode.
// Cycle counts are the documented S-SMP timings. execute() spends exactly these, one bus access
// or idle step per cycle; the table keeps them for documentation and the cycle tests.
constexpr std::array<Opcode, 256> kOpcodes = {{
    // 0x00
    {NOP, Imp, 2}, {TCALL, Imp, 8}, {SET1, DpBit, 4}, {BBS, DpBitRel, 5}, {OR, Dp, 3}, {OR, Abs, 4}, {OR, IndX, 3}, {OR, DpIndX, 6},
    {OR, Imm, 2}, {OR, DpDp, 6}, {OR1, AbsBit, 5}, {ASL, Dp, 4}, {ASL, Abs, 5}, {PUSHP, Imp, 4}, {TSET1, Abs, 6}, {BRK, Imp, 8},
    // 0x10
    {BPL, Rel, 2}, {TCALL, Imp, 8}, {CLR1, DpBit, 4}, {BBC, DpBitRel, 5}, {OR, DpX, 4}, {OR, AbsX, 5}, {OR, AbsY, 5}, {OR, DpIndY, 6},
    {OR, DpImm, 5}, {OR, IndXY, 5}, {DECW, Dp, 6}, {ASL, DpX, 5}, {ASL, Acc, 2}, {DEX, Imp, 2}, {CMPX, Abs, 4}, {JMP, AbsXInd, 6},
    // 0x20
    {CLRP, Imp, 2}, {TCALL, Imp, 8}, {SET1, DpBit, 4}, {BBS, DpBitRel, 5}, {AND, Dp, 3}, {AND, Abs, 4}, {AND, IndX, 3}, {AND, DpIndX, 6},
    {AND, Imm, 2}, {AND, DpDp, 6}, {OR1N, AbsBit, 5}, {ROL, Dp, 4}, {ROL, Abs, 5}, {PUSHA, Imp, 4}, {CBNE, DpRel, 5}, {BRA, Rel, 2},
    // 0x30
    {BMI, Rel, 2}, {TCALL, Imp, 8}, {CLR1, DpBit, 4}, {BBC, DpBitRel, 5}, {AND, DpX, 4}, {AND, AbsX, 5}, {AND, AbsY, 5}, {AND, DpIndY, 6},
    {AND, DpImm, 5}, {AND, IndXY, 5}, {INCW, Dp, 6}, {ROL, DpX, 5}, {ROL, Acc, 2}, {INX, Imp, 2}, {CMPX, Dp, 3}, {CALL, Abs, 8},
    // 0x40
    {SETP, Imp, 2}, {TCALL, Imp, 8}, {SET1, DpBit, 4}, {BBS, DpBitRel, 5}, {EOR, Dp, 3}, {EOR, Abs, 4}, {EOR, IndX, 3}, {EOR, DpIndX, 6},
    {EOR, Imm, 2}, {EOR, DpDp, 6}, {AND1, AbsBit, 4}, {LSR, Dp, 4}, {LSR, Abs, 5}, {PUSHX, Imp, 4}, {TCLR1, Abs, 6}, {PCALL, UPage, 6},
    // 0x50
    {BVC, Rel, 2}, {TCALL, Imp, 8}, {CLR1, DpBit, 4}, {BBC, DpBitRel, 5}, {EOR, DpX, 4}, {EOR, AbsX, 5}, {EOR, AbsY, 5}, {EOR, DpIndY, 6},
    {EOR, DpImm, 5}, {EOR, IndXY, 5}, {CMPW, Dp, 4}, {LSR, DpX, 5}, {LSR, Acc, 2}, {TAX, Imp, 2}, {CMPY, Abs, 4}, {JMP, Abs, 3},
    // 0x60
    {CLRC, Imp, 2}, {TCALL, Imp, 8}, {SET1, DpBit, 4}, {BBS, DpBitRel, 5}, {CMP, Dp, 3}, {CMP, Abs, 4}, {CMP, IndX, 3}, {CMP, DpIndX, 6},
    {CMP, Imm, 2}, {CMP, DpDp, 6}, {AND1N, AbsBit, 4}, {ROR, Dp, 4}, {ROR, Abs, 5}, {PUSHY, Imp, 4}, {DBNZ, DpRel, 5}, {RET, Imp, 5},
    // 0x70
    {BVS, Rel, 2}, {TCALL, Imp, 8}, {CLR1, DpBit, 4}, {BBC, DpBitRel, 5}, {CMP, DpX, 4}, {CMP, AbsX, 5}, {CMP, AbsY, 5}, {CMP, DpIndY, 6},
    {CMP, DpImm, 5}, {CMP, IndXY, 5}, {ADDW, Dp, 5}, {ROR, DpX, 5}, {ROR, Acc, 2}, {TXA, Imp, 2}, {CMPY, Dp, 3}, {RETI, Imp, 6},
    // 0x80
    {SETC, Imp, 2}, {TCALL, Imp, 8}, {SET1, DpBit, 4}, {BBS, DpBitRel, 5}, {ADC, Dp, 3}, {ADC, Abs, 4}, {ADC, IndX, 3}, {ADC, DpIndX, 6},
    {ADC, Imm, 2}, {ADC, DpDp, 6}, {EOR1, AbsBit, 5}, {DEC, Dp, 4}, {DEC, Abs, 5}, {MOVY, Imm, 2}, {POPP, Imp, 4}, {MOVM, DpImm, 5},
    // 0x90
    {BCC, Rel, 2}, {TCALL, Imp, 8}, {CLR1, DpBit, 4}, {BBC, DpBitRel, 5}, {ADC, DpX, 4}, {ADC, AbsX, 5}, {ADC, AbsY, 5}, {ADC, DpIndY, 6},
    {ADC, DpImm, 5}, {ADC, IndXY, 5}, {SUBW, Dp, 5}, {DEC, DpX, 5}, {DEC, Acc, 2}, {TSX, Imp, 2}, {DIV, Imp, 12}, {XCN, Imp, 5},
    // 0xA0
    {EI, Imp, 3}, {TCALL, Imp, 8}, {SET1, DpBit, 4}, {BBS, DpBitRel, 5}, {SBC, Dp, 3}, {SBC, Abs, 4}, {SBC, IndX, 3}, {SBC, DpIndX, 6},
    {SBC, Imm, 2}, {SBC, DpDp, 6}, {MOV1C, AbsBit, 4}, {INC, Dp, 4}, {INC, Abs, 5}, {CMPY, Imm, 2}, {POPA, Imp, 4}, {STA, IndXInc, 4},
    // 0xB0
    {BCS, Rel, 2}, {TCALL, Imp, 8}, {CLR1, DpBit, 4}, {BBC, DpBitRel, 5}, {SBC, DpX, 4}, {SBC, AbsX, 5}, {SBC, AbsY, 5}, {SBC, DpIndY, 6},
    {SBC, DpImm, 5}, {SBC, IndXY, 5}, {LDW, Dp, 5}, {INC, DpX, 5}, {INC, Acc, 2}, {TXS, Imp, 2}, {DAS, Imp, 3}, {MOVA, IndXInc, 4},
    // 0xC0
    {DI, Imp, 3}, {TCALL, Imp, 8}, {SET1, DpBit, 4}, {BBS, DpBitRel, 5}, {STA, Dp, 4}, {STA, Abs, 5}, {STA, IndX, 4}, {STA, DpIndX, 7},
    {CMPX, Imm, 2}, {STX, Abs, 5}, {MOV1M, AbsBit, 6}, {STY, Dp, 4}, {STY, Abs, 5}, {MOVX, Imm, 2}, {POPX, Imp, 4}, {MUL, Imp, 9},
    // 0xD0
    {BNE, Rel, 2}, {TCALL, Imp, 8}, {CLR1, DpBit, 4}, {BBC, DpBitRel, 5}, {STA, DpX, 5}, {STA, AbsX, 6}, {STA, AbsY, 6}, {STA, DpIndY, 7},
    {STX, Dp, 4}, {STX, DpY, 5}, {STW, Dp, 5}, {STY, DpX, 5}, {DEY, Imp, 2}, {TYA, Imp, 2}, {CBNE, DpXRel, 6}, {DAA, Imp, 3},
    // 0xE0
    {CLRV, Imp, 2}, {TCALL, Imp, 8}, {SET1, DpBit, 4}, {BBS, DpBitRel, 5}, {MOVA, Dp, 3}, {MOVA, Abs, 4}, {MOVA, IndX, 3}, {MOVA, DpIndX, 6},
    {MOVA, Imm, 2}, {MOVX, Abs, 4}, {NOT1, AbsBit, 5}, {MOVY, Dp, 3}, {MOVY, Abs, 4}, {NOTC, Imp, 3}, {POPY, Imp, 4}, {SLEEP, Imp, 3},
    // 0xF0
    {BEQ, Rel, 2}, {TCALL, Imp, 8}, {CLR1, DpBit, 4}, {BBC, DpBitRel, 5}, {MOVA, DpX, 4}, {MOVA, AbsX, 5}, {MOVA, AbsY, 5}, {MOVA, DpIndY, 6},
    {MOVX, Dp, 3}, {MOVX, DpY, 4}, {MOVM, DpDp, 5}, {MOVY, DpX, 4}, {INY, Imp, 2}, {TAY, Imp, 2}, {DBNZY, Rel, 4}, {STOP, Imp, 3},
}};

constexpr Word kTcallBase = 0xFFDE;  // TCALL n reads its vector at $FFDE - 2n; BRK uses TCALL 0's
constexpr Word kPcallPage = 0xFF00;

} // namespace

SPC700::SPC700(Bus& b) : bus(b) {}

auto SPC700::reset(Word start) -> void {
  a = x = y = 0;
  sp = 0xEF;
  psw = 0x02;
  pc = start;
  halted_ = false;
}

auto SPC700::registers() const -> SmpRegisters { return {a, x, y, sp, psw, pc}; }

auto SPC700::set_registers(const SmpRegisters& r) -> void {
  a = r.a;
  x = r.x;
  y = r.y;
  sp = r.sp;
  psw = r.psw;
  pc = r.pc;
  halted_ = false;
}

auto SPC700::set_nz(Byte value) -> void {
  set_flag(Z, value == 0);
  set_flag(N, (value & 0x80) != 0);
}

auto SPC700::set_ya(Word value) -> void {
  a = static_cast<Byte>(value);
  y = static_cast<Byte>(value >> 8);
}

auto SPC700::fetch16() -> Word {
  const Byte low = fetch();
  return static_cast<Word>(low | (fetch() << 8));
}

auto SPC700::read_dp16(Byte offset) -> Word {
  const Byte low = load(offset);
  return static_cast<Word>(low | (load(static_cast<Byte>(offset + 1)) << 8));
}

auto SPC700::push_pc() -> void {
  push(static_cast<Byte>(pc >> 8));
  push(static_cast<Byte>(pc));
}

auto SPC700::pull_pc() -> void {
  const Byte low = pull();
  pc = static_cast<Word>(low | (pull() << 8));
}

auto SPC700::read_vector(Word vector) -> void {
  const Byte low = read(vector);
  pc = static_cast<Word>(low | (read(static_cast<Word>(vector + 1)) << 8));
}

// Operand reads, cycle by cycle: an index addition costs an idle cycle, and a mode without an
// operand byte still reads the byte after the opcode.
auto SPC700::read_operand(Mode mode) -> Byte {
  switch (mode) {
    case Imm: return fetch();
    case Dp: return load(fetch());
    case DpX: case DpY: {
      const Byte offset = fetch();
      idle();
      return load(static_cast<Byte>(offset + (mode == DpX ? x : y)));
    }
    case Abs: return read(fetch16());
    case AbsX: case AbsY: {
      const Word base = fetch16();
      idle();
      return read(static_cast<Word>(base + (mode == AbsX ? x : y)));
    }
    case IndX: read(pc); return load(x);
    case IndXInc: {
      read(pc);
      const Byte value = load(x++);
      idle();
      return value;
    }
    case DpIndX: {
      const Byte offset = fetch();
      idle();
      return read(read_dp16(static_cast<Byte>(offset + x)));
    }
    case DpIndY: {
      const Word base = read_dp16(fetch());
      idle();
      return read(static_cast<Word>(base + y));
    }
    default: return 0;  // not a read mode
  }
}

// Operand writes: every mode but (X)+ reads the target once before writing it.
auto SPC700::write_operand(Mode mode, Byte value) -> void {
  Word ea = 0;
  switch (mode) {
    case Dp: ea = dp(fetch()); break;
    case DpX: case DpY: {
      const Byte offset = fetch();
      idle();
      ea = dp(static_cast<Byte>(offset + (mode == DpX ? x : y)));
      break;
    }
    case Abs: ea = fetch16(); break;
    case AbsX: case AbsY: {
      const Word base = fetch16();
      idle();
      ea = static_cast<Word>(base + (mode == AbsX ? x : y));
      break;
    }
    case IndX: read(pc); ea = dp(x); break;
    case IndXInc: read(pc); idle(); write(dp(x++), value); return;
    case DpIndX: {
      const Byte offset = fetch();
      idle();
      ea = read_dp16(static_cast<Byte>(offset + x));
      break;
    }
    case DpIndY: {
      const Word base = read_dp16(fetch());
      idle();
      ea = static_cast<Word>(base + y);
      break;
    }
    default: return;  // not a write mode
  }
  read(ea);
  write(ea, value);
}

// Read-modify-write target for the shifts and INC/DEC (Dp, DpX, Abs).
auto SPC700::modify_address(Mode mode) -> Word {
  switch (mode) {
    case Dp: return dp(fetch());
    case DpX: {
      const Byte offset = fetch();
      idle();
      return dp(static_cast<Byte>(offset + x));
    }
    case Abs: return fetch16();
    default: return 0;
  }
}

auto SPC700::adc(Byte lhs, Byte rhs) -> Byte {
  const int sum = lhs + rhs + (flag(C) ? 1 : 0);
  const auto result = static_cast<Byte>(sum);
  set_flag(C, sum > 0xFF);
  set_flag(H, ((lhs ^ rhs ^ sum) & 0x10) != 0);
  set_flag(V, (~(lhs ^ rhs) & (lhs ^ sum) & 0x80) != 0);
  set_nz(result);
  return result;
}

auto SPC700::compare(Byte lhs, Byte rhs) -> void {
  const int diff = lhs - rhs;
  set_flag(C, diff >= 0);
  set_nz(static_cast<Byte>(diff));
}

auto SPC700::alu(Op op, Byte lhs, Byte rhs) -> Byte {
  Byte result = lhs;
  switch (op) {
    case OR: result = lhs | rhs; set_nz(result); break;
    case AND: result = lhs & rhs; set_nz(result); break;
    case EOR: result = lhs ^ rhs; set_nz(result); break;
    case CMP: compare(lhs, rhs); break;
    case ADC: result = adc(lhs, rhs); break;
    case SBC: result = adc(lhs, static_cast<Byte>(~rhs)); break;
    default: break;
  }
  return result;
}

auto SPC700::shift(Op op, Byte value) -> Byte {
  const bool carry_in = flag(C);
  switch (op) {
    case ASL: set_flag(C, value & 0x80); value = static_cast<Byte>(value << 1); break;
    case ROL: set_flag(C, value & 0x80); value = static_cast<Byte>((value << 1) | (carry_in ? 1 : 0)); break;
    case LSR: set_flag(C, value & 0x01); value = static_cast<Byte>(value >> 1); break;
    case ROR: set_flag(C, value & 0x01); value = static_cast<Byte>((value >> 1) | (carry_in ? 0x80 : 0)); break;
    case INC: value++; break;
    case DEC: value--; break;
    default: break;
  }
  set_nz(value);
  return value;
}

auto SPC700::branch(bool taken) -> void {
  const auto offset = static_cast<int8_t>(fetch());
  if (!taken) return;
  idle();
  idle();
  pc = static_cast<Word>(pc + offset);
}

auto SPC700::documented_cycles(Byte opcode) -> int { return kOpcodes[opcode].cycles; }

auto SPC700::step() -> int {
  cycles_ = 0;
  if (halted_) {
    idle();
    idle();
    return cycles_;
  }
  const Byte opcode = fetch();
  execute(kOpcodes[opcode], opcode);
  return cycles_;
}

auto SPC700::execute(const Opcode& entry, Byte opcode) -> void {
  const Mode mode = entry.mode;
  const int bit = opcode >> 5;  // SET1/CLR1/BBS/BBC

  switch (entry.op) {
    // ---- 8-bit ALU on A, or memory-to-memory (CMP ends with an idle cycle instead of the write) ----
    case OR: case AND: case EOR: case CMP: case ADC: case SBC: {
      if (mode == DpDp || mode == DpImm || mode == IndXY) {
        Byte rhs = 0;
        Word dst = 0;
        if (mode == DpDp) {
          rhs = load(fetch());
          dst = dp(fetch());
        } else if (mode == DpImm) {
          rhs = fetch();
          dst = dp(fetch());
        } else {
          read(pc);
          rhs = load(y);
          dst = dp(x);
        }
        const Byte result = alu(entry.op, read(dst), rhs);
        if (entry.op == CMP) idle(); else write(dst, result);
      } else {
        a = alu(entry.op, a, read_operand(mode));
      }
      break;
    }
    case CMPX: compare(x, read_operand(mode)); break;
    case CMPY: compare(y, read_operand(mode)); break;

    // ---- shifts, INC/DEC ----
    case ASL: case ROL: case LSR: case ROR: case INC: case DEC:
      if (mode == Acc) {
        read(pc);
        a = shift(entry.op, a);
      } else {
        const Word ea = modify_address(mode);
        write(ea, shift(entry.op, read(ea)));
      }
      break;
    case INX: read(pc); set_nz(++x); break;
    case DEX: read(pc); set_nz(--x); break;
    case INY: read(pc); set_nz(++y); break;
    case DEY: read(pc); set_nz(--y); break;

    // ---- moves ----
    case MOVA: a = read_operand(mode); set_nz(a); break;
    case MOVX: x = read_operand(mode); set_nz(x); break;
    case MOVY: y = read_operand(mode); set_nz(y); break;
    case STA: write_operand(mode, a); break;
    case STX: write_operand(mode, x); break;
    case STY: write_operand(mode, y); break;
    case MOVM: {
      if (mode == DpImm) {  // MOV d,#i reads the target first
        const Byte value = fetch();
        const Byte offset = fetch();
        load(offset);
        store(offset, value);
      } else {  // MOV dd,ds doesn't
        const Byte value = load(fetch());
        store(fetch(), value);
      }
      break;
    }
    case TAX: read(pc); x = a; set_nz(x); break;
    case TXA: read(pc); a = x; set_nz(a); break;
    case TAY: read(pc); y = a; set_nz(y); break;
    case TYA: read(pc); a = y; set_nz(a); break;
    case TSX: read(pc); x = sp; set_nz(x); break;
    case TXS: read(pc); sp = x; break;

    // ---- 16-bit ----
    case LDW: case ADDW: case SUBW: case CMPW: {
      const Byte offset = fetch();
      const Byte low = load(offset);
      if (entry.op != CMPW) idle();
      const auto rhs = static_cast<Word>(low | (load(static_cast<Byte>(offset + 1)) << 8));
      if (entry.op == LDW) {
        set_ya(rhs);
        set_flag(Z, ya() == 0);
        set_flag(N, (ya() & 0x8000) != 0);
      } else if (entry.op == CMPW) {
        const int diff = ya() - rhs;
        set_flag(C, diff >= 0);
        set_flag(Z, static_cast<Word>(diff) == 0);
        set_flag(N, (diff & 0x8000) != 0);
      } else {
        // Two 8-bit adds through the carry: C, H and V come from the high byte, Z from all 16 bits.
        const bool sub = entry.op == SUBW;
        set_flag(C, sub);
        const Byte lo = adc(a, static_cast<Byte>(sub ? ~rhs : rhs));
        const Byte hi = adc(y, static_cast<Byte>(sub ? ~(rhs >> 8) : (rhs >> 8)));
        a = lo;
        y = hi;
        set_flag(Z, ya() == 0);
      }
      break;
    }
    case STW: {
      const Byte offset = fetch();
      load(offset);
      store(offset, a);
      store(static_cast<Byte>(offset + 1), y);
      break;
    }
    case INCW: case DECW: {
      const Byte offset = fetch();
      auto value = static_cast<Word>(load(offset) + (entry.op == INCW ? 1 : -1));
      store(offset, static_cast<Byte>(value));
      value = static_cast<Word>(value + (load(static_cast<Byte>(offset + 1)) << 8));
      store(static_cast<Byte>(offset + 1), static_cast<Byte>(value >> 8));
      set_flag(Z, value == 0);
      set_flag(N, (value & 0x8000) != 0);
      break;
    }

    // ---- multiply, divide, decimal ----
    case MUL: {
      read(pc);
      idle_for(7);
      set_ya(static_cast<Word>(y * a));
      set_nz(y);  // N and Z follow the high byte only
      break;
    }
    case DIV: {
      // YA / X: A = quotient, Y = remainder. H and V are computed before the divide. Quotients
      // that do not fit in 9 bits come out the way the hardware's shift-subtract loop leaves them.
      read(pc);
      idle_for(10);
      const Word dividend = ya();
      set_flag(H, (y & 0x0F) >= (x & 0x0F));
      set_flag(V, y >= x);
      if (y < (x << 1)) {
        a = static_cast<Byte>(dividend / x);
        y = static_cast<Byte>(dividend % x);
      } else {
        a = static_cast<Byte>(255 - (dividend - (x << 9)) / (256 - x));
        y = static_cast<Byte>(x + (dividend - (x << 9)) % (256 - x));
      }
      set_nz(a);
      break;
    }
    case DAA:
      read(pc);
      idle();
      if (flag(C) || a > 0x99) {
        a = static_cast<Byte>(a + 0x60);
        set_flag(C, true);
      }
      if (flag(H) || (a & 0x0F) > 0x09) a = static_cast<Byte>(a + 0x06);
      set_nz(a);
      break;
    case DAS:
      read(pc);
      idle();
      if (!flag(C) || a > 0x99) {
        a = static_cast<Byte>(a - 0x60);
        set_flag(C, false);
      }
      if (!flag(H) || (a & 0x0F) > 0x09) a = static_cast<Byte>(a - 0x06);
      set_nz(a);
      break;
    case XCN:
      read(pc);
      idle_for(3);
      a = static_cast<Byte>((a >> 4) | (a << 4));
      set_nz(a);
      break;

    // ---- branches ----
    case BRA: branch(true); break;
    case BPL: branch(!flag(N)); break;
    case BMI: branch(flag(N)); break;
    case BVC: branch(!flag(V)); break;
    case BVS: branch(flag(V)); break;
    case BCC: branch(!flag(C)); break;
    case BCS: branch(flag(C)); break;
    case BNE: branch(!flag(Z)); break;
    case BEQ: branch(flag(Z)); break;
    case BBS: case BBC: {
      const Byte value = load(fetch());
      idle();
      const bool set = (value >> bit) & 1;
      branch(entry.op == BBS ? set : !set);
      break;
    }
    case CBNE: {
      Byte offset = fetch();
      if (mode == DpXRel) {
        idle();
        offset = static_cast<Byte>(offset + x);
      }
      const Byte value = load(offset);
      idle();
      branch(a != value);
      break;
    }
    case DBNZ: {
      const Byte offset = fetch();
      const auto value = static_cast<Byte>(load(offset) - 1);
      store(offset, value);
      branch(value != 0);
      break;
    }
    case DBNZY:
      read(pc);
      idle();
      branch(--y != 0);
      break;

    // ---- jumps, calls, returns ----
    case JMP:
      if (mode == Abs) {
        pc = fetch16();
      } else {
        const Word table = fetch16();
        idle();
        read_vector(static_cast<Word>(table + x));
      }
      break;
    case CALL: {
      const Word target = fetch16();
      idle();
      push_pc();
      idle_for(2);
      pc = target;
      break;
    }
    case PCALL: {
      const Byte target = fetch();
      idle();
      push_pc();
      idle();
      pc = static_cast<Word>(kPcallPage | target);
      break;
    }
    case TCALL:
      read(pc);
      idle();
      push_pc();
      idle();
      read_vector(static_cast<Word>(kTcallBase - (opcode >> 4) * 2));
      break;
    case BRK:
      read(pc);
      push_pc();
      push(psw);
      idle();
      read_vector(kTcallBase);
      set_flag(B, true);
      set_flag(I, false);
      break;
    case RET: read(pc); idle(); pull_pc(); break;
    case RETI: read(pc); idle(); psw = pull(); pull_pc(); break;

    // ---- stack ----
    case PUSHA: case PUSHX: case PUSHY: case PUSHP:
      read(pc);
      push(entry.op == PUSHA ? a : entry.op == PUSHX ? x : entry.op == PUSHY ? y : psw);
      idle();
      break;
    case POPA: case POPX: case POPY: case POPP: {
      read(pc);
      idle();
      const Byte value = pull();
      (entry.op == POPA ? a : entry.op == POPX ? x : entry.op == POPY ? y : psw) = value;
      break;
    }

    // ---- flags ----
    case CLRC: read(pc); set_flag(C, false); break;
    case SETC: read(pc); set_flag(C, true); break;
    case NOTC: read(pc); idle(); psw ^= C; break;
    case CLRV: read(pc); set_flag(V, false); set_flag(H, false); break;
    case CLRP: read(pc); set_flag(P, false); break;
    case SETP: read(pc); set_flag(P, true); break;
    case EI: read(pc); idle(); set_flag(I, true); break;
    case DI: read(pc); idle(); set_flag(I, false); break;

    // ---- bit operations ----
    case SET1: case CLR1: {
      const Byte offset = fetch();
      const Byte value = load(offset);
      const auto mask = static_cast<Byte>(1 << bit);
      store(offset, entry.op == SET1 ? (value | mask) : (value & ~mask));
      break;
    }
    case TSET1: case TCLR1: {
      const Word ea = fetch16();
      const Byte value = read(ea);
      set_nz(static_cast<Byte>(a - value));
      read(ea);
      write(ea, entry.op == TSET1 ? (value | a) : (value & ~a));
      break;
    }
    case OR1: case OR1N: case AND1: case AND1N: case EOR1: case MOV1C: case MOV1M: case NOT1: {
      const Word operand = fetch16();
      const auto ea = static_cast<Word>(operand & 0x1FFF);
      const int b = operand >> 13;
      const Byte value = read(ea);
      const bool m = (value >> b) & 1;
      switch (entry.op) {
        case OR1: idle(); set_flag(C, flag(C) || m); break;
        case OR1N: idle(); set_flag(C, flag(C) || !m); break;
        case AND1: set_flag(C, flag(C) && m); break;
        case AND1N: set_flag(C, flag(C) && !m); break;
        case EOR1: idle(); set_flag(C, flag(C) != m); break;
        case MOV1C: set_flag(C, m); break;
        case MOV1M:
          idle();
          write(ea, static_cast<Byte>(flag(C) ? (value | (1 << b)) : (value & ~(1 << b))));
          break;
        case NOT1: write(ea, static_cast<Byte>(value ^ (1 << b))); break;
        default: break;
      }
      break;
    }

    case NOP: read(pc); break;
    case SLEEP: case STOP: read(pc); idle(); halted_ = true; break;
  }
}

} // namespace snes::apu
