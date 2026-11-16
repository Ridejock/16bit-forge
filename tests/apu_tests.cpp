// SPDX-License-Identifier: MIT
// APU tests: SPC700 opcodes and flags (MUL/DIV/DAA/DAS, the P flag, calls, bit ops, cycles),
// the S-SMP memory map, ports, IPL upload and timers, master-clock sync, and the S-DSP (BRR
// decoding, Gaussian interpolation, envelopes, counters, KON/KOFF/ENDX, echo, noise, mute).
// Then every *.spc in SNES_TEST_ROMS_DIR (optional), plus any SPC test programs given on the
// command line, is run to completion: the PeterLemon SPC700 tests post 1, 2, 3... on port 0 per passing
// check and $80 | n on a failure.
#include "snes/apu/apu.hpp"
#include "snes/apu/dsp.hpp"
#include "snes/apu/dsp_tables.hpp"
#include "snes/apu/spc700.hpp"
#include "snes/memory/bus.hpp"
#include "test_rom.hpp"
#include "dsp_stress.hpp"
#include "utils/logging.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

using snes::Byte;
using snes::Word;
using snes::apu::APU;
using snes::apu::DSP;
using snes::apu::SPC700;

namespace {

int failures_in_test = 0;
auto check(bool ok, const char* what) -> void {
    if (!ok) {
        if (failures_in_test == 0) printf("\n");
        printf("      FAIL: %s\n", what);
        failures_in_test++;
    }
}
auto check_eq(long got, long want, const char* what) -> void {
    if (got != want) {
        if (failures_in_test == 0) printf("\n");
        printf("      FAIL: %s: got %ld (0x%lX), want %ld (0x%lX)\n", what, got, static_cast<unsigned long>(got), want,
               static_cast<unsigned long>(want));
        failures_in_test++;
    }
}

// ---- SPC700 on a flat 64 KB bus ---------------------------------------------------------------

struct FlatBus : SPC700::Bus {
    std::array<Byte, 0x10000> mem{};
    std::string trace;  // one letter per cycle: R(ead), W(rite), I(dle)
    std::vector<Word> addrs;  // address of each R/W
    auto read(Word addr) -> Byte override { trace += 'R'; addrs.push_back(addr); return mem[addr]; }
    auto write(Word addr, Byte value) -> void override { trace += 'W'; addrs.push_back(addr); mem[addr] = value; }
    auto idle() -> void override { trace += 'I'; }
};

constexpr Word kOrg = 0x0400;

struct Cpu {
    FlatBus bus;
    SPC700 cpu{bus};
    Cpu() { cpu.reset(kOrg); }
    // Loads code at $0400 and sets registers; returns the cycles of the first instruction.
    auto load(std::initializer_list<Byte> code) -> void {
        Word at = kOrg;
        for (Byte b : code) bus.mem[at++] = b;
    }
    auto regs(Byte a, Byte x, Byte y, Byte psw) -> void {
        cpu.set_registers({a, x, y, 0xEF, psw, kOrg});
    }
    auto r() const -> snes::apu::SmpRegisters { return cpu.registers(); }
};

// Vectors from gilyon/snes-tests spctest (tests.txt): opcode, A, X, Y, P in -> A, Y, P out.
struct ArithVector {
    Byte opcode, a, x, y, p, a_out, y_out, p_out;
};
constexpr ArithVector kArith[] = {
    {0xCF, 0xab, 0x34, 0xcd, 0xff, 0xef, 0x88, 0xfd},  // spctest 03fe
    {0xCF, 0x05, 0x34, 0x02, 0x00, 0x0a, 0x00, 0x02},  // spctest 03ff
    {0xCF, 0xff, 0x34, 0xff, 0x00, 0x01, 0xfe, 0x80},  // spctest 0400
    {0xCF, 0x80, 0x34, 0x02, 0x00, 0x00, 0x01, 0x00},  // spctest 0401
    {0xDF, 0x10, 0x34, 0x56, 0x00, 0x10, 0x56, 0x00},  // spctest 0541
    {0xDF, 0x1a, 0x34, 0x56, 0x00, 0x20, 0x56, 0x00},  // spctest 0542
    {0xDF, 0x10, 0x34, 0x56, 0x01, 0x70, 0x56, 0x01},  // spctest 0543
    {0xDF, 0x10, 0x34, 0x56, 0x09, 0x76, 0x56, 0x09},  // spctest 0544
    {0xDF, 0xff, 0x34, 0x56, 0xf6, 0x65, 0x56, 0x75},  // spctest 0545
    {0xDF, 0x9a, 0x34, 0x56, 0x00, 0x00, 0x56, 0x03},  // spctest 0546
    {0xDF, 0x91, 0x34, 0x56, 0x01, 0xf1, 0x56, 0x81},  // spctest 0547
    {0xDF, 0x9a, 0x34, 0x56, 0x08, 0x00, 0x56, 0x0b},  // spctest 0548
    {0xDF, 0x99, 0x34, 0x56, 0x08, 0x9f, 0x56, 0x88},  // spctest 0549
    {0xBE, 0x10, 0x34, 0x56, 0x09, 0x10, 0x56, 0x09},  // spctest 054a
    {0xBE, 0x1a, 0x34, 0x56, 0x09, 0x14, 0x56, 0x09},  // spctest 054b
    {0xBE, 0xa1, 0x34, 0x56, 0xff, 0x41, 0x56, 0x7c},  // spctest 054c
    {0xBE, 0xff, 0x34, 0x56, 0x09, 0x99, 0x56, 0x88},  // spctest 054d
    {0xBE, 0x99, 0x34, 0x56, 0x00, 0x33, 0x56, 0x00},  // spctest 054e
    {0xBE, 0x66, 0x34, 0x56, 0x00, 0x00, 0x56, 0x02},  // spctest 054f
    {0xBE, 0x11, 0x34, 0x56, 0x00, 0xab, 0x56, 0x80},  // spctest 0550
    {0x9E, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xc8},  // spctest 0551
    {0x9E, 0x23, 0x10, 0x01, 0x00, 0x12, 0x03, 0x08},  // spctest 0552
    {0x9E, 0x10, 0x88, 0x01, 0xff, 0x02, 0x00, 0x35},  // spctest 0553
    {0x9E, 0x0f, 0x88, 0x01, 0xff, 0x01, 0x87, 0x35},  // spctest 0554
    {0x9E, 0x23, 0x01, 0x01, 0x00, 0x23, 0x00, 0x48},  // spctest 0555
    {0x9E, 0xff, 0x00, 0xff, 0x00, 0x00, 0xff, 0x4a},  // spctest 0556
    {0x9E, 0xcd, 0x03, 0xab, 0x00, 0x58, 0xc5, 0x48},  // spctest 0557
};

auto test_mul_div_daa_das() -> void {
    for (const auto& v : kArith) {
        Cpu c;
        c.load({v.opcode});
        c.regs(v.a, v.x, v.y, v.p);
        c.cpu.step();
        char what[64];
        snprintf(what, sizeof(what), "op %02X A=%02X X=%02X Y=%02X P=%02X", v.opcode, v.a, v.x, v.y, v.p);
        const auto r = c.r();
        if (r.a != v.a_out || r.y != v.y_out || r.psw != v.p_out) {
            check(false, what);
            printf("        got A=%02X Y=%02X P=%02X, want A=%02X Y=%02X P=%02X\n", r.a, r.y, r.psw, v.a_out, v.y_out, v.p_out);
        }
    }
    // DIV by zero takes the out-of-range path: A=$FF-ish, V set; it must not trap.
    Cpu c;
    c.load({0x9E});
    c.regs(0x34, 0x00, 0x12, 0x00);
    c.cpu.step();
    check((c.r().psw & SPC700::V) != 0, "DIV by 0 sets V");
}

auto test_adc_sbc_flags() -> void {
    Cpu c;
    c.load({0x88, 0x0F});  // ADC A,#$0F
    c.regs(0x01, 0, 0, 0x00);
    c.cpu.step();
    check_eq(c.r().a, 0x10, "ADC result");
    check(c.r().psw & SPC700::H, "ADC half carry");
    c.load({0x88, 0x01});
    c.regs(0x7F, 0, 0, 0x00);
    c.cpu.step();
    check_eq(c.r().psw & (SPC700::V | SPC700::N | SPC700::C), SPC700::V | SPC700::N, "ADC signed overflow");
    c.load({0xA8, 0x01});  // SBC A,#1 with C set
    c.regs(0x00, 0, 0, SPC700::C);
    c.cpu.step();
    check_eq(c.r().a, 0xFF, "SBC result");
    check_eq(c.r().psw & (SPC700::C | SPC700::N | SPC700::H), SPC700::N, "SBC borrow clears C and H");
}

auto test_word_ops() -> void {
    Cpu c;
    c.bus.mem[0x10] = 0xFF;
    c.bus.mem[0x11] = 0x00;
    c.load({0x7A, 0x10});  // ADDW YA,$10
    c.regs(0x01, 0, 0x7F, 0x00);
    c.cpu.step();
    check_eq(c.r().a | (c.r().y << 8), 0x8000, "ADDW result");
    check_eq(c.r().psw & (SPC700::N | SPC700::V | SPC700::H | SPC700::Z | SPC700::C),
             SPC700::N | SPC700::V | SPC700::H, "ADDW flags");
    c.load({0x9A, 0x10});  // SUBW YA,$10
    c.regs(0xFF, 0, 0x00, 0x00);
    c.cpu.step();
    check_eq(c.r().a | (c.r().y << 8), 0x0000, "SUBW result");
    check_eq(c.r().psw & (SPC700::Z | SPC700::C), SPC700::Z | SPC700::C, "SUBW flags");
    c.load({0x3A, 0x10});  // INCW $10 ($00FF -> $0100)
    c.regs(0, 0, 0, 0);
    c.cpu.step();
    check_eq(c.bus.mem[0x10] | (c.bus.mem[0x11] << 8), 0x0100, "INCW carries into the high byte");
    c.load({0x5A, 0x10});  // CMPW YA,$10
    c.regs(0x00, 0, 0x01, 0);
    c.cpu.step();
    check_eq(c.r().psw & (SPC700::Z | SPC700::C), SPC700::Z | SPC700::C, "CMPW equal");
    c.load({0xBA, 0xFF});  // MOVW YA,$FF: high byte wraps to $00 within the page
    c.bus.mem[0xFF] = 0x34;
    c.bus.mem[0x00] = 0x12;
    c.regs(0, 0, 0, 0);
    c.cpu.step();
    check_eq(c.r().a | (c.r().y << 8), 0x1234, "MOVW direct page wrap");
}

auto test_direct_page_flag() -> void {
    Cpu c;
    c.bus.mem[0x0010] = 0x11;
    c.bus.mem[0x0110] = 0x22;
    c.load({0xE4, 0x10, 0x40, 0xE4, 0x10, 0x8F, 0x55, 0x20});  // MOV A,$10; SETP; MOV A,$10; MOV $20,#$55
    c.regs(0, 0, 0, 0);
    c.cpu.step();
    check_eq(c.r().a, 0x11, "P=0 reads page 0");
    c.cpu.step();
    c.cpu.step();
    check_eq(c.r().a, 0x22, "P=1 reads page 1");
    c.cpu.step();
    check_eq(c.bus.mem[0x0120], 0x55, "P=1 writes page 1");
    check_eq(c.bus.mem[0x0020], 0x00, "page 0 untouched");
}

auto test_calls_and_returns() -> void {
    Cpu c;
    c.bus.mem[0xFFDE - 2 * 3] = 0x00;  // TCALL 3 vector -> $0600
    c.bus.mem[0xFFDF - 2 * 3] = 0x06;
    c.bus.mem[0x0600] = 0x6F;  // RET
    c.load({0x31, 0x4F, 0x80});  // TCALL 3; PCALL $80
    c.regs(0, 0, 0, 0);
    c.cpu.step();
    check_eq(c.r().pc, 0x0600, "TCALL vector");
    check_eq(c.r().sp, 0xED, "TCALL pushes the return address");
    check_eq(c.bus.mem[0x01EF] << 8 | c.bus.mem[0x01EE], kOrg + 1, "TCALL return address");
    c.cpu.step();
    check_eq(c.r().pc, kOrg + 1, "RET");
    c.cpu.step();
    check_eq(c.r().pc, 0xFF80, "PCALL page $FF");

    Cpu b;
    b.bus.mem[0xFFDE] = 0x00;
    b.bus.mem[0xFFDF] = 0x07;
    b.bus.mem[0x0700] = 0x7F;  // RETI
    b.load({0x0F});            // BRK
    b.regs(0, 0, 0, SPC700::I | SPC700::C);
    b.cpu.step();
    check_eq(b.r().pc, 0x0700, "BRK vector");
    check_eq(b.r().psw, SPC700::B | SPC700::C, "BRK sets B, clears I");
    b.cpu.step();
    check_eq(b.r().pc, kOrg + 1, "RETI address");
    check_eq(b.r().psw, SPC700::I | SPC700::C, "RETI restores PSW");
}

auto test_bit_ops() -> void {
    Cpu c;
    c.load({0xA2, 0x10,         // SET1 $10.5
            0x12, 0x10,         // CLR1 $10.0
            0xA3, 0x10, 0x02,   // BBS $10.5,+2
            0x00, 0x00,
            0x13, 0x10, 0x10}); // BBC $10.0,+16
    c.bus.mem[0x10] = 0x01;
    c.regs(0, 0, 0, 0);
    c.cpu.step();
    check_eq(c.bus.mem[0x10], 0x21, "SET1");
    c.cpu.step();
    check_eq(c.bus.mem[0x10], 0x20, "CLR1");
    const int taken = c.cpu.step();
    check_eq(c.r().pc, kOrg + 9, "BBS taken");
    check_eq(taken, 7, "BBS taken cycles");
    c.cpu.step();
    check_eq(c.r().pc, kOrg + 12 + 16, "BBC taken");

    Cpu t;
    t.bus.mem[0x1234] = 0x0F;
    t.load({0x0E, 0x34, 0x12, 0x4E, 0x34, 0x12});  // TSET1 !$1234; TCLR1 !$1234
    t.regs(0xF0, 0, 0, 0);
    t.cpu.step();
    check_eq(t.bus.mem[0x1234], 0xFF, "TSET1");
    check_eq(t.r().psw & (SPC700::N | SPC700::Z), SPC700::N, "TSET1 flags from A - mem");
    t.cpu.step();
    check_eq(t.bus.mem[0x1234], 0x0F, "TCLR1");

    // m.b operand: 13-bit address, bit in the top 3 bits. $1FF0 bit 3 -> 0x7FF0.
    Cpu m;
    m.bus.mem[0x1FF0] = 0x08;
    m.load({0xAA, 0xF0, 0x7F,    // MOV1 C,$1FF0.3
            0x6A, 0xF0, 0x7F,    // AND1 C,/$1FF0.3
            0x0A, 0xF0, 0x7F,    // OR1 C,$1FF0.3
            0x8A, 0xF0, 0x7F,    // EOR1 C,$1FF0.3
            0xEA, 0xF0, 0x7F,    // NOT1 $1FF0.3
            0xCA, 0xF1, 0x1F,    // MOV1 $1FF1.0,C
            0x2A, 0xF0, 0x7F});  // OR1 C,/$1FF0.3
    m.regs(0, 0, 0, 0);
    m.cpu.step();
    check(m.r().psw & SPC700::C, "MOV1 C,m.b");
    m.cpu.step();
    check(!(m.r().psw & SPC700::C), "AND1 C,/m.b");
    m.cpu.step();
    check(m.r().psw & SPC700::C, "OR1 C,m.b");
    m.cpu.step();
    check(!(m.r().psw & SPC700::C), "EOR1 C,m.b");
    m.cpu.step();
    check_eq(m.bus.mem[0x1FF0], 0x00, "NOT1");
    m.cpu.step();
    check_eq(m.bus.mem[0x1FF1], 0x00, "MOV1 m.b,C (C clear)");
    m.cpu.step();
    check(m.r().psw & SPC700::C, "OR1 C,/m.b");
}

auto test_misc_ops_and_cycles() -> void {
    Cpu c;
    c.load({0x9F, 0xCF, 0x9E, 0xD0, 0x00, 0xD0, 0x10, 0xFE, 0x00, 0xEF});  // XCN; MUL; DIV; BNE; BNE; DBNZ Y; SLEEP
    c.regs(0x12, 0x01, 0x00, 0x00);
    check_eq(c.cpu.step(), 5, "XCN cycles");
    check_eq(c.r().a, 0x21, "XCN");
    check_eq(c.cpu.step(), 9, "MUL cycles");
    check_eq(c.cpu.step(), 12, "DIV cycles");
    check_eq(c.cpu.step(), 2, "BNE not taken (Z from DIV quotient 0)");
    c.cpu.set_registers({0, 0, 1, 0xEF, 0, static_cast<Word>(kOrg + 5)});
    check_eq(c.cpu.step(), 4, "BNE taken = 2 + 2");
    c.cpu.set_registers({0, 0, 1, 0xEF, 0, static_cast<Word>(kOrg + 7)});
    check_eq(c.cpu.step(), 4, "DBNZ Y not taken");
    c.cpu.step();
    check(c.cpu.halted(), "SLEEP halts");
    const Word pc = c.r().pc;
    c.cpu.step();
    check_eq(c.r().pc, pc, "halted core stays put");

    // Every opcode decodes and returns a cycle count.
    for (int op = 0; op < 256; op++) {
        Cpu any;
        any.load({static_cast<Byte>(op), 0x00, 0x00});
        any.regs(0, 0, 0, 0);
        const int cycles = any.cpu.step();
        if (cycles < 2 || cycles > 12) check(false, "opcode cycle count out of range");
    }
}

// Each cycle is one bus call in hardware order: dummy reads, idle cycles, and a count that
// matches the opcode table for every opcode.
auto test_cycle_placement() -> void {
    auto trace_of = [](std::initializer_list<Byte> code, Byte x = 0, Byte psw = 0) {
        Cpu c;
        c.load(code);
        c.regs(0, x, 0, psw);
        c.bus.mem[0x20] = 0x01;
        c.cpu.step();
        return std::pair{c.bus.trace, c.bus.addrs};
    };
    check(trace_of({0xC4, 0x20}).first == "RRRW", "MOV d,A: fetch, fetch, dummy read, write");
    check(trace_of({0xC4, 0x20}).second == std::vector<Word>{0x0400, 0x0401, 0x0020, 0x0020}, "MOV d,A reads its target");
    check(trace_of({0xD5, 0x00, 0x03}, 4).first == "RRRIRW", "MOV !a+X,A: index idle before the dummy read");
    check(trace_of({0xAF}).first == "RRIW", "MOV (X)+,A: no dummy read");
    check(trace_of({0xBF}).first == "RRRI", "MOV A,(X)+");
    check(trace_of({0x3F, 0x00, 0x05}).first == "RRRIWWII", "CALL");
    check(trace_of({0x01}).first == "RRIWWIRR", "TCALL");
    check(trace_of({0x0F}).first == "RRWWWIRR", "BRK");
    check(trace_of({0x2D}).first == "RRWI", "PUSH A");
    check(trace_of({0xAE}).first == "RRIR", "POP A");
    check(trace_of({0xCF}).first == "RRIIIIIII", "MUL");
    check(trace_of({0x2F, 0x10}).first == "RRII", "BRA: two idles when taken");
    check(trace_of({0xF0, 0x10}).first == "RR", "BEQ not taken");
    check(trace_of({0x69, 0x20, 0x21}).first == "RRRRRI", "CMP dd,ds ends idle");
    check(trace_of({0x29, 0x20, 0x21}).first == "RRRRRW", "AND dd,ds writes");
    check(trace_of({0xBA, 0x20}).first == "RRRIR", "MOVW YA,d");
    check(trace_of({0x7A, 0x20}).first == "RRRIR", "ADDW YA,d");
    check(trace_of({0x5A, 0x20}).first == "RRRR", "CMPW YA,d");
    check(trace_of({0x0E, 0x20, 0x00}).first == "RRRRRW", "TSET1 reads twice");
    check(trace_of({0x03, 0x20, 0x10}).first == "RRRIRII", "BBS taken");

    // Every opcode: counted cycles = bus calls = the table (+2 for a taken branch).
    for (int op = 0; op < 256; op++) {
        for (Byte psw : {Byte{0x00}, Byte{0xFF}}) {
            Cpu c;
            c.load({static_cast<Byte>(op), 0x10, 0x02});
            c.regs(0x05, 0x01, 0x03, psw);
            const int cycles = c.cpu.step();
            const int table = SPC700::documented_cycles(static_cast<Byte>(op));
            if (cycles != static_cast<int>(c.bus.trace.size())) check(false, "cycles != bus calls");
            const bool branched = cycles == table + 2 && c.bus.trace.ends_with("II");
            if (cycles != table && !branched) {
                std::printf("    opcode %02X psw %02X: %d cycles, table %d (%s)\n", op, psw, cycles, table, c.bus.trace.c_str());
                check(false, "opcode cycles match the table");
            }
        }
    }
}

// Through the APU: the dummy read before a write is real (it clears a timer output), and timers
// advance inside an instruction, so a read sees the cycles before it.
auto test_smp_cycle_timing() -> void {
    APU apu;
    apu.smp_write(0xF1, 0x00);  // IPL off, timers off
    apu.smp_write(0xFA, 0x01);  // T0 target 1: an output tick every 128 cycles
    Word at = 0x0200;
    for (Byte b : {Byte{0x8F}, Byte{0x01}, Byte{0xF1}}) apu.smp_write(at++, b);  // MOV $F1,#$01 (5 cycles)
    for (int i = 0; i < 64; i++) apu.smp_write(at++, 0x00);                       // NOP x64 (128 cycles)
    for (Byte b : {Byte{0xC4}, Byte{0xFD}, Byte{0xEF}}) apu.smp_write(at++, b);   // MOV $FD,A; SLEEP
    apu.smp().set_registers({0, 0, 0, 0xEF, 0x02, 0x0200});
    apu.run_smp_cycles(5 + 128 + 4 + 3);
    check_eq(apu.smp_read(0xFD), 0, "MOV $FD,A's dummy read cleared T0OUT");

    APU b;
    b.smp_write(0xF1, 0x00);
    b.smp_write(0xFA, 0x01);
    at = 0x0200;
    for (Byte v : {Byte{0x8F}, Byte{0x01}, Byte{0xF1}}) b.smp_write(at++, v);
    for (int i = 0; i < 59; i++) b.smp_write(at++, 0x00);                       // NOP x59 (118 cycles)
    b.smp_write(at++, 0xDF);                                                     // DAA (3): 126 so far
    for (Byte v : {Byte{0xE5}, Byte{0xFD}, Byte{0x00}, Byte{0xEF}}) b.smp_write(at++, v);  // MOV A,!$00FD; SLEEP
    b.smp().set_registers({0, 0, 0, 0xEF, 0x02, 0x0200});
    b.run_smp_cycles(126 + 4 + 3);
    // The divider's 128th cycle falls inside MOV A,!$00FD, whose read is cycle 130.
    check_eq(b.smp().registers().a, 1, "timer tick inside the reading instruction is visible");
}

// ---- S-SMP memory map, ports, IPL, timers -----------------------------------------------------

auto test_ipl_and_control() -> void {
    APU apu;
    check_eq(apu.smp().registers().pc, 0xFFC0, "reset vector from the IPL ROM");
    check_eq(apu.smp_read(0xFFC0), 0xCD, "IPL ROM visible at $FFC0");
    apu.smp_write(0xFFC0, 0x5A);  // writes go to the RAM underneath
    check_eq(apu.smp_read(0xFFC0), 0xCD, "IPL ROM still visible");
    apu.smp_write(0xF1, 0x00);  // IPL off
    check_eq(apu.smp_read(0xFFC0), 0x5A, "RAM under the IPL");
    apu.smp_write(0xF2, 0x0C);
    apu.smp_write(0xF3, 0x7F);
    check_eq(apu.dsp().read(0x0C), 0x7F, "DSPADDR/DSPDATA write");
    apu.smp_write(0xF2, 0x8C);
    check_eq(apu.smp_read(0xF3), 0x7F, "DSP $80-$FF read back $00-$7F");
    apu.smp_write(0xF3, 0x00);
    check_eq(apu.dsp().read(0x0C), 0x7F, "DSP $80-$FF are read-only");
}

auto test_ports_and_mirrors() -> void {
    APU apu;
    snes::memory::Bus bus;
    bus.connect_apu(&apu);
    bus.load_test_rom(std::vector<Byte>(0x8000, 0xEA));
    bus.reset();

    bus.write(0x2140, 0x11);
    bus.write(0x2175, 0x22);  // mirror of $2141
    bus.write(0x217E, 0x33);  // mirror of $2142
    check_eq(apu.smp_read(0xF4), 0x11, "CPU -> SMP port 0");
    check_eq(apu.smp_read(0xF5), 0x22, "$2175 mirrors port 1");
    check_eq(apu.smp_read(0xF6), 0x33, "$217E mirrors port 2");
    apu.smp_write(0xF7, 0x44);
    check_eq(bus.read(0x2143), 0x44, "SMP -> CPU port 3");
    check_eq(bus.read(0x217F), 0x44, "$217F mirrors port 3");
    check_eq(apu.smp_read(0xF7), 0x00, "ports are two separate latches");
    apu.smp_write(0xF1, 0x90);  // clear input ports 0/1
    check_eq(apu.smp_read(0xF4) | apu.smp_read(0xF5), 0, "CONTROL bit 4 clears ports 0/1");
    check_eq(apu.smp_read(0xF6), 0x33, "port 2 kept");
    apu.smp_write(0xF1, 0xA0);
    check_eq(apu.smp_read(0xF6), 0x00, "CONTROL bit 5 clears ports 2/3");
}

// Runs the APU until port 0 shows `value` (or gives up), like an S-CPU wait loop.
auto wait_port0(APU& apu, Byte value) -> bool {
    for (int i = 0; i < 200000; i++) {
        if (apu.read_register(0) == value) return true;
        apu.step(48);
    }
    return false;
}

auto test_ipl_upload() -> void {
    APU apu;
    check(wait_port0(apu, 0xAA), "IPL posts $AA");
    apu.step(200);
    check_eq(apu.read_register(1), 0xBB, "IPL posts $BB");
    // Upload "mov $F5,#$42 ; bra $" to $0300 with the boot ROM's protocol, then run it.
    const Byte program[] = {0x8F, 0x42, 0xF5, 0x2F, 0xFE};
    apu.write_register(2, 0x00);
    apu.write_register(3, 0x03);
    apu.write_register(1, 0x01);
    apu.write_register(0, 0xCC);
    check(wait_port0(apu, 0xCC), "IPL acknowledges $CC");
    for (Byte i = 0; i < sizeof(program); i++) {
        apu.write_register(1, program[i]);
        apu.write_register(0, i);
        if (!wait_port0(apu, i)) {
            check(false, "IPL acknowledges each byte");
            return;
        }
    }
    apu.write_register(2, 0x00);
    apu.write_register(3, 0x03);
    apu.write_register(1, 0x00);  // 0 = jump
    apu.write_register(0, static_cast<Byte>(sizeof(program) + 1));
    check(wait_port0(apu, static_cast<Byte>(sizeof(program) + 1)), "IPL acknowledges the jump");
    apu.step(10000);
    check_eq(apu.aram()[0x0300], 0x8F, "program in ARAM");
    check_eq(apu.read_register(1), 0x42, "uploaded program ran");
}

auto test_timers() -> void {
    APU apu;
    apu.aram()[0x0300] = 0x2F;  // bra $ : keep the SMP off the I/O page (reading TnOUT clears it)
    apu.aram()[0x0301] = 0xFE;
    apu.smp().set_registers({0, 0, 0, 0xEF, 0x02, 0x0300});
    apu.smp_write(0xFC, 4);     // T2: 64 kHz / 4
    apu.smp_write(0xFA, 0);     // T0: 8 kHz / 256
    apu.smp_write(0xF1, 0x05);  // enable T0 and T2 (IPL off)
    apu.run_smp_cycles(16 * 4 * 5);
    check_eq(apu.smp_read(0xFF), 5, "T2 counts at 64 kHz / target");
    check_eq(apu.smp_read(0xFF), 0, "TnOUT clears on read");
    apu.run_smp_cycles(16 * 4 * 17);
    check_eq(apu.smp_read(0xFF), 1, "TnOUT is 4 bits (17 wraps to 1)");
    apu.run_smp_cycles(128 * 256 - 16 * 4 * 22 + 8);
    check_eq(apu.smp_read(0xFD), 1, "T0 target 0 = 256 ticks at 8 kHz");
    apu.smp_write(0xF1, 0x00);
    apu.run_smp_cycles(128 * 256 * 2);
    check_eq(apu.smp_read(0xFD), 0, "disabled timer holds");
    apu.smp_write(0xFB, 2);
    apu.smp_write(0xF1, 0x02);
    apu.run_smp_cycles(128 * 2 * 3);
    check_eq(apu.smp_read(0xFE), 3, "T1 counts at 8 kHz / target");
}

auto test_master_clock_sync() -> void {
    APU apu;
    // One NTSC second of master clocks, a scanline (1364 clocks) at a time.
    constexpr int kLines = 262 * 60;  // ~0.9984 s at 60.0988 Hz
    size_t frames = 0;
    for (int i = 0; i < 15734; i++) {  // 15734 lines * 1364 = 21.461 M clocks
        apu.step(1364);
        frames += apu.samples().size() / 2;
        apu.clear_samples();
    }
    (void)kLines;
    // 21,461,176 master clocks = 0.999245 s -> 32015.8 samples at 32040 Hz.
    check(frames >= 32014 && frames <= 32017, "32040 Hz from master clocks");
}

// ---- CPU <-> SMP port timing inside a CPU instruction --------------------------------------------
// One SMP cycle is ~20.95 master clocks. Streaming sound drivers (Star Ocean, Tales of
// Phantasia) send data through the ports on a cycle count instead of a handshake, so a port value
// must change for the other side at the moment it is written, not a whole CPU instruction early
// or late.

auto load_smp_program(APU& apu, const std::vector<Byte>& code) -> void {
    std::copy(code.begin(), code.end(), apu.aram().begin() + 0x0300);
    apu.smp_write(0xF1, 0x00);  // IPL off, timers off
    apu.smp().set_registers({0, 0, 0, 0xEF, 0x02, 0x0300});
}

// The SMP counts loop passes until port 0 changes: a CPU write 1800 clocks into an instruction
// must not be seen at the start of it.
auto test_port_write_timing() -> void {
    APU apu;
    // 0300 mov $10,#0 / 0303 inc $10 / mov a,$F4 / beq 0303 / mov $F7,a / bra $
    load_smp_program(apu, {0x8F, 0x00, 0x10, 0xAB, 0x10, 0xE4, 0xF4, 0xF0, 0xFA, 0xC4, 0xF7, 0x2F, 0xFE});
    apu.step(2000);
    const int before = apu.aram()[0x10];
    apu.cpu_write(0, 0x01, 1800);  // 1800 master clocks into the next CPU instruction
    apu.step(2000);
    const int passes = apu.aram()[0x10] - before;  // 11 SMP cycles (~230 clocks) a pass
    check(passes >= 7 && passes <= 9, "SMP sees the write 1800 clocks in (~7.8 passes)");
    check_eq(apu.port_out(3), 0x01, "SMP got the value");
    // Two writes to the same port in one instruction: the SMP sees them in order.
    APU twice;
    // 0300 mov a,$F4 / beq 0300 / mov $F7,a / bra $
    load_smp_program(twice, {0xE4, 0xF4, 0xF0, 0xFC, 0xC4, 0xF7, 0x2F, 0xFE});
    twice.step(500);
    twice.cpu_write(0, 0x05, 100);
    twice.cpu_write(0, 0x00, 2500);
    twice.step(3000);
    check_eq(twice.port_out(3), 0x05, "the earlier value was there first");
    check_eq(twice.smp_read(0xF4), 0x00, "the later write landed by the end of the step");
}

// The SMP increments port 0 every 10 cycles (~210 clocks): two CPU reads 2095 clocks apart in
// one instruction must see ~10 steps between them, and never a value from the SMP's future.
auto test_port_read_timing() -> void {
    // 0300 inc a / mov $F4,a / bra 0300
    const std::vector<Byte> counter = {0xBC, 0xC4, 0xF4, 0x2F, 0xFB};
    APU apu;
    load_smp_program(apu, counter);
    apu.step(1000);
    const Byte first = apu.cpu_read(0, 0);
    const Byte second = apu.cpu_read(0, 2095);
    apu.step(2200);
    const int diff = static_cast<Byte>(second - first);
    check(diff >= 9 && diff <= 11, "CPU read syncs the SMP to its clock (~10 steps)");
    bool monotonic = true, smooth = true;
    Byte last = apu.cpu_read(0, 0);
    for (uint64_t e = 7; e < 20000; e += 7) {
        const Byte v = apu.cpu_read(0, e);
        const int d = static_cast<Byte>(v - last);
        monotonic = monotonic && d <= 1;  // at most one SMP write per 7 clocks
        smooth = smooth && d >= 0;
        last = v;
    }
    apu.step(20000);
    check(monotonic && smooth, "reads within one instruction step by at most 1, never back");

    // Syncing at port reads doesn't change what the SMP does: same state with and without them.
    APU a, b;
    load_smp_program(a, counter);
    load_smp_program(b, counter);
    for (int i = 0; i < 3000; i++) {
        (void)a.cpu_read(1, 37);
        (void)a.cpu_read(0, 90);
        a.step(120);
        b.step(120);
    }
    check(a.aram() == b.aram() && a.port_out(0) == b.port_out(0), "same SMP progress");
    check(a.samples().size() == b.samples().size(), "same DSP sample count");
}

// The same through the bus: $2140 reads inside a CPU instruction use the bus's clock.
auto test_port_timing_bus() -> void {
    APU apu;
    snes::memory::Bus bus;
    bus.connect_apu(&apu);
    bus.load_test_rom(std::vector<Byte>(0x8000, 0xEA));
    bus.reset();
    load_smp_program(apu, {0xBC, 0xC4, 0xF4, 0x2F, 0xFB});
    apu.step(1000);
    bus.begin_cpu_step();
    const Byte first = bus.read(0x2140);
    for (int i = 0; i < 300; i++) (void)bus.read(0x7E0000);  // 8 master clocks each
    const Byte second = bus.read(0x2140);
    apu.step(static_cast<uint32_t>(bus.end_cpu_step(0)));
    const int diff = static_cast<Byte>(second - first);
    check(diff >= 10 && diff <= 13, "$2140 read 2400 clocks later sees ~11.5 more steps");
}

// ---- S-DSP ------------------------------------------------------------------------------------

auto test_brr_decode() -> void {
    std::array<int16_t, 16> out{};
    {
        const Byte block[9] = {0xC0, 0x7F, 0x80, 0, 0, 0, 0, 0, 0};  // shift 12, filter 0
        int p1 = 0, p2 = 0;
        DSP::decode_brr_block(block, out, p1, p2);
        check_eq(out[0], 28672, "nibble 7, shift 12");
        check_eq(out[1], -4096, "nibble -1, shift 12");
        check_eq(out[2], -32768, "nibble -8, shift 12");
        check_eq(out[3], 0, "nibble 0");
    }
    {
        const Byte block[9] = {0xD0, 0x7F, 0, 0, 0, 0, 0, 0, 0};  // shift 13: 0 or -2048
        int p1 = 0, p2 = 0;
        DSP::decode_brr_block(block, out, p1, p2);
        check_eq(out[0], 0, "shift 13, positive nibble -> 0");
        check_eq(out[1], -4096, "shift 13, negative nibble -> -2048");
    }
    {
        const Byte block[9] = {0x04, 0, 0, 0, 0, 0, 0, 0, 0};  // filter 1
        int p1 = 1000, p2 = 0;
        DSP::decode_brr_block(block, out, p1, p2);
        check_eq(out[0], 936, "filter 1 step 1");
        check_eq(out[1], 876, "filter 1 step 2");
    }
    {
        const Byte block[9] = {0x08, 0, 0, 0, 0, 0, 0, 0, 0};  // filter 2
        int p1 = 2000, p2 = 1000;
        DSP::decode_brr_block(block, out, p1, p2);
        check_eq(out[0], 2874, "filter 2");
    }
    {
        const Byte block[9] = {0x0C, 0, 0, 0, 0, 0, 0, 0, 0};  // filter 3
        int p1 = 2000, p2 = 1000;
        DSP::decode_brr_block(block, out, p1, p2);
        check_eq(out[0], 2778, "filter 3");
    }
    {
        const Byte block[9] = {0xC8, 0x70, 0, 0, 0, 0, 0, 0, 0};  // overflow clamps, then wraps when doubled
        int p1 = 30000, p2 = 0;
        DSP::decode_brr_block(block, out, p1, p2);
        check_eq(out[0], -2, "clamp to 32767, stored doubled with 16-bit wrap");
    }
}

auto test_gaussian() -> void {
    check_eq(snes::apu::kGaussTable[0], 0, "table start");
    check_eq(snes::apu::kGaussTable[511], 1305, "table peak");
    for (int phase = 0; phase < 256; phase++) {
        const int sum = snes::apu::kGaussTable[static_cast<size_t>(255 - phase)] + snes::apu::kGaussTable[static_cast<size_t>(511 - phase)] +
                        snes::apu::kGaussTable[static_cast<size_t>(256 + phase)] + snes::apu::kGaussTable[static_cast<size_t>(phase)];
        if (sum < 2047 || sum > 2049) check(false, "kernel weights sum to ~2048 at every phase");
    }
    check_eq(DSP::gaussian(0, 0, 10000, 0, 0), 6372, "impulse at phase 0");
    check_eq(DSP::gaussian(0, 16384, 16384, 16384, 16384), 16392, "flat input at phase 0");
    check_eq(DSP::gaussian(255, 0, 0, 10000, 0), 6372, "mirror at phase 255");
    // The first three taps sum with 16-bit wrap; only the last add clamps (hardware quirk).
    check_eq(DSP::gaussian(0, 32767, 32767, 32767, 32767), -32756, "first three taps wrap");
    check_eq(DSP::gaussian(255, 32767, 32767, 32767, 32767), 32766, "last tap clamps (and bit 0 clears)");
}

auto test_counter_rates() -> void {
    for (int rate = 1; rate < 32; rate++) {
        int fires = 0;
        for (int c = 0; c < snes::apu::kCounterRange; c++) fires += DSP::counter_fires(static_cast<uint16_t>(c), rate) ? 1 : 0;
        if (fires != snes::apu::kCounterRange / snes::apu::kCounterRate[static_cast<size_t>(rate)]) check(false, "rate fires range/rate times per cycle");
    }
    int never = 0;
    for (int c = 0; c < snes::apu::kCounterRange; c++) never += DSP::counter_fires(static_cast<uint16_t>(c), 0) ? 1 : 0;
    check_eq(never, 0, "rate 0 never fires");
}

// A DSP over its own ARAM: directory at $0200, source 0 loops a constant block at $0300,
// source 1 is a one-shot block at $0400.
struct DspRig {
    std::array<Byte, 0x10000> ram{};
    DSP dsp{ram};
    std::vector<int16_t> out;

