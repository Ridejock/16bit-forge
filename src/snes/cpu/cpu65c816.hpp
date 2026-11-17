// SPDX-License-Identifier: MIT
#pragma once
#include "snes/serialize.hpp"
#include "snes/types.hpp"
#include "snes/cpu/cpu_bus.hpp"

namespace bitforge::cpu {

// Register snapshot, for tests and debug views.
struct Registers {
  Word a = 0, x = 0, y = 0, sp = 0x01FF, d = 0, pc = 0;
  Byte p = 0x34, dbr = 0, pbr = 0;
  bool emulation = true;
};

class CPU65c816 {
public:
  explicit CPU65c816(CpuBus& bus);

  auto reset() -> void;
  // The /RES line on a running CPU (the SA-1 being rebooted by the S-CPU): A and the low bytes of
  // X/Y and S survive; D, DBR and PBR clear; M, X and I set, D clears; emulation mode; PC from the
  // reset vector. Also wakes from STP/WAI.
  auto reset_signal() -> void;
  // Runs one instruction (or services a pending interrupt). Returns CPU cycles used.
  auto step() -> int;
  auto print_state() const -> void;

  auto registers() const -> Registers;
  auto set_registers(const Registers& r) -> void;
  auto stopped() const -> bool { return halted; }
  auto waiting() const -> bool { return wait_for_interrupt; }

  auto serialize(Archive& ar) -> void {
    ar(a, x, y, sp, d, pc, p, dbr, pbr, emulation, halted, wait_for_interrupt, full_stack, extra_cycles);
  }

  // Instruction set, addressing modes and the opcode table entry (public so the table can name them).
  enum class Op : Byte {
    ADC, AND, ASL, BCC, BCS, BEQ, BIT, BMI, BNE, BPL, BRA, BRK, BRL, BVC, BVS, CLC,
    CLD, CLI, CLV, CMP, COP, CPX, CPY, DEC, DEX, DEY, EOR, INC, INX, INY, JML, JMP,
    JSL, JSR, LDA, LDX, LDY, LSR, MVN, MVP, NOP, ORA, PEA, PEI, PER, PHA, PHB, PHD,
    PHK, PHP, PHX, PHY, PLA, PLB, PLD, PLP, PLX, PLY, REP, ROL, ROR, RTI, RTL, RTS,
    SBC, SEC, SED, SEI, SEP, STA, STP, STX, STY, STZ, TAX, TAY, TCD, TCS, TDC, TRB,
    TSB, TSC, TSX, TXA, TXS, TXY, TYA, TYX, WAI, WDM, XBA, XCE,
  };
  enum class Mode : Byte {
    Imp,        // implied / stack
    Acc,        // A
    ImmM,       // #imm, width from M
    ImmX,       // #imm, width from X
    Imm8,       // #imm8 (REP, SEP, BRK/COP/WDM signature)
    Abs,        // addr
    AbsX,       // addr,X
    AbsY,       // addr,Y
    Long,       // long
    LongX,      // long,X
    Dp,         // dp
    DpX,        // dp,X
    DpY,        // dp,Y
    DpInd,      // (dp)
    DpIndPei,   // (dp) for PEI: a 65816 instruction, so no emulation-mode page wrap
    DpIndX,     // (dp,X)
    DpIndY,     // (dp),Y
    DpIndLong,  // [dp]
    DpIndLongY, // [dp],Y
    Sr,         // sr,S
    SrIndY,     // (sr,S),Y
    Rel8,       // 8-bit branch
    Rel16,      // 16-bit branch / PER
    AbsInd,     // (addr)   JMP
    AbsIndX,    // (addr,X) JMP/JSR
    AbsIndLong, // [addr]   JML
    Move,       // MVN/MVP: dest bank, src bank
  };
  struct Opcode {
    Op op;
    Mode mode;
    Byte cycles;  // base cycles: native mode, 8-bit M/X, DL = 0, branch not taken
  };

private:
  // An effective address plus the mask its second byte wraps within:
  // 0xFFFFFF carries into the next bank, 0x00FFFF stays in the bank, 0x0000FF stays in the page.
  struct EA {
    Address addr = 0;
    Address wrap = 0xFFFFFF;
  };

  CpuBus& bus;

  Word a = 0, x = 0, y = 0, sp = 0x01FF, d = 0, pc = 0;
  Byte p = 0x34, dbr = 0, pbr = 0;
  bool emulation = true;
  bool halted = false;
  bool wait_for_interrupt = false;
  bool full_stack = false;  // current instruction uses the 16-bit stack pointer in emulation mode
  int extra_cycles = 0;

  enum Flags : Byte { C = 0x01, Z = 0x02, I = 0x04, D = 0x08, X = 0x10, M = 0x20, V = 0x40, N = 0x80 };

  auto flag(Byte f) const -> bool { return (p & f) != 0; }
  auto set_flag(Byte f, bool on) -> void { p = on ? (p | f) : (p & ~f); }
  auto set_p(Byte value) -> void;  // applies emulation-mode and index-width side effects
  auto set_nz(Word value, bool wide) -> void;
  auto m_wide() const -> bool { return !flag(M); }
  auto x_wide() const -> bool { return !flag(X); }

  // Memory
  auto read8(Address addr) -> Byte;
  auto write8(Address addr, Byte value) -> void;
  auto next(EA ea) const -> Address;
  auto read(EA ea, bool wide) -> Word;
  auto write(EA ea, Word value, bool wide) -> void;
  auto fetch8() -> Byte;
  auto fetch16() -> Word;
  auto fetch24() -> Address;

  // Stack (bank 0; page 1 in emulation mode)
  auto push8(Byte value) -> void;
  auto push16(Word value) -> void;
  auto pull8() -> Byte;
  auto pull16() -> Word;

  // Addressing
  auto direct(Word offset, bool page_wrap) const -> EA;
  auto effective_address(Mode mode) -> EA;

  // Execution
  auto execute(Op op, Mode mode, EA ea) -> void;
  auto add(Word lhs, Word rhs, bool wide, bool subtract) -> Word;
  auto compare(Word reg, Word value, bool wide) -> void;
  auto branch(bool taken, EA target) -> void;
  auto block_move(int direction) -> void;
  auto interrupt(Word native_vector, Word emulation_vector, bool software) -> void;
  auto cycles_for(const Opcode& entry) const -> int;
};

} // namespace bitforge::cpu
