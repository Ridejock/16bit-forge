// SPDX-License-Identifier: MIT
#pragma once
#include "snes/serialize.hpp"
#include "snes/types.hpp"

namespace snes::apu {

// Register snapshot, for tests and debug views.
struct SmpRegisters {
  Byte a = 0, x = 0, y = 0, sp = 0xEF, psw = 0x02;
  Word pc = 0xFFC0;
};

// The S-SMP's CPU core (Sony SPC700). Instructions run whole, but every cycle is a bus call in
// hardware order (operand fetches, dummy reads, idle cycles), so the owner can advance timers
// and the DSP per cycle and place each access exactly.
class SPC700 {
public:
  // Everything the core touches goes through this: 64 KB of address space with the APU's
  // registers at $F0-$FF and the boot ROM at $FFC0.
  struct Bus {
    virtual auto read(Word addr) -> Byte = 0;
    virtual auto write(Word addr, Byte value) -> void = 0;
    virtual auto idle() -> void {}  // a cycle with no bus access

  protected:
    ~Bus() = default;
  };

  explicit SPC700(Bus& bus);

  auto reset(Word pc) -> void;
  // Runs one instruction. Returns SPC700 cycles used (an idle cycle pair once halted).
  auto step() -> int;
  static auto documented_cycles(Byte opcode) -> int;  // from the opcode table; taken branches add 2

  auto registers() const -> SmpRegisters;
  auto set_registers(const SmpRegisters& r) -> void;
  auto halted() const -> bool { return halted_; }  // after SLEEP or STOP (no wake-up source on the SNES)
  auto serialize(Archive& ar) -> void { ar(a, x, y, sp, psw, pc, halted_); }

  // The instruction set and addressing modes, named for the opcode table.
  // A-target ALU ops (OR, AND, EOR, CMP, ADC, SBC) work memory-to-memory in DpDp, DpImm and IndXY.
  enum class Op : Byte {
    ADC, ADDW, AND, AND1, AND1N, ASL, BBC, BBS, BCC, BCS, BEQ, BMI, BNE, BPL, BRA, BRK,
    BVC, BVS, CALL, CBNE, CLR1, CLRC, CLRP, CLRV, CMP, CMPW, CMPX, CMPY, DAA, DAS, DBNZ, DBNZY,
    DEC, DECW, DEX, DEY, DI, DIV, EI, EOR, EOR1, INC, INCW, INX, INY, JMP,
    LDW,    // MOVW YA,d
    LSR,
    MOV1C,  // MOV1 C,m.b
    MOV1M,  // MOV1 m.b,C
    MOVA, MOVM, MOVX, MOVY,  // MOV A/X/Y,src; MOVM = MOV dd,ds and MOV d,#i
    MUL, NOP, NOT1, NOTC, OR, OR1, OR1N, PCALL, POPA, POPP, POPX, POPY, PUSHA, PUSHP, PUSHX, PUSHY,
    RET, RETI, ROL, ROR, SBC, SET1, SETC, SETP, SLEEP, STA, STOP,
    STW,    // MOVW d,YA
    STX, STY, SUBW, TAX, TAY, TCALL, TCLR1, TSET1, TSX, TXA, TXS, TYA, XCN,
  };
  enum class Mode : Byte {
    Imp,       // implied / register
    Acc,       // A (shifts, INC/DEC)
    Imm,       // #i
    Dp,        // d
    DpX,       // d+X
    DpY,       // d+Y
    Abs,       // !a
    AbsX,      // !a+X
    AbsY,      // !a+Y
    IndX,      // (X)
    IndXInc,   // (X)+
    DpIndX,    // [d+X]
    DpIndY,    // [d]+Y
    DpDp,      // dd, ds
    DpImm,     // d, #i
    IndXY,     // (X), (Y)
    Rel,       // r
    DpRel,     // d, r
    DpXRel,    // d+X, r
    DpBit,     // d.b (bit from the opcode's top 3 bits)
    DpBitRel,  // d.b, r
    AbsBit,    // m.b (13-bit address, 3-bit bit number)
    AbsXInd,   // [!a+X]
    UPage,     // u (PCALL $FFuu)
  };
  struct Opcode {
    Op op;
    Mode mode;
    Byte cycles;  // branches: not taken; a taken branch adds 2
  };

  enum Flags : Byte { C = 0x01, Z = 0x02, I = 0x04, H = 0x08, B = 0x10, P = 0x20, V = 0x40, N = 0x80 };

private:
  Bus& bus;
  Byte a = 0, x = 0, y = 0, sp = 0xEF, psw = 0x02;
  Word pc = 0xFFC0;
  bool halted_ = false;
  int cycles_ = 0;  // cycles used by the running step()

  auto flag(Byte f) const -> bool { return (psw & f) != 0; }
  auto set_flag(Byte f, bool on) -> void { psw = on ? (psw | f) : (psw & ~f); }
  auto set_nz(Byte value) -> void;
  auto ya() const -> Word { return static_cast<Word>((y << 8) | a); }
  auto set_ya(Word value) -> void;

  auto read(Word addr) -> Byte { cycles_++; return bus.read(addr); }
  auto write(Word addr, Byte value) -> void { cycles_++; bus.write(addr, value); }
  auto idle() -> void { cycles_++; bus.idle(); }
  auto idle_for(int n) -> void { while (n-- > 0) idle(); }
  auto fetch() -> Byte { return read(pc++); }
  auto load(Byte offset) -> Byte { return read(dp(offset)); }
  auto store(Byte offset, Byte value) -> void { write(dp(offset), value); }
  auto fetch16() -> Word;
  auto dp(Byte offset) const -> Word { return static_cast<Word>((flag(P) ? 0x100 : 0) | offset); }
  auto read_dp16(Byte offset) -> Word;  // the high byte wraps within the direct page
  auto push(Byte value) -> void { write(static_cast<Word>(0x100 | sp--), value); }
  auto pull() -> Byte { return read(static_cast<Word>(0x100 | ++sp)); }
  auto push_pc() -> void;
  auto pull_pc() -> void;
  auto read_vector(Word vector) -> void;  // pc = 16-bit word at vector

  auto read_operand(Mode mode) -> Byte;
  auto write_operand(Mode mode, Byte value) -> void;
  auto modify_address(Mode mode) -> Word;
  auto alu(Op op, Byte lhs, Byte rhs) -> Byte;  // OR/AND/EOR/CMP/ADC/SBC
  auto adc(Byte lhs, Byte rhs) -> Byte;
  auto shift(Op op, Byte value) -> Byte;  // ASL/ROL/LSR/ROR/INC/DEC
  auto compare(Byte lhs, Byte rhs) -> void;
  auto branch(bool taken) -> void;
  auto execute(const Opcode& entry, Byte opcode) -> void;
};

} // namespace snes::apu