    DspRig() {
        ram[0x0200] = 0x00; ram[0x0201] = 0x03; ram[0x0202] = 0x00; ram[0x0203] = 0x03;
        ram[0x0204] = 0x00; ram[0x0205] = 0x04; ram[0x0206] = 0x00; ram[0x0207] = 0x04;
        ram[0x0300] = 0xB3;  // shift 11, loop + end
        for (int i = 1; i < 9; i++) ram[static_cast<size_t>(0x0300 + i)] = 0x44;
        ram[0x0400] = 0xB1;  // end, no loop
        for (int i = 1; i < 9; i++) ram[static_cast<size_t>(0x0400 + i)] = 0x44;
        reg(0x6C, 0x20);  // FLG: out of reset, unmuted, echo writes off
        reg(0x5D, 0x02);  // DIR
        reg(0x0C, 0x7F);
        reg(0x1C, 0x7F);
        for (Byte v = 0; v < 2; v++) {
            reg(static_cast<Byte>(v << 4 | 0x0), 0x40);
            reg(static_cast<Byte>(v << 4 | 0x1), 0x40);
            reg(static_cast<Byte>(v << 4 | 0x3), 0x10);  // pitch $1000 = 32 kHz
        }
    }
    auto reg(Byte addr, Byte value) -> void { dsp.write(addr, value); }
    auto run(int samples) -> void {
        for (int i = 0; i < samples; i++) dsp.run_sample(out);
    }
    auto env(int v = 0) const -> int { return dsp.voice(v).envelope; }
    auto key_on(Byte mask) -> void {
        reg(0x5C, 0x00);
        reg(0x4C, mask);
        run(9);  // KON is polled every other sample, then 5 samples of setup
    }
};

auto test_envelope_gain() -> void {
    {
        DspRig r;
        r.reg(0x05, 0x00);
        r.reg(0x07, 0x7F);  // direct
        r.key_on(1);
        check_eq(r.env(), 0x7F0, "GAIN direct");
        check_eq(r.dsp.read(0x08), 0x7F, "ENVX");
        r.reg(0x07, 0x9F);  // linear decrease, rate 31
        r.run(1);
        const int a = r.env();
        r.run(1);
        check_eq(a - r.env(), 0x20, "GAIN linear decrease");
        r.reg(0x07, 0xBF);  // exponential decrease
        const int before = r.env();
        r.run(1);
        check_eq(r.env(), before - 1 - ((before - 1) >> 8), "GAIN exponential decrease");
    }
    {
        DspRig r;
        r.reg(0x07, 0xDF);  // linear increase, rate 31
        r.key_on(1);
        const int a = r.env();
        r.run(1);
        check_eq(r.env() - a, 0x20, "GAIN linear increase");
        r.run(80);
        check_eq(r.env(), 0x7FF, "linear increase clamps at $7FF");
    }
    {
        DspRig r;
        r.reg(0x07, 0xFF);  // bent line
        r.key_on(1);
        int prev = r.env();
        bool slow_seen = false, fast_ok = true;
        for (int i = 0; i < 120 && r.env() < 0x7FF; i++) {
            r.run(1);
            const int step = r.env() - prev;
            if (prev < 0x600 && step != 0x20) fast_ok = false;
            if (prev >= 0x600 && r.env() < 0x7FF && step == 0x08) slow_seen = true;
            prev = r.env();
        }
        check(fast_ok, "bent increase: +$20 below $600");
        check(slow_seen, "bent increase: +$08 from $600");
    }
}

auto test_envelope_adsr() -> void {
    DspRig r;
    r.reg(0x05, 0x8F);  // ADSR on, attack 15 (+$400 per sample), decay 0
    r.reg(0x06, 0xBF);  // sustain level 5, sustain rate 31
    r.reg(0x4C, 0x01);
    int peak = 0;
    bool reached_decay = false;
    for (int i = 0; i < 20; i++) {
        r.run(1);
        peak = std::max(peak, r.env());
        if (r.dsp.voice(0).env_mode == DSP::EnvMode::Decay) reached_decay = true;
    }
    check_eq(peak, 0x7FF, "attack reaches $7FF");
    check(reached_decay, "attack -> decay");
    r.run(8000);  // decay rate 0 steps every 64 samples
    check(r.dsp.voice(0).env_mode == DSP::EnvMode::Sustain, "decay -> sustain at SL");
    check(r.env() < 0x600, "sustain keeps falling at SR");
    r.reg(0x5C, 0x01);  // KOFF
    r.run(2);
    const int a = r.env();
    r.run(1);
    check(r.dsp.voice(0).env_mode == DSP::EnvMode::Release, "KOFF -> release");
    check_eq(a - r.env(), a >= 8 ? 8 : a, "release: -8 per sample");
    r.run(400);
    check_eq(r.env(), 0, "release ends at 0");
}

auto test_kon_endx_mute() -> void {
    DspRig r;
    r.reg(0x07, 0x7F);
    r.reg(0x14, 0x01);  // voice 1: one-shot source
    r.reg(0x17, 0x7F);
    r.reg(0x7C, 0x00);
    r.key_on(0x03);
    r.out.clear();
    r.run(32);
    bool loud = false;
    for (int16_t s : r.out) loud = loud || s != 0;
    check(loud, "keyed-on voices produce output");
    check_eq(r.dsp.read(0x7C) & 0x03, 0x03, "ENDX set for the looping and the one-shot voice");
    check_eq(r.env(1), 0, "one-shot voice silenced at its end block");
    check(r.env(0) > 0, "looping voice keeps playing");
    r.reg(0x7C, 0xFF);
    check_eq(r.dsp.read(0x7C), 0, "writing ENDX clears it");

    r.reg(0x6C, 0x60);  // mute
    r.out.clear();
    r.run(8);
    check(std::all_of(r.out.begin(), r.out.end(), [](int16_t s) { return s == 0; }), "FLG mute");
    r.reg(0x6C, 0xA0);  // soft reset
    r.run(2);
    check_eq(r.env(0), 0, "FLG soft reset silences voices");
}

auto test_noise_and_pmon() -> void {
    {
        DspRig r;
        r.reg(0x07, 0x7F);
        r.reg(0x3D, 0x01);  // NON voice 0
        r.reg(0x6C, 0x3F);  // fastest noise clock
        r.key_on(1);
        std::set<Byte> outx;
        for (int i = 0; i < 64; i++) {
            r.run(1);
            outx.insert(r.dsp.read(0x09));
        }
        check(outx.size() > 8, "noise voice output varies");
    }
    auto render = [](bool pmon) {
        DspRig r;
        r.reg(0x07, 0x7F);
        r.reg(0x17, 0x7F);
        r.reg(0x2D, pmon ? 0x02 : 0x00);
        r.key_on(0x03);
        r.run(200);
        return r.out;
    };
    check(render(true) != render(false), "PMON changes voice 1 with voice 0 playing");
}

// Echo/FIR/EFB/EDL/FLG/KON/PMON/NON stress against golden hashes. The hashes were produced by
// running the same script (tests/dsp_stress.hpp) through an independent, well-tested S-DSP
// implementation (snes_spc 0.9.0) and are kept as data only (no code from it is used); ours matches: outputs, polled ENVX/OUTX/ENDX and
// the echo writes in ARAM over 10 s per seed.
auto test_dsp_stress_golden() -> void {
    struct Golden { uint32_t seed; uint64_t hash; };
    constexpr Golden kGolden[] = {
        {1, 0xb5a0c54b32113421ull}, {2, 0x2bd82a2b3644f99aull}, {3, 0x6b2e0655d72d0f46ull},
    };
    for (const auto& g : kGolden) {
        static std::array<Byte, 0x10000> ram;
        DSP dsp(ram);
        std::vector<int16_t> out;
        const uint64_t h = dsp_stress::run(
            g.seed, 32040 * 10, ram, [&](int a, int v) { dsp.write(static_cast<Byte>(a), static_cast<Byte>(v)); },
            [&](int a) { return static_cast<int>(dsp.read(static_cast<Byte>(a))); },
            [&]() {
                out.clear();
                dsp.run_sample(out);
                return (static_cast<uint32_t>(static_cast<uint16_t>(out[0])) << 16) | static_cast<uint16_t>(out[1]);
            });
        char what[96];
        snprintf(what, sizeof(what), "seed %u: hash %016llx, reference %016llx", g.seed, static_cast<unsigned long long>(h),
                 static_cast<unsigned long long>(g.hash));
        check(h == g.hash, what);
    }
}

auto test_echo() -> void {
    auto run = [](Byte flg) {
        DspRig r;
        r.reg(0x07, 0x7F);
        r.reg(0x4D, 0x01);  // EON voice 0
        r.reg(0x6D, 0x80);  // ESA $8000
        r.reg(0x7D, 0x01);  // EDL 1 = 2 KB
        r.reg(0x0D, 0x40);  // feedback
        r.reg(0x0F, 0x7F);  // FIR tap 0
        r.reg(0x2C, 0x40);
        r.reg(0x3C, 0x40);
        r.reg(0x6C, flg);
        r.key_on(1);
        r.run(1200);
        int inside = 0, outside = 0;
        for (int a = 0x8000; a < 0x8800; a++) inside += r.ram[static_cast<size_t>(a)] != 0;
        for (int a = 0x8800; a < 0x9000; a++) outside += r.ram[static_cast<size_t>(a)] != 0;
        return std::pair{inside, outside};
    };
    const auto [in_on, out_on] = run(0x00);
    check(in_on > 1000, "echo writes its buffer");
    check_eq(out_on, 0, "EDL bounds the buffer");
    const auto [in_off, out_off] = run(0x20);
    check_eq(in_off + out_off, 0, "FLG bit 5 disables echo writes");
}

// ---- SPC700 test programs ---------------------------------------------------------------------

struct PortResult {
    int passed = 0;
    int failed = 0;
};

// Runs a PeterLemon-style SPC test until port 0 stops changing.
auto run_port_test(const std::string& path) -> std::optional<PortResult> {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<Byte> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    APU apu;
    if (!apu.load_spc(data.data(), data.size())) {
        // Raw SPC700 binary (bass "output ... create"): loaded and started at $0200.
        constexpr Word kRawOrg = 0x0200;
        std::copy_n(data.begin(), std::min<size_t>(data.size(), 0x10000 - kRawOrg), apu.aram().begin() + kRawOrg);
        apu.smp().set_registers({0, 0, 0, 0xEF, 0x02, kRawOrg});
    }
    PortResult r;
    std::set<Byte> seen;
    Byte last = apu.port_out(0);
    int quiet_ms = 0;
    constexpr int kSmpCyclesPerMs = static_cast<int>(APU::kClockHz / 24 / 1000);
    for (int ms = 0; ms < 120000 && quiet_ms < 3000; ms++) {
        apu.run_smp_cycles(kSmpCyclesPerMs);
        apu.clear_samples();
        const Byte now = apu.port_out(0);
        if (now == last) {
            quiet_ms++;
            continue;
        }
        quiet_ms = 0;
        last = now;
        if (now & 0x80) {
            r.failed++;
            printf("\n      test %d failed (A=$%02X PSW=$%02X)", now & 0x7F, apu.port_out(2), apu.port_out(1));
            break;
        }
        if (seen.insert(now).second) r.passed++;
    }
    return r;
}

} // namespace

int main(int argc, char** argv) {
    snes::utils::Logger::set_level(snes::utils::LogLevel::Error);
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"MUL/DIV/DAA/DAS (spctest vectors)", test_mul_div_daa_das},
        {"ADC/SBC flags (H, V)", test_adc_sbc_flags},
        {"ADDW/SUBW/INCW/CMPW/MOVW", test_word_ops},
        {"Direct page select (P)", test_direct_page_flag},
        {"TCALL/PCALL/BRK/RET/RETI", test_calls_and_returns},
        {"Bit ops (SET1..MOV1)", test_bit_ops},
        {"XCN, cycles, SLEEP, all opcodes", test_misc_ops_and_cycles},
        {"SMP per-cycle bus placement", test_cycle_placement},
        {"SMP timers and dummy reads per cycle", test_smp_cycle_timing},
        {"IPL ROM, CONTROL, DSP ports", test_ipl_and_control},
        {"CPU ports + $2140-$217F mirrors", test_ports_and_mirrors},
        {"IPL upload protocol", test_ipl_upload},
        {"Timers T0-T2", test_timers},
        {"Master-clock sync -> 32040 Hz", test_master_clock_sync},
        {"Port write timing inside a CPU instruction", test_port_write_timing},
        {"Port read timing inside a CPU instruction", test_port_read_timing},
        {"Port timing through the bus", test_port_timing_bus},
        {"BRR decode, filters 0-3", test_brr_decode},
        {"Gaussian interpolation", test_gaussian},
        {"Envelope counter rates", test_counter_rates},
        {"GAIN modes", test_envelope_gain},
        {"ADSR + release", test_envelope_adsr},
        {"KON/ENDX/mute/soft reset", test_kon_endx_mute},
        {"Noise + pitch modulation", test_noise_and_pmon},
        {"Echo buffer, EDL, FLG", test_echo},
        {"DSP stress vs reference hashes", test_dsp_stress_golden},
    };
    printf("=== SNES APU tests ===\n\n");
    int passed = 0, failed = 0;
    for (const auto& t : tests) {
        printf("  %s... ", t.name);
        failures_in_test = 0;
        t.fn();
        if (failures_in_test == 0) { printf("PASS\n"); passed++; } else { failed++; }
    }

    // SPC test programs: *.spc in SNES_TEST_ROMS_DIR (optional) plus anything on the command line.
    std::vector<std::string> programs;
    std::error_code ec;
    if (const std::string dir = test_rom::test_roms_dir(); !dir.empty()) {
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (e.path().extension() == ".spc") programs.push_back(e.path().string());
        }
    }
    std::sort(programs.begin(), programs.end());
    for (int i = 1; i < argc; i++) programs.emplace_back(argv[i]);
    for (const auto& path : programs) {
        printf("  %s... ", std::filesystem::path(path).filename().string().c_str());
        const auto r = run_port_test(path);
        if (!r) {
            printf("cannot open\n");
            failed++;
        } else if (r->failed == 0 && r->passed > 0) {
            printf("PASS (%d checks)\n", r->passed);
            passed++;
        } else {
            printf(" FAIL (%d passed before)\n", r->passed);
            failed++;
        }
    }
    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
