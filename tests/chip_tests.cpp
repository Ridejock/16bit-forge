// SPDX-License-Identifier: MIT
// Enhancement chips: the NEC uPD77C25 core, the Super FX (GSU), the SA-1, the Cx4, the S-DD1
// and SPC7110 (decompressors checked against reference encoders, mapping, ALU, RTC-4513),
// cartridge detection and firmware loading, battery RAM on chip boards, and save/run/load/run
// determinism with coprocessor state in the archive.
//
//   chip_tests [rom.sfc ...]
//
// Everything here runs on hand-assembled code; no game ROM or firmware is needed. ROMs given
// on the command line (free test carts and demos only) get the determinism test too.
#include "snes/coprocessor/board.hpp"
#include "snes/coprocessor/cx4.hpp"
#include "snes/coprocessor/cx4_rom.hpp"
#include "snes/coprocessor/gsu.hpp"
#include "snes/coprocessor/necdsp.hpp"
#include "snes/coprocessor/obc1.hpp"
#include "snes/coprocessor/sa1.hpp"
#include "snes/coprocessor/sdd1.hpp"
#include "snes/coprocessor/spc7110.hpp"
#include "snes/coprocessor/upd77c25.hpp"
#include "snes/cheats.hpp"
#include "snes/memory/cartridge.hpp"
#include "snes/msu1.hpp"
#include "snes/savestate.hpp"
#include "snes/system.hpp"
#include "utils/logging.hpp"

#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace snes;
using namespace snes::coprocessor;
namespace fs = std::filesystem;

int checks_failed = 0;

#define CHECK(cond)                                                           \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::printf("\n    check failed: %s (line %d)", #cond, __LINE__);       \
      checks_failed++;                                                        \
    }                                                                         \
  } while (0)
#define CHECK_EQ(a, b)                                                                          \
  do {                                                                                          \
    const auto va_ = (a);                                                                       \
    const auto vb_ = (b);                                                                       \
    if (!(va_ == vb_)) {                                                                        \
      std::printf("\n    check failed: %s == %s (got $%llX, want $%llX, line %d)", #a, #b,      \
                  static_cast<unsigned long long>(va_), static_cast<unsigned long long>(vb_), __LINE__); \
      checks_failed++;                                                                          \
    }                                                                                           \
  } while (0)

auto scratch_dir() -> fs::path {
  const fs::path dir = fs::temp_directory_path() / "snes_chip_tests";
  fs::create_directories(dir);
  return dir;
}

auto write_file(const fs::path& path, const std::vector<Byte>& data) -> void {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

// ==== uPD77C25 =====================================================================================

// Instruction encoders (24-bit words).
constexpr auto op(unsigned pselect, unsigned alu, unsigned asl, unsigned dpl, unsigned dphm, unsigned rpdcr,
                  unsigned src, unsigned dst) -> uint32_t {
  return pselect << 20 | alu << 16 | asl << 15 | dpl << 13 | dphm << 9 | rpdcr << 8 | src << 4 | dst;
}
constexpr auto rt(uint32_t operation) -> uint32_t { return 0x400000 | operation; }
constexpr auto jp(unsigned brch, unsigned target) -> uint32_t { return 0x800000 | brch << 13 | target << 2; }
constexpr auto ld(unsigned id, unsigned dst) -> uint32_t { return 0xC00000 | id << 6 | dst; }

// Register numbers for src / dst.
enum : unsigned { kSrcTrb = 0, kSrcA = 1, kSrcB = 2, kSrcDr = 8, kSrcMem = 15 };
enum : unsigned { kDstA = 1, kDstB = 2, kDstDp = 4, kDstRp = 5, kDstDr = 6, kDstSr = 7, kDstK = 10, kDstL = 13, kDstMem = 15 };
enum : unsigned { kAluOr = 1, kAluAnd = 2, kAluSub = 4, kAluAdd = 5, kAluDec = 8, kAluInc = 9, kAluShr1 = 11, kAluShl1 = 12, kAluXchg = 15 };
constexpr unsigned kIdb = 1;  // pselect: the source register

auto load_program(UPD77C25& dsp, const std::vector<uint32_t>& program) -> void {
  for (size_t i = 0; i < program.size(); i++) dsp.set_program_word(static_cast<uint32_t>(i), program[i]);
  dsp.reset();
}

auto test_upd_alu() -> bool {
  UPD77C25 dsp;
  load_program(dsp, {
    ld(0x1234, kDstA), ld(0x0001, kDstB),
    op(kIdb, kAluAdd, 0, 0, 0, 0, kSrcB, 0),            // A += B
    ld(0x7FFF, kDstA), op(kIdb, kAluAdd, 0, 0, 0, 0, kSrcB, 0),  // overflow into the sign
    ld(0x0000, kDstB), op(kIdb, kAluDec, 1, 0, 0, 0, 0, 0),      // B = 0 - 1: borrow
    ld(0x8001, kDstA), op(0, kAluShr1, 0, 0, 0, 0, 0, 0),        // arithmetic shift right
    ld(0x12F0, kDstA), op(0, kAluXchg, 0, 0, 0, 0, 0, 0),        // byte swap
    ld(0x00FF, kDstB), op(kIdb, kAluAnd, 0, 0, 0, 0, kSrcB, 0),  // A &= B
  });
  dsp.exec(); dsp.exec(); dsp.exec();
  CHECK_EQ(static_cast<uint16_t>(dsp.regs().a), 0x1235);
  CHECK(!dsp.regs().fa.z && !dsp.regs().fa.c && !dsp.regs().fa.ov0 && !dsp.regs().fa.s0);
  dsp.exec(); dsp.exec();
  CHECK_EQ(static_cast<uint16_t>(dsp.regs().a), 0x8000);
  CHECK(dsp.regs().fa.ov0 && dsp.regs().fa.s0 && !dsp.regs().fa.c);
  dsp.exec(); dsp.exec();
  CHECK_EQ(static_cast<uint16_t>(dsp.regs().b), 0xFFFF);
  CHECK(dsp.regs().fb.c && dsp.regs().fb.s0 && !dsp.regs().fb.z);
  dsp.exec(); dsp.exec();
  CHECK_EQ(static_cast<uint16_t>(dsp.regs().a), 0xC000);
  CHECK(dsp.regs().fa.c);
  dsp.exec(); dsp.exec();
  CHECK_EQ(static_cast<uint16_t>(dsp.regs().a), 0xF012);
  dsp.exec(); dsp.exec();
  CHECK_EQ(static_cast<uint16_t>(dsp.regs().a), 0x0012);
  CHECK(!dsp.regs().fa.z && !dsp.regs().fa.c);
  return true;
}

auto test_upd_multiplier_memory() -> bool {
  UPD77C25 dsp;
  load_program(dsp, {
    ld(0x4000, kDstK), ld(0x4000, kDstL),   // 0.5 * 0.5 in Q15
    ld(0x0003, kDstK), ld(0xFFFB, kDstL),   // 3 * -5
    ld(0x0005, kDstDp), ld(0xBEEF, kDstMem),
    op(0, 0, 0, 1, 0, 0, 0, 0),             // DPINC
    op(0, 0, 0, 0, 1, 0, 0, 0),             // DP high nibble ^= 1
    op(0, kAluOr, 0, 3, 0, 0, kSrcMem, kDstA),  // A |= RAM[DP] (=0), A <- idb... then DPCLR
  });
  dsp.exec(); dsp.exec();
  CHECK_EQ(static_cast<uint16_t>(dsp.regs().m), 0x2000);
  CHECK_EQ(static_cast<uint16_t>(dsp.regs().n), 0x0000);
  dsp.exec(); dsp.exec();
  // -15 as sign + 30 bits: M = -15 >> 15 = -1, N = (-15 << 1) & 0xFFFF
  CHECK_EQ(static_cast<uint16_t>(dsp.regs().m), 0xFFFF);
  CHECK_EQ(static_cast<uint16_t>(dsp.regs().n), 0xFFE2);
  dsp.exec(); dsp.exec();
  CHECK_EQ(dsp.data_ram()[5], 0xBEEF);
  dsp.exec();
  CHECK_EQ(dsp.regs().dp, 6);
  dsp.exec();
  CHECK_EQ(dsp.regs().dp, 0x16);
  dsp.exec();
  CHECK_EQ(dsp.regs().dp, 0x10);  // DPCLR clears the low nibble
  return true;
}

auto test_upd_jumps() -> bool {
  UPD77C25 dsp;
  load_program(dsp, {
    /*0*/ ld(3, kDstB),
    /*1*/ op(0, kAluDec, 1, 0, 0, 0, 0, 0),  // B--
    /*2*/ jp(0x08C, 1),                      // JNZB 1
    /*3*/ jp(0x140, 6),                      // CALL 6
    /*4*/ ld(0xAAAA, kDstA),
    /*5*/ jp(0x100, 5),                      // JMP $ (park)
    /*6*/ rt(op(0, kAluInc, 1, 0, 0, 0, 0, 0)),  // B++ and return
  });
  int steps = 0;
  while (dsp.regs().pc != 5 && steps < 100) { dsp.exec(); steps++; }
  CHECK_EQ(steps, 1 + 3 * 2 + 1 + 1 + 1);
  CHECK_EQ(static_cast<uint16_t>(dsp.regs().a), 0xAAAA);
  CHECK_EQ(static_cast<uint16_t>(dsp.regs().b), 1);
  CHECK_EQ(dsp.regs().sp, 0);
  return true;
}

auto test_upd_host_port() -> bool {
  UPD77C25 dsp;
  load_program(dsp, {
    ld(0xABCD, kDstDr),          // 16-bit reply
    jp(0x0BE, 1),                // JRQM $: wait for the host to take it
    op(0, 0, 0, 0, 0, 0, kSrcDr, kDstA),  // A = DR (what the host wrote), RQM again
    ld(0x0400, kDstSr),          // DRC: 8-bit transfers
    jp(0x100, 4),
  });
  dsp.exec();
  CHECK(dsp.read_sr() & 0x80);   // RQM
  dsp.exec();
  CHECK_EQ(dsp.regs().pc, 1);    // still waiting
  CHECK_EQ(dsp.read_dr(), 0xCD);
  CHECK(dsp.read_sr() & 0x10);   // DRS: halfway
  CHECK_EQ(dsp.read_dr(), 0xAB);
  CHECK(!(dsp.read_sr() & 0x80));
  dsp.exec();
  CHECK_EQ(dsp.regs().pc, 2);
  dsp.write_dr(0x34);
  dsp.write_dr(0x12);
  dsp.exec();
  CHECK_EQ(static_cast<uint16_t>(dsp.regs().a), 0x1234);
  CHECK(dsp.read_sr() & 0x80);
  dsp.exec();
  CHECK(dsp.read_sr() & 0x04);   // DRC
  dsp.write_dr(0x55);            // one byte completes a transfer
  CHECK(!(dsp.read_sr() & 0x80));
  return true;
}

auto test_necdsp_board() -> bool {
  constexpr NecBoard board{"test", {0x30, 0x3F, 0x8000, 0xFFFF, 0x4000}, std::nullopt};
  NecDsp dsp(Chip::DSP1B, kUpd7725, board);
  dsp.core().set_program_word(0, ld(0x5AA5, kDstDr));
  dsp.core().set_program_word(1, jp(0x100, 1));
  dsp.reset();
  CHECK(!dsp.read(0x008000).has_value());        // not the DSP's window: ROM answers
  dsp.step(100);
  CHECK_EQ(dsp.read(0x30C000).value_or(0), 0x80);  // SR (A14 set): RQM
  CHECK_EQ(dsp.read(0xB08000).value_or(0), 0xA5);  // DR, $80 mirror
  CHECK_EQ(dsp.read(0x3F8000).value_or(0), 0x5A);
  // Save state round trip.
  Archive saver = Archive::saver();
  dsp.serialize(saver);
  NecDsp copy(Chip::DSP1B, kUpd7725, board);
  Archive loader = Archive::loader(saver.data());
  copy.serialize(loader);
  CHECK(loader.ok() && loader.remaining() == 0);
  CHECK_EQ(copy.core().regs().pc, dsp.core().regs().pc);
  return true;
}

// ==== Super FX =====================================================================================

struct GsuRig {
  std::vector<Byte> rom = std::vector<Byte>(0x100000, 0x01);  // NOP fill
  std::vector<Byte> ram = std::vector<Byte>(0x10000, 0x00);
  GSU gsu{rom, ram};

  // Code at $00:8000 (ROM offset 0); returns clocks taken.
  auto run(const std::vector<Byte>& code, Byte scmr = 0x18) -> uint64_t {
    std::copy(code.begin(), code.end(), rom.begin());
    gsu.reset();
    gsu.write(0x00303A, scmr);
    gsu.write(0x003039, 1);       // 21 MHz
    gsu.write(0x003034, 0);       // PBR
    gsu.write(0x00301E, 0x00);
    gsu.write(0x00301F, 0x80);    // R15 = $8000: go
    return gsu.run_until_stop();
  }
};

auto test_gsu_alu() -> bool {
  GsuRig rig;
  rig.run({0xF1, 0x34, 0x12,       // IWT R1,#$1234
           0xF2, 0x0F, 0x0F,       // IWT R2,#$0F0F
           0xB1, 0x13, 0x72,       // FROM R1 TO R3 AND R2
           0xB1, 0x14, 0x3E, 0x51, // FROM R1 TO R4 ADD #1
           0x00, 0x01});
  CHECK_EQ(rig.gsu.reg(3), 0x0204);
  CHECK_EQ(rig.gsu.reg(4), 0x1235);
  CHECK(!rig.gsu.running());

  rig.run({0xF1, 0xFF, 0x7F, 0xB1, 0x14, 0x3E, 0x51, 0x00, 0x01});  // $7FFF + 1
  CHECK_EQ(rig.gsu.reg(4), 0x8000);
  CHECK(rig.gsu.sfr() & GSU::kOv);
  CHECK(rig.gsu.sfr() & GSU::kS);
  CHECK(!(rig.gsu.sfr() & GSU::kCy));

  rig.run({0xF1, 0x05, 0x00, 0xF2, 0x07, 0x00, 0xB1, 0x3F, 0x62, 0x00, 0x01});  // CMP R2: 5 - 7
  CHECK(rig.gsu.sfr() & GSU::kS);
  CHECK(!(rig.gsu.sfr() & GSU::kCy));  // borrow
  CHECK(!(rig.gsu.sfr() & GSU::kZ));
  CHECK_EQ(rig.gsu.reg(1), 5);         // CMP writes nothing

  rig.run({0xF1, 0x81, 0x00, 0xB1, 0x15, 0x95,   // SEX: $81 -> $FF81
           0xB1, 0x16, 0x96,                     // ASR: $0081 >> 1
           0xB1, 0x17, 0x4F,                     // NOT
           0xB1, 0x18, 0x4D,                     // SWAP
           0xF2, 0x34, 0x12, 0xB2, 0x19, 0xC0,   // HIB
           0xB2, 0x1A, 0x9E,                     // LOB
           0x00, 0x01});
  CHECK_EQ(rig.gsu.reg(5), 0xFF81);
  CHECK_EQ(rig.gsu.reg(6), 0x0040);
  CHECK_EQ(rig.gsu.reg(7), 0xFF7E);
  CHECK_EQ(rig.gsu.reg(8), 0x8100);
  CHECK_EQ(rig.gsu.reg(9), 0x0012);
  CHECK_EQ(rig.gsu.reg(10), 0x0034);
  return true;
}

auto test_gsu_multiply() -> bool {
  GsuRig rig;
  rig.run({0xF1, 0xFD, 0xFF, 0xF2, 0x05, 0x00,
           0xB1, 0x15, 0x82,             // MULT R2: -3 * 5 (signed 8x8)
           0xB1, 0x13, 0x3D, 0x82,       // UMULT R2: $FD * 5
           0xF1, 0x34, 0x12, 0xF6, 0x78, 0x56,
           0xB1, 0x17, 0x9F,             // FMULT: (R1 * R6) >> 16
           0xB1, 0x18, 0x3D, 0x9F,       // LMULT: high -> R8, low -> R4
           0x00, 0x01});
  CHECK_EQ(rig.gsu.reg(5), 0xFFF1);
  CHECK_EQ(rig.gsu.reg(3), 0x04F1);
  CHECK_EQ(rig.gsu.reg(7), 0x0626);
  CHECK_EQ(rig.gsu.reg(8), 0x0626);
  CHECK_EQ(rig.gsu.reg(4), 0x0060);
  return true;
}

auto test_gsu_loop_memory() -> bool {
  GsuRig rig;
  // R12 = 5, R13 = loop; loop: INC R1; LOOP; (delay slot NOP). Then store R1 to RAM and read back.
  rig.run({0xFC, 0x05, 0x00,     // 8000 IWT R12,#5
           0xA1, 0x00,           // 8003 IBT R1,#0
           0xFD, 0x08, 0x80,     // 8005 IWT R13,#$8008
           0xD1,                 // 8008 INC R1
           0x3C,                 // 8009 LOOP
           0x01,                 // 800A NOP
           0xF3, 0x20, 0x00,     // IWT R3,#$0020
           0xB1, 0x33,           // FROM R1 STW (R3)
           0x14, 0x43,           // TO R4 LDW (R3)
           0x15, 0x3D, 0x43,     // TO R5 LDB (R3)
           0x00, 0x01});
  CHECK_EQ(rig.gsu.reg(1), 5);
  CHECK_EQ(rig.gsu.reg(12), 0);
  CHECK_EQ(rig.ram[0x20], 5);
  CHECK_EQ(rig.ram[0x21], 0);
  CHECK_EQ(rig.gsu.reg(4), 5);
  CHECK_EQ(rig.gsu.reg(5), 5);
  return true;
}

// PLOT in 2bpp and 4bpp, 128-line screen at SCBR 0: pixel (3, 2) is tile 0, row 2, bit 4.
auto test_gsu_plot() -> bool {
  GsuRig rig;
  const std::vector<Byte> code = {
    0xA1, 0x03, 0xA2, 0x02,     // IBT R1,#3  IBT R2,#2
    0xA0, 0x0F, 0x4E,           // IBT R0,#15  COLOR
    0x4C,                       // PLOT (R1++)
    0xA1, 0x03,
    0x13, 0x3D, 0x4C,           // TO R3 RPIX (flushes the pixel cache)
    0x00, 0x01};
  rig.run(code, 0x18);          // 2bpp, RON|RAN
  CHECK_EQ(rig.ram[4], 0x10);
  CHECK_EQ(rig.ram[5], 0x10);
  CHECK_EQ(rig.ram[20], 0x00);
  CHECK_EQ(rig.gsu.reg(3), 3);  // 2bpp keeps the low 2 bits
  std::fill(rig.ram.begin(), rig.ram.end(), 0);
  rig.run(code, 0x19);          // 4bpp
  CHECK_EQ(rig.ram[4], 0x10);
  CHECK_EQ(rig.ram[5], 0x10);
  CHECK_EQ(rig.ram[20], 0x10);
  CHECK_EQ(rig.ram[21], 0x10);
  CHECK_EQ(rig.gsu.reg(3), 15);
  CHECK_EQ(rig.gsu.reg(1), 3);
  // Next column of tiles in 128-line mode starts 16 tiles on: x = 8 -> tile 16.
  std::fill(rig.ram.begin(), rig.ram.end(), 0);
  rig.run({0xA1, 0x08, 0xA2, 0x00, 0xA0, 0x01, 0x4E, 0x4C, 0xA1, 0x08, 0x13, 0x3D, 0x4C, 0x00, 0x01}, 0x18);
  CHECK_EQ(rig.ram[16 * 16], 0x80);
  return true;
}

auto test_gsu_cpu_interface() -> bool {
  GsuRig rig;
  // A long loop so the GSU is still running when the S-CPU looks.
  const std::vector<Byte> code = {0xFC, 0xFF, 0x7F, 0xFD, 0x06, 0x80, 0x3C, 0x01, 0x00, 0x01};
  std::copy(code.begin(), code.end(), rig.rom.begin());
  rig.rom[0x10] = 0x77;
  rig.gsu.reset();
  rig.gsu.write(0x00303A, 0x18);
  rig.gsu.write(0x003039, 1);
  rig.gsu.write(0x003037, 0x00);  // IRQ on STOP
  rig.gsu.write(0x00301E, 0x00);
  rig.gsu.write(0x00301F, 0x80);
  rig.gsu.step(1000);
  CHECK(rig.gsu.running());
  CHECK(rig.gsu.read(0x003030).value_or(0) & 0x20);       // SFR GO
  CHECK_EQ(rig.gsu.read(0x008010).value_or(0), 0x00);     // RON: vectors instead of ROM
  CHECK_EQ(rig.gsu.read(0x00800A).value_or(0), 0x08);
  CHECK(!rig.gsu.read(0x700000).has_value());             // RAN: RAM is open bus
  for (int i = 0; i < 2000 && rig.gsu.running(); i++) rig.gsu.step(10000);
  CHECK(!rig.gsu.running());
  CHECK(rig.gsu.irq());
  CHECK(rig.gsu.read(0x003031).value_or(0) & 0x80);       // IRQ flag, cleared by the read
  CHECK(!rig.gsu.irq());
  CHECK_EQ(rig.gsu.read(0x008010).value_or(0), 0x77);     // stopped: ROM is back
  CHECK_EQ(rig.gsu.read(0x700000).value_or(0xEE), 0x00);
  // CFGR bit 7 masks the IRQ.
  rig.gsu.write(0x003037, 0x80);
  rig.gsu.write(0x00301E, 0x08);
  rig.gsu.write(0x00301F, 0x80);                          // straight to STOP
  for (int i = 0; i < 10 && rig.gsu.running(); i++) rig.gsu.step(1000);
  CHECK(!rig.gsu.running());
  CHECK(!rig.gsu.irq());
  // 10 MHz (CLSR 0) takes longer than 21 MHz for the same loop.
  GsuRig fast, slow;
  const std::vector<Byte> loop = {0xFC, 0x00, 0x01, 0xFD, 0x06, 0x80, 0x3C, 0x01, 0x00, 0x01};
  const uint64_t fast_clocks = fast.run(loop);
  std::copy(loop.begin(), loop.end(), slow.rom.begin());
  slow.gsu.reset();
  slow.gsu.write(0x00303A, 0x18);
  slow.gsu.write(0x003039, 0);
  slow.gsu.write(0x00301E, 0x00);
  slow.gsu.write(0x00301F, 0x80);
  const uint64_t slow_clocks = slow.gsu.run_until_stop();
  CHECK(slow_clocks > fast_clocks);
  return true;
}

auto test_gsu_state() -> bool {
  GsuRig rig;
  rig.run({0xF1, 0x34, 0x12, 0xA0, 0x05, 0x4E, 0x00, 0x01});
  Archive saver = Archive::saver();
  rig.gsu.serialize(saver);
  GsuRig other;
  Archive loader = Archive::loader(saver.data());
  other.gsu.serialize(loader);
  CHECK(loader.ok() && loader.remaining() == 0);
  CHECK_EQ(other.gsu.reg(1), 0x1234);
  CHECK_EQ(other.gsu.colr(), 5);
  return true;
}

// ==== SA-1 =========================================================================================

struct Sa1Rig {
  std::vector<Byte> rom = std::vector<Byte>(0x100000, 0x00);
  std::vector<Byte> bwram = std::vector<Byte>(0x10000, 0x00);
  SA1 sa1{rom, bwram};
  auto mr() -> uint64_t {
    uint64_t value = 0;
    for (int i = 4; i >= 0; i--) value = value << 8 | sa1.sa1_read(0x002306 + static_cast<Address>(i));
    return value;
  }
  auto arith(Byte mode, Word ma, Word mb) -> void {
    if (mode != 0xFF) sa1.sa1_write(0x002250, mode);
    sa1.sa1_write(0x002251, static_cast<Byte>(ma));
    sa1.sa1_write(0x002252, static_cast<Byte>(ma >> 8));
    sa1.sa1_write(0x002253, static_cast<Byte>(mb));
    sa1.sa1_write(0x002254, static_cast<Byte>(mb >> 8));
  }
};

auto test_sa1_arithmetic() -> bool {
  Sa1Rig rig;
  rig.arith(0x00, 0xFFFD, 0x0005);                 // signed multiply
  CHECK_EQ(rig.mr() & 0xFFFFFFFF, 0xFFFFFFF1ull);
  rig.arith(0x00, 0x7FFF, 0x7FFF);
  CHECK_EQ(rig.mr() & 0xFFFFFFFF, 0x3FFF0001ull);
  // Division: signed dividend / unsigned divisor (values measured on hardware).
  struct Div { Word ma, mb, quotient, remainder; };
  for (const Div d : {Div{0x53FE, 0x0037, 0x0186, 0x0034}, Div{0x8000, 0x0101, 0xFF81, 0x0081},
                      Div{0x87F8, 0xFFFB, 0x0000, 0x7808}, Div{0x8000, 0x8000, 0xFFFF, 0x0000},
                      Div{0x1234, 0x0000, 0xFFFF, 0x1234}, Div{0x8000, 0x0000, 0x0001, 0x8000}}) {
    rig.arith(0x01, d.ma, d.mb);
    CHECK_EQ(rig.mr() & 0xFFFF, d.quotient);
    CHECK_EQ((rig.mr() >> 16) & 0xFFFF, d.remainder);
  }
  // Cumulative sum: 40 bits, OF on carry out.
  rig.sa1.sa1_write(0x002250, 0x02);              // also clears MR
  CHECK_EQ(rig.mr(), 0ull);
  rig.arith(0xFF, 0x7FFF, 0x7FFF);
  rig.arith(0xFF, 0x7FFF, 0x7FFF);
  CHECK_EQ(rig.mr(), 0x7FFE0002ull);
  rig.arith(0xFF, 0xFFFF, 0x0002);                // -2
  CHECK_EQ(rig.mr(), 0x7FFE0000ull);
  CHECK_EQ(rig.sa1.sa1_read(0x00230B), 0x00);
  rig.sa1.sa1_write(0x002250, 0x02);
  for (int i = 0; i < 1024; i++) rig.arith(0xFF, 0x8000, 0x8000);  // 1024 * 2^30 = 2^40
  CHECK_EQ(rig.mr(), 0ull);
  CHECK_EQ(rig.sa1.sa1_read(0x00230B), 0x80);
  return true;
}

auto test_sa1_bit_reader() -> bool {
  Sa1Rig rig;
  const Byte data[] = {0x12, 0x34, 0x56, 0x78, 0x9A};
  std::memcpy(rig.rom.data(), data, sizeof(data));
  rig.sa1.sa1_write(0x002259, 0x00);
  rig.sa1.sa1_write(0x00225A, 0x80);
  rig.sa1.sa1_write(0x00225B, 0x00);               // VA = $00:8000 (ROM 0)
  CHECK_EQ(rig.sa1.sa1_read(0x00230C), 0x12);
  CHECK_EQ(rig.sa1.sa1_read(0x00230D), 0x34);
  rig.sa1.sa1_write(0x002258, 0x04);               // fixed mode: advance 4 bits
  CHECK_EQ(rig.sa1.sa1_read(0x00230C), 0x41);
  CHECK_EQ(rig.sa1.sa1_read(0x00230D), 0x63);
  rig.sa1.sa1_write(0x002258, 0x8C);               // auto mode, 12 bits per read of $230D
  CHECK_EQ(rig.sa1.sa1_read(0x00230D), 0x63);      // 4 + 12 = 16 bits on
  CHECK_EQ(rig.sa1.sa1_read(0x00230C), 0x56);
  CHECK_EQ(rig.sa1.sa1_read(0x00230D), 0x78);      // then 28 bits
  CHECK_EQ(rig.sa1.sa1_read(0x00230C), 0xA7);
  return true;
}

auto test_sa1_dma() -> bool {
  Sa1Rig rig;
  for (int i = 0; i < 16; i++) rig.rom[0x10 + static_cast<size_t>(i)] = static_cast<Byte>(0xA0 + i);
  // ROM -> I-RAM
  rig.sa1.sa1_write(0x002230, 0x80);               // DMA on, ROM -> I-RAM
  rig.sa1.sa1_write(0x002232, 0x10);
  rig.sa1.sa1_write(0x002233, 0x80);
  rig.sa1.sa1_write(0x002234, 0x00);               // DSA $00:8010
  rig.sa1.sa1_write(0x002238, 16);
  rig.sa1.sa1_write(0x002239, 0);
  rig.sa1.sa1_write(0x002235, 0x00);
  rig.sa1.sa1_write(0x002236, 0x01);               // DDA I-RAM $100: starts
  for (int i = 0; i < 16; i++) CHECK_EQ(rig.sa1.iram()[0x100 + static_cast<size_t>(i)], 0xA0 + i);
  CHECK(rig.sa1.sa1_read(0x002301) & 0x20);        // DMA IRQ flag
  // I-RAM -> BW-RAM
  rig.sa1.sa1_write(0x002230, 0x86);               // I-RAM -> BW-RAM
  rig.sa1.sa1_write(0x002232, 0x00);
  rig.sa1.sa1_write(0x002233, 0x01);
  rig.sa1.sa1_write(0x002234, 0x00);
  rig.sa1.sa1_write(0x002238, 16);
  rig.sa1.sa1_write(0x002239, 0);
  rig.sa1.sa1_write(0x002235, 0x00);
  rig.sa1.sa1_write(0x002236, 0x02);
  CHECK_EQ(rig.bwram[0x200], 0x00);               // waits for the bank byte
  rig.sa1.sa1_write(0x002237, 0x00);
  for (int i = 0; i < 16; i++) CHECK_EQ(rig.bwram[0x200 + static_cast<size_t>(i)], 0xA0 + i);
  return true;
}

// The SNES bitplane layout of one 8-pixel row of a tile: planes 0/1 interleaved, then 2/3...
auto planar_row(const Byte* pixels, int bpp) -> std::vector<Byte> {
  std::vector<Byte> planes(static_cast<size_t>(bpp), 0);
  for (int x = 0; x < 8; x++) {
    for (int p = 0; p < bpp; p++) planes[static_cast<size_t>(p)] |= static_cast<Byte>(((pixels[x] >> p) & 1) << (7 - x));
  }
  return planes;
}
auto plane_offset(int plane) -> size_t { return static_cast<size_t>((plane & 1) + (plane >> 1) * 16); }

auto test_sa1_character_conversion() -> bool {
  // Type 2: the SA-1 writes rows of 8 pixels to BRF, each becomes a bitplane row in I-RAM.
  {
    Sa1Rig rig;
    rig.sa1.sa1_write(0x002231, 0x01);             // 4bpp
    rig.sa1.sa1_write(0x002230, 0xA0);             // DMA on, character conversion, type 2
    rig.sa1.sa1_write(0x002235, 0x00);
    rig.sa1.sa1_write(0x002236, 0x00);             // DDA I-RAM 0
    const Byte rows[2][8] = {{1, 2, 3, 4, 5, 6, 7, 8}, {15, 0, 15, 0, 9, 9, 9, 9}};
    for (int row = 0; row < 2; row++) {
      for (int i = 0; i < 8; i++) rig.sa1.sa1_write(0x002240 + static_cast<Address>(row * 8 + i), rows[row][i]);
    }
    for (int row = 0; row < 2; row++) {
      const auto want = planar_row(rows[row], 4);
      for (int p = 0; p < 4; p++) {
        CHECK_EQ(rig.sa1.iram()[static_cast<size_t>(row * 2) + plane_offset(p)], want[static_cast<size_t>(p)]);
      }
    }
  }
  // Type 1: the S-CPU reads tiles out of BW-RAM and gets them converted from the packed bitmap.
  {
    Sa1Rig rig;
    auto pixel = [](int x, int y) { return static_cast<Byte>((x * 3 + y * 5) & 15); };
    for (int y = 0; y < 8; y++) {
      for (int x = 0; x < 8; x += 2) {
        rig.bwram[static_cast<size_t>(y * 4 + x / 2)] = static_cast<Byte>(pixel(x, y) | pixel(x + 1, y) << 4);
      }
    }
    rig.sa1.sa1_write(0x002231, 0x01);             // 4bpp, 1 character per line
    rig.sa1.sa1_write(0x002230, 0xB0);             // character conversion, type 1
    rig.sa1.sa1_write(0x002232, 0x00);
    rig.sa1.sa1_write(0x002233, 0x00);
    rig.sa1.sa1_write(0x002234, 0x40);             // DSA BW-RAM $40:0000
    rig.sa1.sa1_write(0x002235, 0x00);
    rig.sa1.sa1_write(0x002236, 0x04);             // DDA I-RAM $400: starts
    CHECK(rig.sa1.read(0x002300).value_or(0) & 0x20);  // character DMA IRQ flag for the S-CPU
    std::vector<Byte> tile;
    for (Address i = 0; i < 32; i++) tile.push_back(rig.sa1.read(0x400000 + i).value_or(0));
    for (int y = 0; y < 8; y++) {
      Byte row[8];
      for (int x = 0; x < 8; x++) row[x] = pixel(x, y);
      const auto want = planar_row(row, 4);
      for (int p = 0; p < 4; p++) CHECK_EQ(tile[static_cast<size_t>(y * 2) + plane_offset(p)], want[static_cast<size_t>(p)]);
    }
    rig.sa1.write(0x002231, 0x80);                 // CHDEND: back to plain BW-RAM reads
    CHECK_EQ(rig.sa1.read(0x400000).value_or(0), rig.bwram[0]);
  }
  return true;
}

auto test_sa1_bitmap_and_protection() -> bool {
  Sa1Rig rig;
  // 4bpp bitmap view at $60:0000: two pixels per byte, low nibble first.
  rig.sa1.sa1_write(0x002227, 0x80);               // CWEN
  rig.sa1.sa1_write(0x600000, 0x0A);
  rig.sa1.sa1_write(0x600001, 0x05);
  CHECK_EQ(rig.bwram[0], 0x5A);
  CHECK_EQ(rig.sa1.sa1_read(0x600001), 0x05);
  rig.sa1.sa1_write(0x00223F, 0x80);               // 2bpp: four pixels per byte
  rig.sa1.sa1_write(0x600007, 0x03);
  CHECK_EQ(rig.bwram[1], 0xC0);
  // Bitmap projection in the $6000 window (BMAP bit 7).
  rig.sa1.sa1_write(0x002225, 0x80);
  CHECK_EQ(rig.sa1.sa1_read(0x006007), 0x03);
  // BW-RAM write protection: the first 256 << BWPA bytes are protected unless SWEN/CWEN.
  rig.sa1.write(0x002226, 0x00);                   // SWEN off
  rig.sa1.write(0x002228, 0x00);                   // protect $000-$0FF
  rig.sa1.sa1_write(0x002227, 0x00);               // CWEN off
  rig.sa1.write(0x400010, 0x99);
  CHECK(rig.bwram[0x10] != 0x99);
  rig.sa1.write(0x400200, 0x99);
  CHECK_EQ(rig.bwram[0x200], 0x99);
  rig.sa1.write(0x002226, 0x80);
  rig.sa1.write(0x400010, 0x99);
  CHECK_EQ(rig.bwram[0x10], 0x99);
  // I-RAM write protection for the S-CPU (SIWP, 256-byte pages).
  rig.sa1.write(0x002229, 0x02);
  rig.sa1.write(0x003010, 0x11);
  rig.sa1.write(0x003110, 0x22);
  CHECK_EQ(rig.sa1.iram()[0x010], 0x00);
  CHECK_EQ(rig.sa1.iram()[0x110], 0x22);
  return true;
}

auto test_sa1_interrupts_and_mmc() -> bool {
  Sa1Rig rig;
  // SA-1 -> S-CPU IRQ: SCNT bit 7, enabled by SIE, cleared by SIC.
  rig.sa1.write(0x002201, 0x80);
  rig.sa1.sa1_write(0x002209, 0x85);
  CHECK(rig.sa1.irq());
  CHECK_EQ(rig.sa1.read(0x002300).value_or(0) & 0x8F, 0x85);
  rig.sa1.write(0x002202, 0x80);
  CHECK(!rig.sa1.irq());
  // S-CPU vector switch: SCNT IVSW/NVSW replace $FFEE/$FFEA.
  rig.rom[0x7FEE] = 0x11; rig.rom[0x7FEF] = 0x22;
  CHECK_EQ(rig.sa1.read(0x00FFEE).value_or(0), 0x11);
  rig.sa1.sa1_write(0x00220E, 0x34);
  rig.sa1.sa1_write(0x00220F, 0x12);
  rig.sa1.sa1_write(0x002209, 0x40);
  CHECK_EQ(rig.sa1.read(0x00FFEE).value_or(0), 0x34);
  CHECK_EQ(rig.sa1.read(0x00FFEF).value_or(0), 0x12);
  // Super MMC: CXB-FXB pick the 1 MB ROM bank each 1 MB of $C0-$FF shows.
  Sa1Rig big;
  big.rom.assign(0x400000, 0);
  SA1 chip(big.rom, big.bwram);
  for (size_t mb = 0; mb < 4; mb++) big.rom[mb * 0x100000] = static_cast<Byte>(0x10 + mb);
  CHECK_EQ(chip.read(0xC00000).value_or(0), 0x10);
  CHECK_EQ(chip.read(0xD00000).value_or(0), 0x11);
  chip.write(0x002220, 0x03);                      // CXB = bank 3
  chip.write(0x002223, 0x00);                      // FXB = bank 0
  CHECK_EQ(chip.read(0xC00000).value_or(0), 0x13);
  CHECK_EQ(chip.read(0xF00000).value_or(0), 0x10);
  CHECK_EQ(chip.read(0x008000).value_or(0), 0x10);   // LoROM $00-$1F: the first 1 MB unless CXB bit 7...
  chip.write(0x002220, 0x83);
  CHECK_EQ(chip.read(0x008000).value_or(0), 0x13);   // ...which makes it follow CXB
  CHECK_EQ(chip.read(0xC00000).value_or(0), 0x13);
  return true;
}

// ==== cartridge detection and firmware ===========================================================

struct HeaderSpec {
  bool hirom = false;
  const char* title = "CHIP TEST";
  Byte map = 0x20, chipset = 0x00, rom_size = 0x08, ram_size = 0x00;
  bool extended = false;
  Byte expansion_ram = 0, subtype = 0;
};

auto make_rom(const HeaderSpec& spec, size_t size = 0x80000) -> std::vector<Byte> {
  std::vector<Byte> rom(size, 0xFF);
  const size_t h = spec.hirom ? 0xFFC0 : 0x7FC0;
  std::memset(&rom[h], ' ', 21);
  std::memcpy(&rom[h], spec.title, std::strlen(spec.title));
  rom[h + 0x15] = spec.map;
  rom[h + 0x16] = spec.chipset;
  rom[h + 0x17] = spec.rom_size;
  rom[h + 0x18] = spec.ram_size;
  rom[h + 0x19] = 0x01;
  rom[h + 0x1A] = spec.extended ? 0x33 : 0x00;
  rom[h + 0x1B] = 0x00;
  rom[h + 0x1C] = 0x34; rom[h + 0x1D] = 0x12;      // complement
  rom[h + 0x1E] = 0xCB; rom[h + 0x1F] = 0xED;      // checksum
  rom[h - 3] = spec.expansion_ram;
  rom[h - 1] = spec.subtype;
  rom[h + 0x3C] = 0x00; rom[h + 0x3D] = 0x80;      // reset vector $8000
  return rom;
}

auto test_board_detection() -> bool {
  struct Case { HeaderSpec spec; Chip chip; size_t ram; bool battery; };
  const Case cases[] = {
    {{false, "SUPER MARIO KART", 0x20, 0x05, 0x09, 0x01}, Chip::DSP1B, 2048, true},
    {{false, "PILOTWINGS", 0x20, 0x03, 0x09, 0x00}, Chip::DSP1, 0, false},
    {{false, "DUNGEON MASTER", 0x20, 0x05}, Chip::DSP2, 0, true},
    {{false, "SD\xB6\xDE\xDD\xC0\xDE\xD1GX", 0x20, 0x05}, Chip::DSP3, 0, true},
    {{false, "TOP GEAR 3000", 0x20, 0x03}, Chip::DSP4, 0, false},
    {{true, "SUPER BASES LOADED 2", 0x31, 0x05, 0x0A, 0x01}, Chip::DSP1B, 2048, true},
    {{false, "STAR FOX", 0x20, 0x13, 0x08, 0x00}, Chip::SuperFX, 32768, false},
    {{false, "YOSHI'S ISLAND", 0x20, 0x15, 0x0B, 0x00, true, 0x05}, Chip::SuperFX, 32768, true},
    {{false, "DOOM", 0x20, 0x15, 0x0B, 0x00, true, 0x06}, Chip::SuperFX, 65536, true},
    {{false, "SUPER MARIO RPG", 0x23, 0x35, 0x0B, 0x03}, Chip::SA1, 8192, true},
    {{false, "METAL COMBAT", 0x20, 0x25, 0x0A, 0x03}, Chip::OBC1, 8192, true},
    {{false, "MEGAMAN X2", 0x20, 0xF3, 0x0A, 0x00, true, 0, 0x10}, Chip::Cx4, 0, false},
    {{false, "EXHAUST HEAT2", 0x20, 0xF6, 0x0A, 0x00, true, 0, 0x01}, Chip::ST010, 0, true},
    {{false, "2DAN MORITA SHOUGI", 0x20, 0xF6, 0x09, 0x00, true, 0, 0x01}, Chip::ST011, 0, true},
    {{false, "STREET FIGHTER ALPHA2", 0x22, 0x43, 0x0C, 0x00}, Chip::SDD1, 0, false},
    {{true, "FEOEZ", 0x3A, 0xF9, 0x0D, 0x03, true, 0, 0x00}, Chip::SPC7110, 8192, true},
    {{false, "STAR OCEAN", 0x32, 0x45, 0x0C, 0x03}, Chip::SDD1, 8192, true},
    {{true, "MOMOTETSU HAPPY", 0x3A, 0xF5, 0x0B, 0x03, true, 0, 0x00}, Chip::SPC7110, 8192, true},
    {{false, "PLAIN GAME", 0x20, 0x02, 0x08, 0x03}, Chip::None, 0, true},  // plain boards: Cartridge sizes SRAM
  };
  for (const Case& c : cases) {
    const auto rom = make_rom(c.spec, 0x100000);
    const BoardInfo info = detect_board(rom, c.spec.hirom ? 0xFFC0 : 0x7FC0, c.spec.hirom);
    if (info.chip != c.chip || info.ram_bytes != c.ram || info.battery != c.battery) {
      std::printf("\n    %s: detected %s, %zu bytes RAM, battery %d", c.spec.title, chip_name(info.chip), info.ram_bytes,
                  info.battery);
    }
    CHECK(info.chip == c.chip);
    CHECK_EQ(info.ram_bytes, c.ram);
    CHECK_EQ(info.battery, c.battery);
  }
  // Layout: SA-1 ($23) and S-DD1 ($22) are LoROM boards, so the LoROM header wins.
  const fs::path dir = scratch_dir();
  for (const Byte map : {Byte{0x23}, Byte{0x22}}) {
    auto rom = make_rom({false, "LOROM CHIP", map, map == 0x23 ? Byte{0x35} : Byte{0x43}, 0x09, 0x03}, 0x100000);
    write_file(dir / "layout.sfc", rom);
    memory::Cartridge cart;
    cart.set_firmware_dir((dir / "nofw").string());
    CHECK(cart.load((dir / "layout.sfc").string()));
    CHECK(cart.map_mode() == memory::MapMode::LoROM);
  }
  return true;
}

auto test_firmware_loading() -> bool {
  utils::Logger::set_level(utils::LogLevel::Error);
  const fs::path dir = scratch_dir();
  const fs::path fw = dir / "firmware";
  fs::remove_all(fw);
  fs::create_directories(fw);
  write_file(dir / "kart.sfc", make_rom({false, "SUPER MARIO KART", 0x20, 0x05, 0x09, 0x01}));
  write_file(dir / "pilot.sfc", make_rom({false, "PILOTWINGS", 0x20, 0x03, 0x09, 0x00}));
  write_file(dir / "exhaust.sfc", make_rom({false, "EXHAUST HEAT2", 0x20, 0xF6, 0x0A, 0x00, true, 0, 0x01}));
  write_file(dir / "cx4.sfc", make_rom({false, "MEGAMAN X2", 0x20, 0xF3, 0x0A, 0x00, true, 0, 0x10}));

  {  // Missing: the error names the file and its size.
    memory::Cartridge cart;
    cart.set_firmware_dir(fw.string());
    CHECK(!cart.load((dir / "kart.sfc").string()));
    CHECK(cart.load_error().find("dsp1b.rom (8192 bytes)") != std::string::npos);
    CHECK(cart.load_error().find(fw.string()) != std::string::npos);
  }
  {  // Wrong size.
    write_file(fw / "dsp1b.rom", std::vector<Byte>(4096, 0));
    memory::Cartridge cart;
    cart.set_firmware_dir(fw.string());
    CHECK(!cart.load((dir / "kart.sfc").string()));
    CHECK(cart.load_error().find("4096") != std::string::npos);
  }
  {  // Present (a dummy image: loads, with a hash warning in the log).
    write_file(fw / "dsp1b.rom", std::vector<Byte>(8192, 0));
    memory::Cartridge cart;
    cart.set_firmware_dir(fw.string());
    CHECK(cart.load((dir / "kart.sfc").string()));
    CHECK(cart.coprocessor() && cart.coprocessor()->chip() == Chip::DSP1B);
    CHECK_EQ(cart.read(0x30C000).value_or(0xEE), 0x00);   // SR
    CHECK_EQ(cart.read(0x008000).value_or(0xEE), 0xFF);   // ROM still there
  }
  {  // Pilotwings wants DSP-1 but falls back to DSP-1B.
    memory::Cartridge cart;
    cart.set_firmware_dir(fw.string());
    CHECK(cart.load((dir / "pilot.sfc").string()));
    CHECK(cart.coprocessor() && cart.coprocessor()->chip() == Chip::DSP1B);
  }
  {  // Split program/data images.
    fs::remove(fw / "dsp1b.rom");
    write_file(fw / "dsp1b.program.rom", std::vector<Byte>(6144, 0));
    write_file(fw / "dsp1b.data.rom", std::vector<Byte>(2048, 0));
    memory::Cartridge cart;
    cart.set_firmware_dir(fw.string());
    CHECK(cart.load((dir / "kart.sfc").string()));
  }
  {  // ST010: 53248 bytes, data RAM is the battery RAM and is mapped at $68:0000.
    memory::Cartridge missing;
    missing.set_firmware_dir(fw.string());
    CHECK(!missing.load((dir / "exhaust.sfc").string()));
    CHECK(missing.load_error().find("st010.rom (53248 bytes)") != std::string::npos);
    write_file(fw / "st010.rom", std::vector<Byte>(53248, 0));
    memory::Cartridge cart;
    cart.set_firmware_dir(fw.string());
    CHECK(cart.load((dir / "exhaust.sfc").string()));
    CHECK_EQ(cart.sram_data().size(), 4096u);
    cart.clear_sram_dirty();
    cart.write(0x680010, 0x42);
    CHECK_EQ(cart.read(0x680010).value_or(0), 0x42);
    CHECK_EQ(cart.sram_data()[0x10], 0x42);
    CHECK(cart.sram_dirty());
  }
  {  // Cx4: a missing data ROM uses the generated tables; a present file is used as-is.
    std::filesystem::remove(fw / "cx4.data.rom");
    std::filesystem::remove(fw / "cx4.rom");
    memory::Cartridge missing;
    missing.set_firmware_dir(fw.string());
    CHECK(missing.load((dir / "cx4.sfc").string()));
    CHECK(missing.coprocessor() && missing.coprocessor()->chip() == Chip::Cx4);
    missing.write(0x007F80, 0x12);  // GPR 0 through the cartridge
    CHECK_EQ(missing.read(0x007F80).value_or(0), 0x12);
    write_file(fw / "cx4.rom", std::vector<Byte>(3000, 0));
    memory::Cartridge wrong;
    wrong.set_firmware_dir(fw.string());
    CHECK(!wrong.load((dir / "cx4.sfc").string()));
    CHECK(wrong.load_error().find("expected 3072") != std::string::npos);
    write_file(fw / "cx4.rom", std::vector<Byte>(3072, 0));
    memory::Cartridge cart;
    cart.set_firmware_dir(fw.string());
    CHECK(cart.load((dir / "cx4.sfc").string()));
    CHECK(cart.coprocessor() && cart.coprocessor()->chip() == Chip::Cx4);
    cart.write(0x007F80, 0x12);
    CHECK_EQ(cart.read(0x007F80).value_or(0), 0x12);
    std::filesystem::remove(fw / "cx4.rom");
  }
  {  // Still-unsupported chips (ST018 here) load as plain ROM with a warning.
    write_file(dir / "st018.sfc", make_rom({false, "2DAN MORITA SHOUGI2", 0x30, 0xF5, 0x0A, 0x00, true, 0, 0x02}));
    memory::Cartridge cart;
    cart.set_firmware_dir(fw.string());
    CHECK(cart.load((dir / "st018.sfc").string()));
    CHECK(!cart.coprocessor());
    CHECK(cart.load_warning().find("ST018") != std::string::npos);
  }
  CHECK(sha256_hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex({'a', 'b', 'c'}) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  return true;
}

// ==== Cx4 (HG51B169) ===============================================================================

auto test_cx4_generated_data_rom() -> bool {
  const auto rom = generate_cx4_data_rom();
  CHECK_EQ(rom.size(), kCx4DataRomSize);
  const std::vector<Byte> bytes(rom.begin(), rom.end());
  CHECK(sha256_hex(bytes) == kCx4DataRomSha256);
  // A couple of documented specials: inverse[0] / cos[0] are 0xFFFFFF, tan[64] is 0x10000.
  auto word = [&](int n) -> uint32_t {
    return static_cast<uint32_t>(rom[static_cast<size_t>(n) * 3] |
                                 (rom[static_cast<size_t>(n) * 3 + 1] << 8) |
                                 (rom[static_cast<size_t>(n) * 3 + 2] << 16));
  };
  CHECK_EQ(word(0), 0xFFFFFFu);          // inverse[0]
  CHECK_EQ(word(256 + 1), 0x100000u);    // sqrt[1] = 2^20
  CHECK_EQ(word(256 + 256 + 128 + 128 + 64), 0x10000u);  // tan[64]
  CHECK_EQ(word(256 + 256 + 128 + 128 + 128), 0xFFFFFFu);  // cos[0]
  return true;
}


// A LoROM image with an HG51B program at $00:8000 (ROM offset 0), data at offset $1F0.
struct Cx4Rig {
  std::vector<Byte> rom = std::vector<Byte>(0x10000, 0);
  std::vector<Byte> ram = std::vector<Byte>(0x2000, 0);
  std::vector<Byte> data_rom = std::vector<Byte>(3072, 0);
  std::unique_ptr<Cx4> chip;

  explicit Cx4Rig(const std::vector<uint16_t>& program) {
    for (size_t i = 0; i < program.size(); i++) {
      rom[i * 2] = static_cast<Byte>(program[i]);
      rom[i * 2 + 1] = static_cast<Byte>(program[i] >> 8);
    }
    for (int n = 0; n < 1024; n++) {  // data ROM word n = n * $010203
      const uint32_t w = (static_cast<uint32_t>(n) * 0x010203u) & 0xFFFFFF;
      data_rom[static_cast<size_t>(n) * 3] = static_cast<Byte>(w);
      data_rom[static_cast<size_t>(n) * 3 + 1] = static_cast<Byte>(w >> 8);
      data_rom[static_cast<size_t>(n) * 3 + 2] = static_cast<Byte>(w >> 16);
    }
    for (int i = 0; i < 16; i++) rom[0x1F0 + static_cast<size_t>(i)] = static_cast<Byte>(0x50 + i);
    chip = std::make_unique<Cx4>(rom, ram, std::span<const Byte>(data_rom.data(), data_rom.size()));
  }
  auto set_gpr(int n, uint32_t value) -> void {
    for (int b = 0; b < 3; b++) chip->write(0x007F80 + static_cast<Address>(n * 3 + b), static_cast<Byte>(value >> (b * 8)));
  }
  auto read_gpr(int n) -> uint32_t {
    uint32_t v = 0;
    for (int b = 0; b < 3; b++) v |= static_cast<uint32_t>(chip->read(0x007F80 + static_cast<Address>(n * 3 + b)).value_or(0)) << (b * 8);
    return v;
  }
  auto start(uint8_t pc = 0) -> void {
    chip->write(0x007F49, 0x00); chip->write(0x007F4A, 0x80); chip->write(0x007F4B, 0x00);  // cache base $008000
    chip->write(0x007F4D, 0x00); chip->write(0x007F4E, 0x00);                               // PB 0
    chip->write(0x007F4F, pc);                                                              // go
  }
  auto run(int master_clocks = 200000) -> void {
    for (int i = 0; i < master_clocks / 100 && (chip->read(0x007F5F).value_or(0) & 0x40); i++) chip->step(100);
  }
};

// Encoders.
constexpr uint16_t kLdAImm = 0x6400, kLdAReg = 0x6000, kLdMdrImm = 0x6500, kLdMarReg = 0x6200, kStA = 0xE000;
constexpr uint16_t kAddImm = 0x8400, kSubImm = 0x9400, kSubrImm = 0x8C00, kCmpImm = 0x5400, kMulImm = 0x9C00, kMulReg = 0x9800;
constexpr uint16_t kShlImm = 0xDC00, kShrImm = 0xC400, kAsrImm = 0xCC00, kRorImm = 0xD400, kSxb = 0x5900, kSxw = 0x5A00;
constexpr uint16_t kXnorImm = 0xA400, kXorImm = 0xAC00, kAndImm = 0xB400, kOrImm = 0xBC00;
constexpr uint16_t kJmp = 0x0800, kJz = 0x0C00, kJsr = 0x2800, kRts = 0x3C00, kSkipZ = 0x2600, kWait = 0x1C00;
constexpr uint16_t kRdRomImm = 0x7400, kWrRamImm = 0xEC00, kRdRamImm = 0x6C00, kSwap = 0xF000, kClear = 0xF800, kHalt = 0xFC00;
constexpr uint16_t kRegMulHi = 0x01, kRegMulLo = 0x02, kRegMdr = 0x03, kRegRom = 0x08, kRegRam = 0x0C, kRegBusRom = 0x2E, kRegBusRam = 0x2F;
constexpr auto kGpr = [](int n) { return static_cast<uint16_t>(0x60 + n); };

auto test_cx4_alu() -> bool {
  Cx4Rig rig({
    kLdAImm | 0xFF, kShlImm | 16, kStA | kGpr(0),                  // $FF0000
    kLdAImm | 0x01, kSubImm | 0x02, kStA | kGpr(1),                // $FFFFFF, borrow
    kLdAImm | 0x80, kSxb, kStA | kGpr(2),                          // $FFFF80
    kLdAImm | 0x40, kAddImm | 0x300 | 0xC0, kStA | kGpr(3),        // ($40 << 16) + $C0
    kLdAImm | 0xF0, kRorImm | 8, kStA | kGpr(4),                   // $F00000
    kLdAImm | 0x80, kShlImm | 16, kAsrImm | 4, kStA | kGpr(5),     // $F80000
    kLdAImm | 0x0F, kXnorImm | 0x0A, kStA | kGpr(6),               // ~$0F ^ $0A = $FFFFFA
    kLdAImm | 0x0F, kXorImm | 0x100 | 0x01, kAndImm | 0x1E, kOrImm | 0x40, kStA | kGpr(7),  // ((($0F<<1)^1)&$1E)|$40
    kLdAImm | 0x10, kSubrImm | 0x30, kStA | kGpr(8),               // $30 - $10
    kLdAImm | 0x80, kShlImm | 8, kSxw, kShrImm | 4, kStA | kGpr(9), // $FF8000 >> 4
    kLdAImm | 0x05, kSwap | 10,                                    // A <-> r10
    kHalt});
  rig.set_gpr(10, 0x123456);
  rig.start();
  rig.run();
  CHECK(!rig.chip->running());
  CHECK_EQ(rig.read_gpr(0), 0xFF0000u);
  CHECK_EQ(rig.read_gpr(1), 0xFFFFFFu);
  CHECK_EQ(rig.read_gpr(2), 0xFFFF80u);
  CHECK_EQ(rig.read_gpr(3), 0x4000C0u);
  CHECK_EQ(rig.read_gpr(4), 0xF00000u);
  CHECK_EQ(rig.read_gpr(5), 0xF80000u);
  CHECK_EQ(rig.read_gpr(6), 0xFFFFFAu);
  CHECK_EQ(rig.read_gpr(7), 0x5Eu);
  CHECK_EQ(rig.read_gpr(8), 0x20u);
  CHECK_EQ(rig.read_gpr(9), 0x0FF800u);
  CHECK_EQ(rig.read_gpr(10), 0x05u);
  CHECK_EQ(rig.chip->accumulator(), 0x123456u);
  // Flags after a compare: 5 - 5 -> Z and C (no borrow); 4 - 5 -> N, no C. (V on subtract uses
  // the same sign rule as ADD; the games depend only on N/Z/C after compares.)
  Cx4Rig flags({kLdAImm | 5, kCmpImm | 5, kHalt});
  flags.start(); flags.run();
  CHECK_EQ(flags.chip->flags(), 0x6u);
  Cx4Rig flags2({kLdAImm | 4, kCmpImm | 5, kHalt});
  flags2.start(); flags2.run();
  CHECK_EQ(flags2.chip->flags() & 0xEu, 0x8u);
  // Signed overflow: $7FFFFF + 1.
  Cx4Rig overflow({kLdAImm | 0x7F, kOrImm | 0x200 | 0xFF, kOrImm | 0x200 | 0xFF, kAddImm | 1, kHalt});
  overflow.start(); overflow.run();
  CHECK_EQ(overflow.chip->accumulator(), 0x800000u);
  CHECK_EQ(overflow.chip->flags() & 0x9u, 0x9u);  // N and V
  return true;
}

auto test_cx4_multiply_rom_ram() -> bool {
  Cx4Rig rig({
    kLdAImm | 0xFF, kSxb, kMulImm | 3,                            // -1 * 3 = -3 (48-bit)
    kLdAReg | kRegMulLo, kStA | kGpr(0), kLdAReg | kRegMulHi, kStA | kGpr(1),
    kLdAReg | kGpr(4), kMulReg | kGpr(5),                         // $123456 * -2
    kLdAReg | kRegMulLo, kStA | kGpr(2), kLdAReg | kRegMulHi, kStA | kGpr(3),
    kRdRomImm | 0x105, kLdAReg | kRegRom, kStA | kGpr(6),         // data ROM word $105
    kLdAImm | 0xAB, kStA | kRegRam, kWrRamImm | 0x0100 | 0x10,    // byte 1 of $AB -> 0? (byte 1 = $00)
    kWrRamImm | 0x11,                                             // byte 0 ($AB) -> data RAM $011
    kLdAImm | 0x00, kStA | kRegRam, kRdRamImm | 0x0200 | 0x11,    // data RAM $011 -> RAM buffer byte 2
    kLdAReg | kRegRam, kStA | kGpr(7),
    kHalt});
  rig.set_gpr(4, 0x123456);
  rig.set_gpr(5, 0xFFFFFE);
  rig.chip->write(0x006010, 0x99);
  rig.start();
  rig.run();
  CHECK_EQ(rig.read_gpr(0), 0xFFFFFDu);
  CHECK_EQ(rig.read_gpr(1), 0xFFFFFFu);
  CHECK_EQ(rig.read_gpr(2), static_cast<uint32_t>((-2LL * 0x123456) & 0xFFFFFF));
  CHECK_EQ(rig.read_gpr(3), static_cast<uint32_t>(((-2LL * 0x123456) >> 24) & 0xFFFFFF));
  CHECK_EQ(rig.read_gpr(6), (0x105u * 0x010203u) & 0xFFFFFF);
  CHECK_EQ(rig.chip->read(0x006010).value_or(0), 0x00);  // byte 1 of $0000AB
  CHECK_EQ(rig.chip->read(0x006011).value_or(0), 0xAB);
  CHECK_EQ(rig.chip->read(0x007011).value_or(0), 0xAB);  // mirrored at $7000
  CHECK_EQ(rig.read_gpr(7), 0xAB0000u);
  return true;
}

auto test_cx4_flow_bus() -> bool {
  // 0: JSR 6 / 1: ST r0 / 2: LD A,0 / 3: CMP 0 / 4: SKIPZ (skip if Z) / 5: (skipped) JMP 20
  // 6..: LD A,$55 / RTS  -> but 4,5 run after return: lay out explicitly.
  std::vector<uint16_t> p(40, kHalt);
  p[0] = kJsr | 10;
  p[1] = kStA | kGpr(0);                     // $55 from the subroutine
  p[2] = kLdAImm | 0; p[3] = kCmpImm | 0;
  p[4] = kSkipZ | 1; p[5] = kLdAImm | 0x11;  // skipped
  p[6] = kStA | kGpr(1);                     // 0
  p[7] = kJz | 14;                           // taken (Z still set)
  p[8] = kLdAImm | 0x22; p[9] = kHalt;
  p[10] = kLdAImm | 0x55; p[11] = kRts;
  // 14: bus read of ROM $0081F3 via MAR, then a bus write of $77 to RAM $700010.
  p[14] = kLdMarReg | kGpr(12); p[15] = kLdAReg | kRegBusRom; p[16] = kWait; p[17] = kLdAReg | kRegMdr; p[18] = kStA | kGpr(2);
  p[19] = kLdMarReg | kGpr(13); p[20] = kLdMdrImm | 0x77; p[21] = kStA | kRegBusRam; p[22] = kWait;
  p[23] = kClear; p[24] = kHalt;
  Cx4Rig rig(p);
  rig.set_gpr(12, 0x0081F3);
  rig.set_gpr(13, 0x700010);
  rig.chip->write(0x007F50, 0x00);  // no wait states
  rig.start();
  rig.run();
  CHECK_EQ(rig.read_gpr(0), 0x55u);
  CHECK_EQ(rig.read_gpr(1), 0x00u);
  CHECK_EQ(rig.read_gpr(2), 0x53u);
  CHECK_EQ(rig.ram[0x10], 0x77);
  CHECK_EQ(rig.chip->accumulator(), 0u);
  CHECK(!rig.chip->running());
  // IRQ at halt; $7F51 bit 0 acknowledges and masks it.
  CHECK(rig.chip->irq());
  CHECK((rig.chip->read(0x007F5F).value_or(0) & 0x02) != 0);
  rig.chip->write(0x007F51, 0x01);
  CHECK(!rig.chip->irq());
  return true;
}

auto test_cx4_cache_dma_timing() -> bool {
  Cx4Rig rig({kHalt});
  // Cache preload: 512 bytes at (1 + ROM wait) clocks each.
  rig.chip->write(0x007F50, 0x30);
  rig.chip->write(0x007F49, 0x00); rig.chip->write(0x007F4A, 0x80); rig.chip->write(0x007F4B, 0x00);
  rig.chip->write(0x007F4D, 0x00); rig.chip->write(0x007F4E, 0x00);
  rig.chip->write(0x007F48, 0x01);  // preload page 1
  CHECK(rig.chip->busy());
  CHECK_EQ(rig.chip->read(0x00FFE0).value_or(0), 0x00);  // vectors visible while busy
  rig.chip->write(0x007F60, 0xAA);
  CHECK_EQ(rig.chip->read(0x00FFE0).value_or(0), 0xAA);
  CHECK_EQ(rig.chip->run_one(), 512u * 4u);
  CHECK(!rig.chip->busy());
  CHECK(!rig.chip->read(0x00FFE0));  // back to ROM
  rig.chip->write(0x007F50, 0x00);
  rig.chip->write(0x007F48, 0x00);
  CHECK_EQ(rig.chip->run_one(), 512u);
  // DMA ROM $0081F0 -> RAM $700020, 4 bytes, ROM wait 1 and RAM wait 2: 4 x (2 + 3) clocks.
  rig.chip->write(0x007F50, 0x12);
  const Byte regs[] = {0xF0, 0x81, 0x00, 0x04, 0x00, 0x20, 0x00, 0x70};
  for (int i = 0; i < 8; i++) rig.chip->write(0x007F40 + static_cast<Address>(i), regs[i]);
  CHECK(rig.chip->busy());
  CHECK_EQ(rig.chip->run_one(), 20u);
  for (int i = 0; i < 4; i++) CHECK_EQ(rig.ram[0x20 + static_cast<size_t>(i)], 0x50 + i);
  // DMA into data RAM ($006100) is allowed; RAM -> RAM locks the chip until $7F53.
  const Byte to_dram[] = {0xF0, 0x81, 0x00, 0x02, 0x00, 0x00, 0x61, 0x00};
  for (int i = 0; i < 8; i++) rig.chip->write(0x007F40 + static_cast<Address>(i), to_dram[i]);
  rig.chip->run_one();
  CHECK_EQ(rig.chip->read(0x006101).value_or(0), 0x51);
  const Byte ram_ram[] = {0x00, 0x00, 0x70, 0x02, 0x00, 0x00, 0x01, 0x70};
  for (int i = 0; i < 8; i++) rig.chip->write(0x007F40 + static_cast<Address>(i), ram_ram[i]);
  rig.chip->run_one();
  CHECK_EQ(rig.chip->run_one(), 1u);  // locked
  // Timing: the S-CPU's master clocks become 20 MHz HG51B clocks exactly.
  Cx4Rig idle({kHalt});
  idle.chip->step(21'477'273);  // ~1 s of master clock (236.25 MHz / 11)
  CHECK(idle.chip->clocks_run() >= 19'999'990 && idle.chip->clocks_run() <= 20'000'010);
  return true;
}

auto test_cx4_state() -> bool {
  std::vector<uint16_t> loop(32, kHalt);
  loop[0] = kAddImm | 3; loop[1] = kMulReg | kGpr(1); loop[2] = kStA | kGpr(1); loop[3] = kXorImm | 0x15;
  loop[4] = kCmpImm | 0x00; loop[5] = kJmp | 0;  // runs forever
  Cx4Rig rig(loop);
  rig.set_gpr(1, 7);
  rig.start();
  for (int i = 0; i < 37; i++) rig.chip->step(13);
  Archive saver = Archive::saver();
  rig.chip->serialize(saver);
  for (int i = 0; i < 50; i++) rig.chip->step(17);
  const uint32_t a = rig.chip->accumulator(), r1 = rig.read_gpr(1);
  const uint64_t mul = rig.chip->multiplier();
  Archive loader = Archive::loader(saver.data());
  rig.chip->serialize(loader);
  CHECK(loader.ok());
  for (int i = 0; i < 50; i++) rig.chip->step(17);
  CHECK_EQ(rig.chip->accumulator(), a);
  CHECK_EQ(rig.read_gpr(1), r1);
  CHECK_EQ(rig.chip->multiplier(), mul);
  CHECK(rig.chip->running());
  rig.chip->write(0x007F53, 0x00);  // stop
  return true;
}

auto test_obc1() -> bool {
  std::vector<Byte> sram(8192, 0);
  OBC1 chip(sram);
  chip.write(0x007FF5, 0x00);                      // table at $1C00
  chip.write(0x007FF6, 0x05);                      // sprite 5
  for (Address i = 0; i < 4; i++) chip.write(0x007FF0 + i, static_cast<Byte>(0x10 + i));
  CHECK_EQ(sram[0x1C00 + 5 * 4 + 2], 0x12);
  chip.write(0x007FF4, 0x03);                      // high bits: sprite 5 -> byte $201, shift 2
  CHECK_EQ(sram[0x1C00 + 0x200 + 1], 0x0C);
  CHECK_EQ(chip.read(0x007FF1).value_or(0), 0x11);
  CHECK_EQ(chip.read(0x006000).value_or(0xEE), 0x00);
  CHECK(!chip.read(0x408000).has_value());
  return true;
}

auto test_chip_battery() -> bool {
  const fs::path dir = scratch_dir();
  // SA-1 with battery: BW-RAM writes from either CPU mark the battery RAM dirty.
  auto rom = make_rom({false, "SA1 BATTERY", 0x23, 0x35, 0x09, 0x03}, 0x100000);
  write_file(dir / "sa1bat.sfc", rom);
  memory::Cartridge cart;
  CHECK(cart.load((dir / "sa1bat.sfc").string()));
  CHECK_EQ(cart.sram_data().size(), 8192u);
  cart.clear_sram_dirty();
  CHECK(!cart.sram_dirty());
  cart.write(0x002226, 0x80);                     // SWEN
  cart.write(0x400123, 0x77);
  CHECK(cart.sram_dirty());
  cart.clear_sram_dirty();
  auto* sa1 = static_cast<SA1*>(cart.coprocessor());
  sa1->sa1_write(0x002227, 0x80);
  sa1->sa1_write(0x400124, 0x78);                 // the SA-1's own write
  CHECK(cart.sram_dirty());
  // Super FX without battery (Star Fox): RAM isn't saved.
  write_file(dir / "fx.sfc", make_rom({false, "STAR FOX", 0x20, 0x13, 0x08, 0x00}));
  memory::Cartridge fx;
  CHECK(fx.load((dir / "fx.sfc").string()));
  CHECK(fx.coprocessor() && fx.coprocessor()->chip() == Chip::SuperFX);
  CHECK(fx.sram_data().empty());
  return true;
}

// ==== machine-level: hand-written SA-1 and Super FX carts =========================================

auto put(std::vector<Byte>& rom, size_t offset, const std::vector<Byte>& bytes) -> void {
  std::copy(bytes.begin(), bytes.end(), rom.begin() + static_cast<std::ptrdiff_t>(offset));
}

auto make_sa1_cart() -> std::vector<Byte> {
  auto rom = make_rom({false, "CHIPTEST SA1", 0x23, 0x35, 0x07, 0x03}, 0x20000);
  std::fill(rom.begin(), rom.begin() + 0x7F00, Byte{0xEA});
  put(rom, 0x0000, {0x78, 0x18, 0xFB, 0xE2, 0x30,     // sei clc xce sep #$30
                    0xA9, 0x00, 0x8D, 0x03, 0x22,     // CRV = $8100
                    0xA9, 0x81, 0x8D, 0x04, 0x22,
                    0xA9, 0xFF, 0x8D, 0x29, 0x22,     // SIWP
                    0x9C, 0x00, 0x22,                 // CCNT = 0: run the SA-1
                    0xAD, 0x02, 0x30,                 // loop: lda $3002 (I-RAM)
                    0x8D, 0x10, 0x00,                 // sta $0010 (WRAM)
                    0xEE, 0x11, 0x00,                 // inc $0011
                    0x80, 0xF5});
  put(rom, 0x0100, {0x18, 0xFB, 0xE2, 0x30,           // SA-1: clc xce sep #$30
                    0xA9, 0xFF, 0x8D, 0x2A, 0x22,     // CIWP
                    0x9C, 0x50, 0x22,                 // MCNT: multiply
                    0xE6, 0x00,                       // loop: inc $00 (I-RAM)
                    0xA5, 0x00, 0x8D, 0x51, 0x22, 0x9C, 0x52, 0x22,
                    0xA9, 0x07, 0x8D, 0x53, 0x22, 0x9C, 0x54, 0x22,
                    0xAD, 0x06, 0x23, 0x8D, 0x02, 0x30,   // I-RAM[2] = (I-RAM[0] * 7) & $FF
                    0xA9, 0x80, 0x8D, 0x27, 0x22,         // CWEN
                    0xA5, 0x00, 0x8F, 0x00, 0x00, 0x40,   // sta $400000 (BW-RAM)
                    0x80, 0xDB});
  return rom;
}

auto make_gsu_cart() -> std::vector<Byte> {
  auto rom = make_rom({false, "CHIPTEST GSU", 0x20, 0x15, 0x08, 0x00, true, 0x05}, 0x20000);
  std::fill(rom.begin(), rom.begin() + 0x7F00, Byte{0x01});
  const std::vector<Byte> wram_code = {
    0xA9, 0x01, 0x8D, 0x39, 0x30,     // CLSR 21 MHz
    0xA9, 0x80, 0x8D, 0x37, 0x30,     // CFGR: IRQ masked
    0x9C, 0x38, 0x30,                 // SCBR 0
    0xA9, 0x19, 0x8D, 0x3A, 0x30,     // SCMR 4bpp, RON|RAN
    0x9C, 0x34, 0x30,                 // PBR 0
    0xAD, 0x30, 0x30, 0x29, 0x20, 0xD0, 0xF9,   // wait while GO
    0x9C, 0x1E, 0x30, 0xA9, 0x82, 0x8D, 0x1F, 0x30,  // R15 = $8200: go
    0xEE, 0x11, 0x00,                 // inc $0011
    0x80, 0xEC};
  put(rom, 0x0000, {0x78, 0x18, 0xFB, 0xE2, 0x30, 0xA2, 0x00,
                    0xBF, 0x40, 0x80, 0x00,           // lda.l $008040,x
                    0x9F, 0x00, 0x01, 0x7E,           // sta.l $7E0100,x
                    0xE8, 0xE0, static_cast<Byte>(wram_code.size()), 0xD0, 0xF3,
                    0x5C, 0x00, 0x01, 0x7E});         // jml $7E0100 (the S-CPU leaves ROM to the GSU)
  put(rom, 0x0040, wram_code);
  put(rom, 0x0200, {0xF3, 0x10, 0x00,                 // IWT R3,#$0010
                    0x40 | 3, 0xD0, 0x33,             // LDW (R3)  INC R0  STW (R3)
                    0x20, 0x11,                       // MOVE R1,R0
                    0xA2, 0x05, 0x4E, 0x4C,           // IBT R2,#5  COLOR  PLOT
                    0x00, 0x01});
  return rom;
}

struct MachineRun { std::vector<uint8_t> state; uint64_t video = 0; };

auto hash_frame(const System& sys) -> uint64_t {
  auto [w, h] = sys.get_framebuffer_size();
  uint64_t value = 0xCBF29CE484222325ull;
  const uint32_t* fb = sys.get_framebuffer();
  for (int i = 0; i < w * h; i++) value = (value ^ fb[i]) * 0x100000001B3ull;
  return value;
}

// Save, run, load, run: same frames and the same final state, coprocessor included.
auto check_determinism(const std::string& path, int warmup, int frames) -> bool {
  System sys;
  sys.set_firmware_dir((fs::path(path).parent_path() / "firmware").string());
  if (!sys.load_rom(path)) {
    std::printf(" [can't load: %s]", sys.load_error().c_str());
    return false;
  }
  sys.reset();
  for (int i = 0; i < warmup; i++) sys.run_frame();
  const auto start = sys.save_state();
  auto run = [&] {
    MachineRun r;
    for (int i = 0; i < frames; i++) {
      sys.run_frame();
      r.video = r.video * 31 + hash_frame(sys);
    }
    r.state = sys.save_state();
    return r;
  };
  const MachineRun first = run();
  CHECK(sys.load_state(start));
  const MachineRun second = run();
  CHECK(first.video == second.video);
  CHECK(first.state == second.state);
  CHECK(sys.load_state(start));
  CHECK(sys.save_state() == start);
  auto* chip = sys.cartridge().coprocessor();
  std::printf(" [%s, %zu-byte state]", chip ? chip_name(chip->chip()) : "no chip", start.size());
  return true;
}

// ==== MSU-1 ========================================================================================

// A synthetic track: "MSU1", the loop point, then the frames.
auto write_pcm(const fs::path& path, uint32_t loop, const std::vector<std::pair<int16_t, int16_t>>& frames) -> void {
  std::vector<Byte> pcm = {'M', 'S', 'U', '1', static_cast<Byte>(loop), static_cast<Byte>(loop >> 8),
                           static_cast<Byte>(loop >> 16), static_cast<Byte>(loop >> 24)};
  for (const auto& [l, r] : frames) {
    for (const int16_t v : {l, r}) {
      pcm.push_back(static_cast<Byte>(v));
      pcm.push_back(static_cast<Byte>(static_cast<uint16_t>(v) >> 8));
    }
  }
  write_file(path, pcm);
}

auto msu_seek(Msu1& msu, uint32_t offset) -> void {
  for (Word i = 0; i < 4; i++) msu.write(static_cast<Word>(0x2000 + i), static_cast<Byte>(offset >> (8 * i)));
}

auto msu_track(Msu1& msu, uint16_t track) -> void {
  msu.write(0x2004, static_cast<Byte>(track));
  msu.write(0x2005, static_cast<Byte>(track >> 8));
}

// Registers, the data port, track select and loop points, volume, the 44.1 kHz -> 32040 Hz
// resampler and the state round trip, on synthetic files.
auto test_msu1_registers() -> bool {
  const fs::path dir = scratch_dir() / "msu";
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::vector<Byte> data(300);
  for (size_t i = 0; i < data.size(); i++) data[i] = static_cast<Byte>(i * 3 + 1);
  write_file(dir / "game.msu", data);
  std::vector<std::pair<int16_t, int16_t>> six;
  for (int i = 0; i < 6; i++) six.emplace_back(static_cast<int16_t>(1000 * (i + 1)), static_cast<int16_t>(-1000 * (i + 1)));
  write_pcm(dir / "game-1.pcm", 3, six);
  std::vector<std::pair<int16_t, int16_t>> ramp;
  for (int i = 0; i < 4000; i++) ramp.emplace_back(static_cast<int16_t>(i * 8), static_cast<int16_t>(-i * 8));
  write_pcm(dir / "game-2.pcm", 0, ramp);
  write_file(dir / "game-3.pcm", {'R', 'I', 'F', 'F', 0, 0, 0, 0, 1, 2, 3, 4});  // not a track

  CHECK(Msu1::detect((dir / "nothing.sfc").string()) == nullptr);
  auto msu = Msu1::detect((dir / "game.sfc").string());
  CHECK(msu != nullptr);
  if (!msu) return true;
  const char id[] = "S-MSU1";
  for (Word i = 0; i < 6; i++) CHECK_EQ(msu->read(static_cast<Word>(0x2002 + i)), static_cast<Byte>(id[i]));
  CHECK_EQ(msu->read(0x2000), 0x02);  // revision 2, idle

  // Data port: sequential reads from the seek offset, zeros past the end.
  CHECK_EQ(msu->read(0x2001), data[0]);
  CHECK_EQ(msu->read(0x2001), data[1]);
  msu_seek(*msu, 0x100);
  for (size_t i = 0x100; i < 0x103; i++) CHECK_EQ(msu->read(0x2001), data[i]);
  msu_seek(*msu, 298);
  CHECK_EQ(msu->read(0x2001), data[298]);
  CHECK_EQ(msu->read(0x2001), data[299]);
  CHECK_EQ(msu->read(0x2001), 0x00);
  msu_seek(*msu, 5);
  CHECK_EQ(msu->read(0x2001), data[5]);

  // Missing and malformed tracks: the error bit, and play is refused.
  for (const uint16_t bad : {uint16_t{9}, uint16_t{3}}) {
    msu_track(*msu, bad);
    CHECK_EQ(msu->read(0x2000), Msu1::kTrackMissing | Msu1::kRevision);
    msu->write(0x2007, 0x03);
    CHECK_EQ(msu->read(0x2000) & Msu1::kPlaying, 0);
  }

  // Track 1, repeat: at the input rate the output is the track itself (two frames of
  // interpolation latency), looping from frame 3.
  std::vector<int16_t> out(2 * 16);
  msu_track(*msu, 1);
  CHECK_EQ(msu->read(0x2000), Msu1::kRevision);
  msu->write(0x2007, 0x03);
  CHECK_EQ(msu->read(0x2000), Msu1::kPlaying | Msu1::kRepeating | Msu1::kRevision);
  msu->render(out, Msu1::kSampleRate);
  const int expect[] = {0, 0, 1, 2, 3, 4, 5, 6, 4, 5, 6, 4, 5, 6, 4, 5};
  for (size_t i = 0; i < 16; i++) {
    CHECK_EQ(out[2 * i], static_cast<int16_t>(1000 * expect[i]));
    CHECK_EQ(out[2 * i + 1], static_cast<int16_t>(-1000 * expect[i]));
  }
  // Volume: linear, 255 = full.
  msu->write(0x2006, 0x80);
  msu->render(std::span(out).first(2), Msu1::kSampleRate);  // frame 6 of the loop
  CHECK_EQ(out[0], static_cast<int16_t>(6000 * 128 / 255));
  msu->write(0x2006, 0xFF);

  // No repeat: plays to the end once, then stops (silence, playing bit clear, back to the start).
  msu_track(*msu, 1);
  msu->write(0x2007, 0x01);
  msu->render(out, Msu1::kSampleRate);
  CHECK_EQ(out[2 * 7], 6000);
  CHECK_EQ(out[2 * 8], 0);
  CHECK_EQ(out[2 * 15], 0);
  CHECK_EQ(msu->read(0x2000) & Msu1::kPlaying, 0);
  // Stop: silence from the next frame on.
  msu->write(0x2007, 0x01);
  msu->render(std::span(out).first(8), Msu1::kSampleRate);
  msu->write(0x2007, 0x00);
  msu->render(std::span(out).first(8), Msu1::kSampleRate);
  CHECK(out[4] == 0 && out[6] == 0);

  // 44.1 kHz -> 32040 Hz: a ramp stays a ramp, 8 * 44100 / 32040 per output frame.
  msu_track(*msu, 2);
  msu->write(0x2007, 0x01);
  std::vector<int16_t> frame(2 * 534);
  msu->render(frame, 32040);
  bool linear = true;
  for (size_t i = 10; i < 534; i++) {
    const double want = (static_cast<double>(i) * 44100.0 / 32040.0 - 2.0) * 8.0;
    if (std::abs(frame[2 * i] - want) > 1.0 || frame[2 * i + 1] != -frame[2 * i]) linear = false;
  }
  CHECK(linear);

  // State round trip mid-track: a fresh MSU-1 (files reopened) continues identically.
  Archive saver = Archive::saver();
  msu->serialize(saver);
  std::vector<int16_t> a(2 * 600), b(2 * 600);
  msu->render(a, 32040);
  Msu1 other((dir / "game").string());
  Archive loader = Archive::loader(saver.data());
  other.serialize(loader);
  CHECK(loader.ok() && loader.remaining() == 0);
  other.render(b, 32040);
  CHECK(a == b);
  CHECK_EQ(other.read(0x2000), Msu1::kPlaying | Msu1::kRevision);
  CHECK_EQ(other.track(), 2);

  msu->reset();
  CHECK_EQ(msu->read(0x2000), Msu1::kRevision);
  return true;
}

// The whole machine: the program reads the ID through the bus, starts a looping track, and each
// frame carries as many MSU-1 samples as APU samples. No .msu: the registers aren't there.
auto test_machine_msu1() -> bool {
  const fs::path dir = scratch_dir() / "msu_machine";
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto rom = make_rom({false, "MSU TEST"}, 0x20000);
  put(rom, 0x0000, {0x78, 0x18, 0xFB, 0xE2, 0x30, 0xA2, 0x00,  // sei clc xce sep #$30 ldx #0
                    0xBD, 0x02, 0x20, 0x9D, 0x10, 0x00,        // loop: lda $2002,x  sta $0010,x
                    0xE8, 0xE0, 0x06, 0xD0, 0xF5,              // inx cpx #6 bne loop
                    0xA9, 0x01, 0x8D, 0x04, 0x20, 0x9C, 0x05, 0x20,  // track 1
                    0xA9, 0x03, 0x8D, 0x07, 0x20,              // play, repeat
                    0xAD, 0x00, 0x20, 0x8D, 0x20, 0x00,        // status -> $0020
                    0x80, 0xFE});
  write_file(dir / "plain.sfc", rom);
  write_file(dir / "msu.sfc", rom);
  write_file(dir / "msu.msu", {});
  std::vector<std::pair<int16_t, int16_t>> tone;
  for (int i = 0; i < 3000; i++) tone.emplace_back(static_cast<int16_t>((i % 100) * 300 - 15000), static_cast<int16_t>(i * 7));
  write_pcm(dir / "msu-1.pcm", 0, tone);

  {
    System sys;
    CHECK(sys.load_rom((dir / "plain.sfc").string()));
    sys.reset();
    sys.run_frame();
    CHECK(sys.msu1() == nullptr && sys.msu1_samples().empty());
    CHECK(sys.peek_wram(0x10) != 'S');
  }
  System sys;
  CHECK(sys.load_rom((dir / "msu.sfc").string()));
  sys.reset();
  CHECK(sys.msu1() != nullptr);
  for (int i = 0; i < 3; i++) sys.run_frame();
  const char id[] = "S-MSU1";
  for (uint32_t i = 0; i < 6; i++) CHECK_EQ(sys.peek_wram(0x10 + i), static_cast<Byte>(id[i]));
  CHECK_EQ(sys.peek_wram(0x20), Msu1::kPlaying | Msu1::kRepeating | Msu1::kRevision);
  CHECK(!sys.audio_samples().empty() && sys.msu1_samples().size() == sys.audio_samples().size());
  size_t loud = 0;
  for (const int16_t v : sys.msu1_samples()) loud += v != 0;
  CHECK(loud > sys.msu1_samples().size() / 2);
  // Save, run, load, run: the same MSU-1 audio (playback position and resampler in the state).
  const auto state = sys.save_state();
  auto run = [&] {
    std::vector<int16_t> all;
    for (int i = 0; i < 10; i++) {
      sys.run_frame();
      all.insert(all.end(), sys.msu1_samples().begin(), sys.msu1_samples().end());
    }
    return all;
  };
  const auto first = run();
  CHECK(sys.load_state(state));
  CHECK(run() == first);
  return true;
}

// ==== cheats =======================================================================================

// Game Genie and Pro Action Replay decoding against published encodings, the other formats,
// and the cheats file.
auto test_cheat_decoding() -> bool {
  struct Case { const char* code; Address address; Byte value; };
  // Published worked examples: gamehacking.org's SNES GG document (ABCD-EFFF), the SNES Game
  // Genie Handbook section 12 (DF47-E76D, DF47-0915, and Final Fantasy III's 135C-7B85
  // "Terra starts with 110 Vigor" = $ED7CA6:6E).
  const Case genie[] = {
    {"ABCD-EFFF", 0xC4A704, 0xC9},
    {"DF47-E76D", 0xC0233E, 0x01},
    {"DF47-0915", 0x192D35, 0x01},
    {"135C-7B85", 0xED7CA6, 0x6E},
    {"df47-e76d", 0xC0233E, 0x01},  // case doesn't matter
  };
  for (const auto& c : genie) {
    const auto got = decode_game_genie(c.code);
    CHECK(got.has_value());
    if (!got) continue;
    CHECK_EQ(got->address, c.address);
    CHECK_EQ(got->value, c.value);
    CHECK(got->kind == Cheat::Kind::ReadOverride && !got->compare);
  }
  CHECK(encode_game_genie(0xC4A704, 0xC9) == "ABCD-EFFF");
  CHECK(encode_game_genie(0xED7CA6, 0x6E) == "135C-7B85");
  for (uint32_t a = 0; a < 0x1000000; a += 0x10101) {  // round trip across the address space
    const auto back = decode_game_genie(encode_game_genie(a, static_cast<Byte>(a * 7)));
    if (!back || back->address != a || back->value != static_cast<Byte>(a * 7)) {
      CHECK(false);
      break;
    }
  }
  CHECK(!decode_game_genie("ABCDEFFF"));   // no dash
  CHECK(!decode_game_genie("ABCD-EFFG"));  // not a GG digit
  CHECK(!decode_game_genie("ABC-DEFFF"));

  // Pro Action Replay: AAAAAAVV; WRAM (and its $00-$3F/$80-$BF mirror) is a per-frame write.
  const auto par = decode_pro_action_replay("7E0DBF09");
  CHECK(par && par->address == 0x7E0DBF && par->value == 0x09 && par->kind == Cheat::Kind::RamWrite);
  const auto low = decode_pro_action_replay("00012345");
  CHECK(low && low->address == 0x000123 && low->kind == Cheat::Kind::RamWrite);
  const auto rom = decode_pro_action_replay("C0233E01");  // the same as GG DF47-E76D
  CHECK(rom && rom->kind == Cheat::Kind::ReadOverride && *rom == *decode_game_genie("DF47-E76D"));
  CHECK(!decode_pro_action_replay("7E0DBF0"));
  CHECK(!decode_pro_action_replay("7E0DBFXX"));

  // Raw, compare, and joined codes.
  const auto raw = decode_cheat("C4A704:C9");
  CHECK(raw && raw->size() == 1 && (*raw)[0].address == 0xC4A704 && (*raw)[0].value == 0xC9 && !(*raw)[0].compare);
  const auto cmp = decode_cheat("808100:42:99");
  CHECK(cmp && (*cmp)[0].compare == Byte{0x42} && (*cmp)[0].value == 0x99);
  const auto joined = decode_cheat(" DF47-E76D + 7E0DBF09+C4A704:C9 ");
  CHECK(joined && joined->size() == 3 && (*joined)[1].kind == Cheat::Kind::RamWrite);
  CHECK(!decode_cheat("DF47-E76D+"));
  CHECK(!decode_cheat(""));
  CHECK(!decode_cheat("hello"));
  CHECK(!decode_cheat("C4A704:C"));

  // The file: on/off, code, description; comments, blanks and junk skipped; round trip.
  const auto entries = parse_cheat_file("# comment\n\non  DF47-E76D  Max vigor\r\noff 7E0DBF09\nmaybe 7E0DBF09 x\non nonsense y\n");
  CHECK(entries.size() == 2);
  if (entries.size() == 2) {
    CHECK((entries[0] == CheatEntry{true, "DF47-E76D", "Max vigor"}));
    CHECK((entries[1] == CheatEntry{false, "7E0DBF09", ""}));
  }
  CHECK(parse_cheat_file(format_cheat_file(entries)) == entries);
  return true;
}

// The engine on a running machine: a ROM read override (Game Genie, mirrored bank, compare),
// a Pro Action Replay WRAM write every frame, and switching cheats off.
auto test_machine_cheats() -> bool {
  auto rom = make_rom({false, "CHEAT TEST"}, 0x20000);
  put(rom, 0x0000, {0x78, 0x18, 0xFB, 0xE2, 0x30,       // sei clc xce sep #$30
                    0xAD, 0x00, 0x81, 0x8D, 0x10, 0x00, // loop: lda $8100  sta $0010
                    0xAF, 0x01, 0x81, 0x80, 0x8D, 0x11, 0x00,  // lda $808101  sta $0011
                    0xEE, 0x20, 0x00,                   // inc $0020
                    0x80, 0xEE});                       // bra loop
  rom[0x0100] = 0x11;
  rom[0x0101] = 0x22;
  const fs::path path = scratch_dir() / "cheat_test.sfc";
  write_file(path, rom);
  System sys;
  CHECK(sys.load_rom(path.string()));
  sys.reset();
  sys.run_frame();
  CHECK_EQ(sys.peek_wram(0x10), 0x11);
  CHECK_EQ(sys.peek_wram(0x11), 0x22);
  CHECK(!sys.cheats().active());

  // GG for $808100 (the FastROM mirror of the $008100 the program reads), a compare code that
  // matches $008101, one that doesn't, and PAR codes for $7E0030 and $000031.
  const std::vector<CheatEntry> list = {
    {true, encode_game_genie(0x808100, 0x5A), "mirror"},
    {true, "008101:22:77", "compare hit"},
    {true, "008101:99:66", "compare miss"},
    {true, "7E003099+00003142", "PAR WRAM"},
    {false, "008100:00", "disabled"},
  };
  sys.set_cheats(list);
  CHECK(sys.cheats().active());
  sys.run_frame();
  CHECK_EQ(sys.peek_wram(0x10), 0x5A);
  CHECK_EQ(sys.peek_wram(0x11), 0x77);
  CHECK_EQ(sys.peek_wram(0x30), 0x99);
  CHECK_EQ(sys.peek_wram(0x31), 0x42);
  CHECK_EQ(rom[0x0100], 0x11);  // nothing is patched: the bus answers instead
  // Off: the real bytes again (WRAM keeps what was written, like the real PAR).
  sys.set_cheats({});
  sys.run_frame();
  CHECK_EQ(sys.peek_wram(0x10), 0x11);
  CHECK_EQ(sys.peek_wram(0x11), 0x22);
  CHECK_EQ(sys.peek_wram(0x30), 0x99);
  // Cheats aren't machine state: the same bytes with and without them.
  sys.set_cheats(list);
  const auto with = sys.save_state();
  sys.set_cheats({});
  CHECK(sys.save_state() == with);
  return true;
}

auto test_machine_sa1() -> bool {
  const fs::path path = scratch_dir() / "chiptest_sa1.sfc";
  write_file(path, make_sa1_cart());
  System sys;
  CHECK(sys.load_rom(path.string()));
  sys.reset();
  for (int i = 0; i < 5; i++) sys.run_frame();
  auto* sa1 = static_cast<SA1*>(sys.cartridge().coprocessor());
  CHECK(sa1 && sa1->running());
  const Byte count = sa1->iram()[0];
  // The frame can end anywhere in the SA-1's loop, and the S-CPU's copy trails a little.
  auto lagging = [&](Byte value, int max_lag) {
    for (int k = 0; k <= max_lag; k++) {
      if (value == static_cast<Byte>((count - k) * 7)) return true;
    }
    return false;
  };
  CHECK(count > 10);
  CHECK(lagging(sa1->iram()[2], 1));
  CHECK(lagging(sys.peek_wram(0x10), 3));
  CHECK(sys.sram_dirty());                        // the SA-1 wrote BW-RAM
  return check_determinism(path.string(), 20, 30);
}

auto test_machine_gsu() -> bool {
  const fs::path path = scratch_dir() / "chiptest_gsu.sfc";
  write_file(path, make_gsu_cart());
  System sys;
  CHECK(sys.load_rom(path.string()));
  sys.reset();
  sys.run_frame();
  auto* gsu = static_cast<GSU*>(sys.cartridge().coprocessor());
  CHECK(gsu != nullptr);
  const auto ram = sys.sram();                    // battery-backed GSU RAM
  CHECK_EQ(ram.size(), 32768u);
  auto runs = [&] { return static_cast<unsigned>(ram[0x10] | ram[0x11] << 8); };
  const unsigned runs_before = runs();
  const Byte starts_before = sys.peek_wram(0x11);  // WRAM powers up with a fill pattern
  for (int i = 0; i < 4; i++) sys.run_frame();
  const unsigned gsu_runs = runs() - runs_before;
  const auto cpu_starts = static_cast<Byte>(sys.peek_wram(0x11) - starts_before);
  CHECK(gsu_runs > 1000);                         // the S-CPU restarted it every few hundred clocks
  const int lag = static_cast<int>(static_cast<Byte>(gsu_runs)) - static_cast<int>(cpu_starts);
  if (std::abs(lag) > 1) std::printf("\n    GSU runs %u, S-CPU starts %u (mod 256)", gsu_runs, cpu_starts);
  CHECK(std::abs(lag) <= 1);
  CHECK(ram[0x0A] != 0 || ram[0x0B] != 0 || ram[0x1A] != 0 || ram[0x1B] != 0);  // PLOT drew row 5 of tile 0
  return check_determinism(path.string(), 20, 30);
}

// ==== S-DD1 ========================================================================================

struct Rng {  // xorshift32: reproducible test data
  uint32_t s;
  auto next() -> uint32_t { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  auto below(uint32_t n) -> uint32_t { return next() % n; }
};

// Reference S-DD1 encoder. It feeds the known output bits through the same context model and
// probability estimator as the decoder (fullsnes's algorithm) and emits, for each of the eight
// run counters, the codewords in the order the decoder will request them. A codeword's slot is
// reserved when its counter needs reloading and filled in once the run's length is known: "0"
// for 2^size MPS symbols, or "1" plus `size` index bits for a shorter run ending in an LPS.
auto sdd1_compress(const std::vector<Byte>& data, Byte plane_mode, Byte context_mode) -> std::vector<Byte> {
  using D = SDD1Decompressor;
  const int planes = plane_mode == 0x00 ? 2 : plane_mode == 0x40 ? 8 : plane_mode == 0x80 ? 4 : 0;
  // (plane, bit) in the order the decoder produces them.
  std::vector<std::pair<int, int>> order;
  if (planes == 0) {
    for (Byte b : data) for (int p = 0; p < 8; p++) order.push_back({p, (b >> p) & 1});
  } else {
    int plane = 0, row = 0;
    for (size_t i = 0; i < data.size(); i += 2) {
      const Byte lo = data[i], hi = i + 1 < data.size() ? data[i + 1] : 0;
      for (int bit = 7; bit >= 0; bit--) {
        order.push_back({plane, (lo >> bit) & 1});
        order.push_back({plane + 1, (hi >> bit) & 1});
      }
      if (++row == 8) { row = 0; plane = (plane + 2) & (planes - 1); }
    }
  }
  static constexpr uint16_t kHigh[4] = {0x01C0, 0x0180, 0x00C0, 0x0180};
  static constexpr uint16_t kLow[4] = {0x0001, 0x0001, 0x0001, 0x0003};
  const uint16_t high = kHigh[(context_mode >> 4) & 3], low = kLow[(context_mode >> 4) & 3];
  struct Codeword { int size; bool lps = false; int mps_symbols = 0; };
  std::vector<Codeword> words;
  std::array<int, 8> active{};  // index into `words` of each counter's open run, -1 = none
  active.fill(-1);
  std::array<Byte, 32> state{}, mps{};
  std::array<uint16_t, 8> history{};
  for (const auto& [plane, bit] : order) {
    const uint16_t h = history[static_cast<size_t>(plane)];
    const size_t ctx = static_cast<size_t>(((plane & 1) << 4) | ((h & high) >> 5) | (h & low));
    const Byte st = state[ctx];
    const int size = D::kCodeSize[st];
    if (active[static_cast<size_t>(size)] < 0) {
      active[static_cast<size_t>(size)] = static_cast<int>(words.size());
      words.push_back({size});
    }
    Codeword& w = words[static_cast<size_t>(active[static_cast<size_t>(size)])];
    if (bit != mps[ctx]) {  // LPS closes the run
      w.lps = true;
      active[static_cast<size_t>(size)] = -1;
      if (st < 2) mps[ctx] = static_cast<Byte>(bit);
      state[ctx] = D::kLpsNext[st];
    } else if (++w.mps_symbols == (1 << size)) {
      active[static_cast<size_t>(size)] = -1;
      state[ctx] = D::kMpsNext[st];
    }
    history[static_cast<size_t>(plane)] = static_cast<uint16_t>((h << 1) | bit);
  }
  std::vector<Byte> stream;  // bits, MSB first, after the 4 header bits
  for (int i = 7; i >= 4; i--) stream.push_back(((plane_mode | context_mode) >> i) & 1);
  for (const Codeword& w : words) {
    if (!w.lps) { stream.push_back(0); continue; }  // a full run (or one still open at the end)
    const int fill = 0x7F >> w.size;
    int prefix = -1;
    for (int p = 0; p < (1 << w.size); p++) {
      if (D::kRunTable[static_cast<size_t>((p << (7 - w.size)) | fill)] == w.mps_symbols + 1) { prefix = p; break; }
    }
    stream.push_back(1);
    for (int i = w.size - 1; i >= 0; i--) stream.push_back(static_cast<Byte>((prefix >> i) & 1));
  }
  std::vector<Byte> out((stream.size() + 7) / 8 + 2, 0);
  for (size_t i = 0; i < stream.size(); i++) out[i / 8] |= static_cast<Byte>(stream[i] << (7 - i % 8));
  return out;
}

// Test images: noise, runs, and tile-like data with structure in each bit plane.
auto sdd1_samples() -> std::vector<std::vector<Byte>> {
  std::vector<std::vector<Byte>> samples;
  Rng rng{0x1234567};
  std::vector<Byte> noise(300), sparse(512), tiles(1024, 0), odd(77);
  for (auto& b : noise) b = static_cast<Byte>(rng.next());
  for (auto& b : sparse) b = rng.below(16) == 0 ? static_cast<Byte>(1 << rng.below(8)) : 0;
  for (size_t i = 0; i < tiles.size(); i++) tiles[i] = static_cast<Byte>((i & 2 ? 0xF0 : 0x3C) ^ (rng.below(32) == 0 ? 0x81 : 0));
  for (auto& b : odd) b = static_cast<Byte>(rng.below(4) ? 0xFF : rng.next());
  samples.push_back(noise);
  samples.push_back(sparse);
  samples.push_back(tiles);
  samples.push_back(odd);
  samples.push_back(std::vector<Byte>(2048, 0x00));  // long MPS runs reach code number 7
  samples.push_back({0x5A});
  return samples;
}

auto test_sdd1_decompressor() -> bool {
  // Run table (fullsnes): "1" + k bits, padded with ones, gives MPS symbols + 1 before the LPS.
  CHECK_EQ(SDD1Decompressor::kRunTable[0x7F], 1);    // size 0: LPS at once
  CHECK_EQ(SDD1Decompressor::kRunTable[0x3F], 2);    // size 1, bit 0: one MPS then LPS
  CHECK_EQ(SDD1Decompressor::kRunTable[0x00], 128);  // size 7, all zero: 127 MPS then LPS
  for (int i = 0; i < 128; i++) {  // a permutation of 1..128
    int hits = 0;
    for (Byte v : SDD1Decompressor::kRunTable) hits += v == i + 1;
    CHECK_EQ(hits, 1);
  }
  int cases = 0;
  for (const auto& data : sdd1_samples()) {
    for (const Byte planes : {Byte{0x00}, Byte{0x40}, Byte{0x80}, Byte{0xC0}}) {
      for (const Byte contexts : {Byte{0x00}, Byte{0x10}, Byte{0x20}, Byte{0x30}}) {
        const auto packed = sdd1_compress(data, planes, contexts);
        SDD1Decompressor d([&](uint32_t a) { return a < packed.size() ? packed[a] : Byte{0}; });
        d.init(0);
        size_t mismatch = data.size();
        for (size_t i = 0; i < data.size(); i++) {
          if (d.read() != data[i] && mismatch == data.size()) mismatch = i;
        }
        if (mismatch != data.size()) {
          std::printf("\n    mode %02X/%02X, %zu bytes: first difference at %zu", planes, contexts, data.size(), mismatch);
        }
        CHECK_EQ(mismatch, data.size());
        if (data.size() == 2048) CHECK(packed.size() < 64);  // all-zero data compresses to almost nothing
        cases++;
      }
    }
  }
  CHECK_EQ(cases, 96);
  return true;
}

auto make_sdd1_rom(const std::vector<Byte>& packed, uint32_t at) -> std::vector<Byte> {
  auto rom = make_rom({false, "STAR OCEAN", 0x32, 0x45, 0x0C, 0x03}, 0x400000);  // 4 MB, 8 KB SRAM
  for (size_t i = 0; i < rom.size(); i += 0x8000) rom[i] = static_cast<Byte>(i >> 15);  // tag each 32 KB
  put(rom, at, packed);
  return rom;
}

// Star Ocean's pattern (6 MB): $4806/$4807 flip between MB 2/3 and 4/5 around each decompression
// DMA. The decompressor and CPU reads must use the bank registers as they are at that moment.
auto test_sdd1_live_mmc() -> bool {
  const auto samples = sdd1_samples();
  const auto a = sdd1_compress(samples[0], 0x00, 0x00);
  const auto b = sdd1_compress(samples[2], 0x80, 0x00);
  const auto decoy = sdd1_compress(samples[1], 0x40, 0x10);
  auto rom = make_rom({false, "Star Ocean", 0x32, 0x45, 0x0D, 0x03}, 0x600000);
  for (size_t i = 0; i < rom.size(); i += 0x8000) rom[i] = static_cast<Byte>(i >> 15);
  rom[0x7FFC] = 0x01; rom[0x7FFD] = 0x80;  // reset vector $8001
  put(rom, 0x5FD0AB, a);                    // $FF:D0AB with $4807 = 5
  put(rom, 0x3ED27F, b);                    // $FE:D27F with $4807 = 3
  put(rom, 0x5ED27F, decoy);                // ... and what a stale $4807 = 5 would give
  const fs::path path = scratch_dir() / "sdd1_6mb.sfc";
  write_file(path, rom);
  memory::Cartridge cart;
  CHECK(cart.load(path.string()));
  CHECK(cart.map_mode() == memory::MapMode::LoROM);
  auto* chip = cart.coprocessor();
  CHECK(chip && chip->chip() == Chip::SDD1);
  if (!chip) return false;
  // LoROM $00-$3F|$80-$BF:8000-FFFF is the first 2 MB, linear; the vector comes from ROM $7FFC.
  CHECK_EQ(cart.read(0x00FFFC).value_or(0), 0x01);
  CHECK_EQ(cart.read(0x80FFFD).value_or(0), 0x80);
  CHECK_EQ(cart.read(0x008000).value_or(0xEE), 0x00);
  CHECK_EQ(cart.read(0x3F8000).value_or(0), 0x3F);   // ROM $1F8000
  CHECK_EQ(cart.read(0xBF8000).value_or(0), 0x3F);
  auto dma = [&](uint32_t source, size_t size) {
    for (Byte r = 2; r <= 6; r++) {
      const uint32_t v = r <= 4 ? source >> ((r - 2) * 8) : static_cast<uint32_t>(size >> ((r - 5) * 8));
      chip->dma_register_write(0, r, static_cast<Byte>(v));
    }
    cart.write(0x004800, 0x01);
    cart.write(0x004801, 0x01);
    std::vector<Byte> out;
    for (size_t i = 0; i < size; i++) out.push_back(cart.read(source).value_or(0));
    cart.write(0x004800, 0x00);
    return out;
  };
  for (int i = 0; i < 21; i++) {  // the game's flips before its first DMA (ends on 4/5)
    cart.write(0x004806, i & 1 ? 0x02 : 0x04);
    cart.write(0x004807, i & 1 ? 0x03 : 0x05);
  }
  CHECK_EQ(cart.read(0x004807).value_or(0), 0x05);
  CHECK(dma(0xFFD0AB, samples[0].size()) == samples[0]);
  cart.write(0x004806, 0x02);
  cart.write(0x004807, 0x03);
  CHECK(dma(0xFED27F, samples[2].size()) == samples[2]);  // MB 3, not the stale MB 5
  // CPU reads follow each switch at once.
  CHECK_EQ(cart.read(0xFE8000).value_or(0), 0x7D);       // ROM $3E8000
  CHECK_EQ(cart.read(0xEE8000).value_or(0), 0x5D);       // $4806 = 2: ROM $2E8000
  cart.write(0x004807, 0x05);
  CHECK_EQ(cart.read(0xFE8000).value_or(0), 0xBD);       // ROM $5E8000
  CHECK(dma(0xFED27F, samples[1].size()) == samples[1]);  // and now the MB 5 stream
  cart.write(0x004807, 0x03);
  CHECK(dma(0xFED27F, samples[2].size()) == samples[2]);
  CHECK_EQ(cart.read(0xD48000).value_or(0), 0x29);       // $4805 = 1 since reset: ROM $148000
  return true;
}

auto test_sdd1_mapping_dma() -> bool {
  const fs::path dir = scratch_dir();
  const std::vector<Byte> data = sdd1_samples()[2];
  const auto packed = sdd1_compress(data, 0x40, 0x10);
  auto rom = make_sdd1_rom(packed, 0x210000);  // MB 2, offset $10000
  write_file(dir / "sdd1.sfc", rom);
  memory::Cartridge cart;
  CHECK(cart.load((dir / "sdd1.sfc").string()));
  CHECK(cart.map_mode() == memory::MapMode::LoROM);
  auto* chip = cart.coprocessor();
  CHECK(chip && chip->chip() == Chip::SDD1);
  if (!chip) return false;
  CHECK(cart.load_warning().empty());
  CHECK_EQ(cart.sram_data().size(), 8192u);
  // Reset: banks 0-3, LoROM for $00-$3F|$80-$BF.
  for (Word r : {0, 1, 4, 5, 6, 7}) CHECK_EQ(cart.read(0x004800 + r).value_or(0xEE), r >= 4 ? r - 4 : 0);
  CHECK_EQ(cart.read(0x004802).value_or(0xEE), rom[0x4802]);  // unused registers read ROM
  CHECK_EQ(cart.read(0x018000).value_or(0), 0x01);  // LoROM bank 1 = 32 KB block 1
  CHECK_EQ(cart.read(0x208000).value_or(0), 0x20);  // $20 = 1 MB in
  CHECK_EQ(cart.read(0xA08000).value_or(0), 0x20);
  cart.write(0x004805, 0x80);                       // $20-$3F fold onto the first 2 MB
  CHECK_EQ(cart.read(0x208000).value_or(0), 0x00);
  CHECK_EQ(cart.read(0xA08000).value_or(0), 0x20);  // $A0-$BF follow $4807 bit 7
  cart.write(0x004807, 0x83);
  CHECK_EQ(cart.read(0xA18000).value_or(0), 0x01);
  CHECK_EQ(cart.read(0x004805).value_or(0), 0x80);  // the bank nibble is 0 now
  cart.write(0x004805, 0xF1);                       // bits 4-6 aren't stored
  CHECK_EQ(cart.read(0x004805).value_or(0), 0x81);
  // MMC: $C0/$D0/$E0/$F0 + 1 MB windows.
  cart.write(0x004804, 0x03);
  cart.write(0x004806, 0x00);
  CHECK_EQ(cart.read(0xC00000).value_or(0), 0x60);  // MB 3
  CHECK_EQ(cart.read(0xC18000).value_or(0), 0x63);
  CHECK_EQ(cart.read(0xD00000).value_or(0), 0x20);  // $4805 = 1
  CHECK_EQ(cart.read(0xE00000).value_or(0), 0x00);  // $4806 = 0
  CHECK_EQ(cart.read(0xF08000).value_or(0), 0x61);  // $4807 = 3 (not SRAM: the MMC owns $F0-$FF)
  // Save RAM (SHVC-LN3B): $00-$3F|$80-$BF:6000-7FFF in 8 KB pages, and $70-$73:0000-FFFF.
  cart.write(0x700010, 0xA5);
  CHECK_EQ(cart.read(0x700010).value_or(0), 0xA5);
  CHECK_EQ(cart.sram_data()[0x10], 0xA5);
  CHECK_EQ(cart.read(0x006010).value_or(0), 0xA5);  // the same byte through the $6000 window
  cart.write(0x807FFF, 0x3C);
  CHECK_EQ(cart.sram_data()[0x1FFF], 0x3C);
  CHECK_EQ(cart.read(0x737FFF).value_or(0), 0x3C);
  cart.write(0x016123, 0x77);                       // $01:6123 = page 1, mirrored onto 8 KB
  CHECK_EQ(cart.sram_data()[0x0123], 0x77);
  cart.write(0x71A123, 0x78);                       // $71:A123: bank $70-$73 maps 0000-FFFF
  CHECK_EQ(cart.sram_data()[0x0123], 0x78);
  CHECK_EQ(cart.read(0x006123).value_or(0), 0x78);
  cart.clear_sram_dirty();
  cart.write(0x006000, 0x01);
  CHECK(cart.sram_dirty());                         // battery save follows these writes
  {  // Street Fighter Alpha 2's board (SHVC-1N0N) has no RAM: the window stays open bus.
    auto sfa2 = make_rom({false, "STREET FIGHTER ALPHA2", 0x32, 0x43, 0x0C, 0x00}, 0x400000);
    write_file(dir / "sdd1_noram.sfc", sfa2);
    memory::Cartridge plain;
    CHECK(plain.load((dir / "sdd1_noram.sfc").string()));
    CHECK(plain.coprocessor() && plain.coprocessor()->chip() == Chip::SDD1);
    CHECK(plain.sram_data().empty());
    CHECK(!plain.read(0x006000).has_value());
    plain.write(0x006000, 0x12);
    CHECK(!plain.read(0x006000).has_value());
  }
  // DMA: channel 3 armed, source $E1:0000 (MB 2 via $4806), 1024 bytes.
  cart.write(0x004806, 0x02);
  for (const auto& [reg, value] : std::vector<std::pair<Byte, Byte>>{{2, 0x00}, {3, 0x00}, {4, 0xE1}, {5, 0x00}, {6, 0x04}}) {
    chip->dma_register_write(3, reg, value);
  }
  chip->dma_register_write(2, 4, 0xE1);             // another channel's registers don't matter
  cart.write(0x004800, 0x08);
  cart.write(0x004801, 0x08);
  std::vector<Byte> out;
  for (size_t i = 0; i < 400; i++) out.push_back(cart.read(0xE10000).value_or(0));
  CHECK_EQ(cart.read(0xE10001).value_or(0), packed[1]);  // other addresses read ROM meanwhile
  // A state taken mid-transfer continues identically.
  auto saver = Archive::saver();
  cart.serialize(saver);
  for (size_t i = 400; i < data.size(); i++) out.push_back(cart.read(0xE10000).value_or(0));
  CHECK(out == data);
  CHECK_EQ(cart.read(0x004801).value_or(0xFF), 0x00);   // finished: the channel disarms
  CHECK_EQ(cart.read(0xE10000).value_or(0), packed[0]);  // and the address reads ROM again
  memory::Cartridge restored;
  CHECK(restored.load((dir / "sdd1.sfc").string()));
  auto loader = Archive::loader(saver.data());
  restored.serialize(loader);
  CHECK(loader.ok());
  bool same = true;
  for (size_t i = 400; i < data.size(); i++) same = same && restored.read(0xE10000).value_or(0) == data[i];
  CHECK(same);
  return true;
}

// An S-CPU program DMAs (fixed source, S-DD1 on channel 0) decompressed data into WRAM $2000.
// First it waits on save RAM variables the way Star Ocean's sound driver does (through the
// $6000-$7FFF window and bank $70): without that RAM mapped it spins forever.
auto make_sdd1_cart(const std::vector<Byte>& packed, uint16_t size) -> std::vector<Byte> {
  auto rom = make_sdd1_rom(packed, 0x010000);
  put(rom, 0x0000, {0x78, 0x18, 0xFB, 0xE2, 0x30,         // sei clc xce sep #$30
                    0xA9, 0x5A, 0x8D, 0x00, 0x60,         // sta $6000 (DB = 0)
                    0xAD, 0x00, 0x60, 0xC9, 0x5A, 0xD0, 0xF9,  // wait until lda $6000 == $5A
                    0xA9, 0xA5, 0x8F, 0x10, 0x80, 0x70,   // sta.l $708010
                    0xAF, 0x10, 0x80, 0x70, 0xC9, 0xA5, 0xD0, 0xF8,  // wait until lda.l $708010 == $A5
                    0x9C, 0x81, 0x21, 0xA9, 0x20, 0x8D, 0x82, 0x21, 0x9C, 0x83, 0x21,  // WMADD = $7E2000
                    0xA9, 0x08, 0x8D, 0x00, 0x43,         // DMAP0: A->B, fixed A address
                    0xA9, 0x80, 0x8D, 0x01, 0x43,         // BBAD0 = $2180
                    0x9C, 0x02, 0x43, 0x9C, 0x03, 0x43,   // A1T0 = $0000
                    0xA9, 0xC1, 0x8D, 0x04, 0x43,         // A1B0 = $C1 (MB 0 + $10000)
                    0xA9, static_cast<Byte>(size), 0x8D, 0x05, 0x43,
                    0xA9, static_cast<Byte>(size >> 8), 0x8D, 0x06, 0x43,
                    0xA9, 0x01, 0x8D, 0x00, 0x48, 0x8D, 0x01, 0x48,  // S-DD1: channel 0 on, armed
                    0x8D, 0x0B, 0x42,                     // MDMAEN
                    0xEE, 0x00, 0x00,                     // inc $0000 (done marker)
                    0x80, 0xFE});
  return rom;
}

auto test_machine_sdd1() -> bool {
  const std::vector<Byte> data = sdd1_samples()[0];
  const auto packed = sdd1_compress(data, 0x00, 0x30);
  const fs::path path = scratch_dir() / "chiptest_sdd1.sfc";
  write_file(path, make_sdd1_cart(packed, static_cast<uint16_t>(data.size())));
  System sys;
  CHECK(sys.load_rom(path.string()));
  sys.reset();
  const Byte marker = sys.peek_wram(0);
  sys.run_frame();
  CHECK_EQ(static_cast<Byte>(sys.peek_wram(0) - marker), 1);
  bool same = true;
  for (size_t i = 0; i < data.size(); i++) same = same && sys.peek_wram(0x2000 + static_cast<uint32_t>(i)) == data[i];
  CHECK(same);
  CHECK_EQ(sys.cartridge().read(0x004801).value_or(0xFF), 0x00);
  const auto ram = sys.sram();                    // battery RAM: Star Ocean's 8 KB
  CHECK_EQ(ram.size(), 8192u);
  if (ram.size() == 8192) {
    CHECK_EQ(ram[0x0000], 0x5A);
    CHECK_EQ(ram[0x0010], 0xA5);                  // $70:8010 = offset $8010, mirrored into 8 KB
  }
  CHECK(sys.sram_dirty());
  return check_determinism(path.string(), 2, 10);
}

// ==== SPC7110 ======================================================================================

// Reference SPC7110 encoder: the binary arithmetic coder that inverts the decoder (fullsnes's
// algorithm). It runs the same context model and colour ranking on the known pixels and, for
// each decision, either keeps the lower (more probable) part of the interval or adds its size
// to the code value. The code value is kept as a bit string so carries can ripple back.
auto spc7110_compress(const std::vector<unsigned>& pixels_in, unsigned mode) -> std::vector<Byte> {
  using D = SPC7110Decompressor;
  std::vector<Byte> code;  // bits, MSB first
  size_t shifts = 0;
  unsigned top = 255;
  std::array<Byte, D::kContexts> state{}, invert{};
  auto add_at_window = [&](uint32_t value) {  // value is aligned to the decoder's 16-bit window
    if (code.size() < shifts + 16) code.resize(shifts + 16, 0);
    unsigned carry = 0;
    for (size_t i = 0; i < shifts + 16; i++) {
      const size_t pos = shifts + 15 - i;
      const unsigned sum = code[pos] + (i < 16 ? (value >> i) & 1 : 0) + carry;
      code[pos] = static_cast<Byte>(sum & 1);
      carry = sum >> 1;
      if (i >= 16 && !carry) break;
    }
  };
  auto encode_bit = [&](unsigned context, unsigned bit) {
    const Byte st = state[context];
    const unsigned probability = D::kProbability[st];
    top -= probability;
    if (bit != invert[context]) {  // LPS: skip past the MPS part of the interval
      add_at_window((top + 1) << 8);
      top = probability - 1;
      if (top > 79) invert[context] ^= 1;
      state[context] = D::kNextLps[st];
    } else if (top <= 126) {
      state[context] = D::kNextMps[st];
    }
    while (top <= 126) { top = (top << 1) | 1; shifts++; }
  };
  const unsigned bpp = 1u << mode;
  std::array<Byte, 16> order{};
  for (unsigned i = 0; i < 16; i++) order[i] = static_cast<Byte>(i);
  auto to_front = [](std::array<Byte, 16>& list, unsigned colour) {
    unsigned i = 0;
    while (list[i] != colour) i++;
    for (; i > 0; i--) list[i] = list[i - 1];
    list[0] = static_cast<Byte>(colour);
  };
  uint32_t out = 0;
  unsigned c = 0;
  const size_t rows = (pixels_in.size() + 7) / 8;
  auto pixel_at = [&](size_t i) { return i < pixels_in.size() ? pixels_in[i] : 0u; };
  for (size_t row = 0; row < rows; row++) {
    if (mode == 0) {
      for (unsigned half = 0; half < 2; half++) {
        unsigned nibble = 0;
        for (unsigned i = 0; i < 4; i++) nibble = (nibble << 1) | (pixel_at(row * 8 + half * 4 + i) & 1);
        const unsigned decoded = nibble ^ ((out >> 12) & 0xF);
        const unsigned base = half * 15;
        encode_bit(base, decoded >> 3);
        encode_bit(base + 1 + (decoded >> 3), (decoded >> 2) & 1);
        encode_bit(base + 3 + (decoded >> 2), (decoded >> 1) & 1);
        encode_bit(base + 7 + (decoded >> 1), decoded & 1);
        out = (out << 4) | nibble;
      }
      continue;
    }
    for (unsigned px = 0; px < 8; px++) {
      const unsigned want = pixel_at(row * 8 + px);
      const unsigned a = mode == 1 ? (out >> 2) & 3 : out & 15;
      const unsigned b = mode == 1 ? (out >> 14) & 3 : (out >> 28) & 15;
      const unsigned k = D::neighbour_class(a, b, c);
      to_front(order, a);
      std::array<Byte, 16> ranked = order;
      to_front(ranked, c);
      to_front(ranked, b);
      to_front(ranked, a);
      unsigned rank = 0;
      while (ranked[rank] != want) rank++;
      if (mode == 1) {
        encode_bit(k, rank >> 1);
        encode_bit(k * 2 + 5 + (rank >> 1), rank & 1);
      } else {
        encode_bit(0, rank >> 3);
        encode_bit(1 + (rank >> 3), (rank >> 2) & 1);
        const unsigned two = rank >> 2;
        const unsigned context = two >= 2 ? two + 11 : k + 3 + two * 5;
        encode_bit(context, (rank >> 1) & 1);
        encode_bit(D::kMode2Context[context] + ((rank >> 1) & 1), rank & 1);
      }
      out = (out << bpp) | want;
      c = b;
    }
  }
  std::vector<Byte> bytes((code.size() + 7) / 8 + 4, 0);
  for (size_t i = 0; i < code.size(); i++) bytes[i / 8] |= static_cast<Byte>(code[i] << (7 - i % 8));
  return bytes;
}

// Pixels with the kind of structure the model is built for: flat areas, repeats from the row
// above (8 pixels back), and noise.
auto spc7110_pixels(unsigned mode, size_t count, uint32_t seed) -> std::vector<unsigned> {
  const unsigned mask = (1u << (1u << mode)) - 1;
  Rng rng{seed};
  std::vector<unsigned> p(count);
  for (size_t i = 0; i < count; i++) {
    const unsigned r = rng.below(10);
    p[i] = r < 4 && i ? p[i - 1] : r < 7 && i >= 8 ? p[i - 8] : rng.next() & mask;
  }
  return p;
}

// Byte n of a decoded row = bit (bpp - 1 - n) of the row's 8 pixels, first pixel in bit 7.
auto spc7110_row(const std::vector<unsigned>& p, size_t first, unsigned bpp) -> uint32_t {
  uint32_t result = 0;
  for (unsigned n = 0; n < bpp; n++) {
    for (unsigned i = 0; i < 8; i++) {
      const unsigned pixel = first + i < p.size() ? p[first + i] : 0;
      result |= static_cast<uint32_t>((pixel >> (bpp - 1 - n)) & 1) << (8 * n + 7 - i);
    }
  }
  return bpp == 1 ? result & 0xFF : result;
}

auto test_spc7110_decompressor() -> bool {
  CHECK_EQ(SPC7110Decompressor::neighbour_class(1, 1, 1), 0u);
  CHECK_EQ(SPC7110Decompressor::neighbour_class(1, 1, 2), 1u);
  CHECK_EQ(SPC7110Decompressor::neighbour_class(2, 1, 1), 2u);
  CHECK_EQ(SPC7110Decompressor::neighbour_class(1, 2, 1), 3u);
  CHECK_EQ(SPC7110Decompressor::neighbour_class(1, 2, 3), 4u);
  // The compression example in fullsnes ("SNES Cart SPC7110 Notes"): the same 64-byte string
  // packed in each mode, read back in the chip's tile byte order.
  {
    const std::string text = "Test123.ABCDABCDAAAAAAAAaaaabbbbccccdddd7654321076543210.Test123";
    const std::vector<std::vector<Byte>> packed = {
      {0x68, 0x91, 0x36, 0x15, 0xF8, 0xBF, 0x42, 0x35, 0x2F, 0x67, 0x3D, 0xB7, 0xAA, 0x05, 0xB4, 0xF7, 0x70, 0x7A,
       0x26, 0x20, 0xEA, 0x58, 0x2C, 0x09, 0x61, 0x00, 0xC5, 0x00, 0x8C, 0x6F, 0xFF, 0xD1, 0x42, 0x9D, 0xEE, 0x7F,
       0x72, 0x87, 0xDF, 0xD6, 0x5F, 0x92, 0x65, 0x00, 0x00},
      {0x4B, 0xF6, 0x80, 0x1E, 0x3A, 0x4C, 0x42, 0x6C, 0xDA, 0x16, 0x0F, 0xC6, 0x44, 0xED, 0x64, 0x10, 0x77, 0xAF,
       0x50, 0x00, 0x05, 0xC0, 0x01, 0x27, 0x22, 0xB0, 0x83, 0x51, 0x05, 0x32, 0x4A, 0x1E, 0x74, 0x93, 0x08, 0x76,
       0x07, 0xE5, 0x32, 0x12, 0xB4, 0x99, 0x9E, 0x55, 0xA3, 0xF8, 0x00},
      {0x13, 0xB3, 0x27, 0xA6, 0xF4, 0x5C, 0xD8, 0xED, 0x6C, 0x6D, 0xF8, 0x76, 0x80, 0xA7, 0x87, 0x20, 0x39, 0x4B,
       0x37, 0x1A, 0xCC, 0x3F, 0xE4, 0x3D, 0xBE, 0x65, 0x2D, 0x89, 0x7E, 0x0B, 0x0A, 0xD3, 0x46, 0xD5, 0x0C, 0x1F,
       0xD3, 0x81, 0xF3, 0xAD, 0xDD, 0xE8, 0x5C, 0xC0, 0xBD, 0x62, 0xAA, 0xCB, 0xF8, 0xB5, 0x38, 0x00},
    };
    for (unsigned mode = 0; mode < 3; mode++) {
      const auto& src = packed[mode];
      SPC7110Decompressor d([&](uint32_t a) { return a < src.size() ? src[a] : Byte{0}; });
      d.initialize(mode, 0);
      std::string got;
      const unsigned bpp = 1u << mode;
      while (got.size() < text.size()) {  // one 8x8 tile of 8 * bpp bytes at a time
        std::array<Byte, 32> tile{};
        for (unsigned row = 0; row < 8; row++) {
          d.decode();
          const uint32_t r = d.result();
          if (bpp == 1) tile[row] = static_cast<Byte>(r);
          else {
            tile[row * 2] = static_cast<Byte>(r);
            tile[row * 2 + 1] = static_cast<Byte>(r >> 8);
            if (bpp == 4) { tile[16 + row * 2] = static_cast<Byte>(r >> 16); tile[17 + row * 2] = static_cast<Byte>(r >> 24); }
          }
        }
        for (unsigned i = 0; i < 8 * bpp; i++) got += static_cast<char>(tile[i]);
      }
      got.resize(text.size());
      if (got != text) std::printf("\n    fullsnes example, mode %u: \"%s\"", mode, got.c_str());
      CHECK(got == text);
    }
  }
  for (unsigned mode = 0; mode < 3; mode++) {
    for (uint32_t seed : {1u, 77u, 4242u}) {
      const auto p = spc7110_pixels(mode, 8 * 600, seed);
      const auto packed = spc7110_compress(p, mode);
      SPC7110Decompressor d([&](uint32_t a) { return a < packed.size() ? packed[a] : Byte{0}; });
      d.initialize(mode, 0);
      const unsigned bpp = 1u << mode;
      size_t bad = SIZE_MAX;
      for (size_t row = 0; row < p.size() / 8; row++) {
        d.decode();
        const uint32_t got = bpp == 1 ? d.result() & 0xFF : d.result();  // 1 bpp: older pixels above
        if (got != spc7110_row(p, row * 8, bpp) && bad == SIZE_MAX) bad = row;
      }
      if (bad != SIZE_MAX) std::printf("\n    mode %u seed %u: first bad row %zu", mode, seed, bad);
      CHECK(bad == SIZE_MAX);
    }
    const std::vector<unsigned> flat(8 * 512, 1);  // a flat area costs next to nothing
    const auto packed = spc7110_compress(flat, mode);
    CHECK(packed.size() < 40);
    SPC7110Decompressor d([&](uint32_t a) { return a < packed.size() ? packed[a] : Byte{0}; });
    d.initialize(mode, 0);
    bool same = true;
    for (size_t row = 0; row < 512; row++) {
      d.decode();
      same = same && (mode == 0 ? d.result() & 0xFF : d.result()) == spc7110_row(flat, row * 8, 1u << mode);
    }
    CHECK(same);
  }
  return true;
}

// Program ROM (1 MB, tagged), then a data ROM; returns the file image.
auto make_spc7110_rom(size_t data_rom_bytes, bool rtc) -> std::vector<Byte> {
  auto rom = make_rom({true, "SPC7110 TEST", 0x3A, rtc ? Byte{0xF9} : Byte{0xF5}, 0x0D, 0x03, true, 0, 0x00},
                      SPC7110::kProgramRomSize + data_rom_bytes);
  for (size_t i = 0; i < rom.size(); i += 0x10000) {
    if ((i & 0xFFFF) == 0 && i != 0) rom[i] = static_cast<Byte>(i >> 16);
  }
  return rom;
}

auto load_cart(memory::Cartridge& cart, const std::vector<Byte>& rom, const char* name) -> SPC7110* {
  const fs::path path = scratch_dir() / name;
  write_file(path, rom);
  if (!cart.load(path.string())) return nullptr;
  auto* chip = cart.coprocessor();
  return chip && chip->chip() == Chip::SPC7110 ? static_cast<SPC7110*>(chip) : nullptr;
}

auto test_spc7110_mapping() -> bool {
  auto rom = make_spc7110_rom(0x500000, false);  // 5 MB data ROM: non-power-of-two mirroring
  rom[0x8000] = 0x5C;
  memory::Cartridge cart;
  SPC7110* chip = load_cart(cart, rom, "spc7110_map.sfc");
  CHECK(chip != nullptr);
  if (!chip) return false;
  CHECK(cart.map_mode() == memory::MapMode::HiROM);
  CHECK(cart.load_warning().empty());
  CHECK(chip->rtc() == nullptr);
  CHECK_EQ(cart.read(0x008000).value_or(0), 0x5C);          // program ROM, HiROM layout
  CHECK_EQ(cart.read(0xC08000).value_or(0), 0x5C);
  CHECK_EQ(cart.read(0x400000 | 0x8000).value_or(0), 0x5C);
  CHECK_EQ(cart.read(0x050000).value_or(0xEE), 0xEE);        // $00-$3F:0000-5FFF stays open bus
  CHECK_EQ(cart.read(0xC50000).value_or(0), 0x05);
  // Data ROM windows: $D0 -> MB $4831, $E0 -> $4832 (1), $F0 -> $4833 (2).
  const auto drom = [&](uint32_t offset) { return rom[SPC7110::kProgramRomSize + offset]; };
  CHECK_EQ(cart.read(0xD00000).value_or(0), drom(0x000000));
  CHECK_EQ(cart.read(0x004834).value_or(0xEE), 0);
  CHECK_EQ(cart.read(0xE10000).value_or(0), drom(0x010000));  // $4834 = 0: an 8 Mbit data ROM
  cart.write(0x004834, 0x02);                                 // 32 Mbit
  CHECK_EQ(cart.read(0xE10000).value_or(0), drom(0x110000));
  CHECK_EQ(cart.read(0xF20000).value_or(0), drom(0x220000));
  CHECK_EQ(cart.read(0x2A8000).value_or(0), drom(0x1A8000));  // $20-$2F:8000-FFFF too
  cart.write(0x004831, 0x0B);                                 // 3 bits: MB 3
  CHECK_EQ(cart.read(0x004831).value_or(0), 0x03);
  CHECK_EQ(cart.read(0xD30000).value_or(0), drom(0x330000));
  cart.write(0x004831, 0x04);                                 // MB 4 is past the 32 Mbit size
  CHECK_EQ(cart.read(0xD00000).value_or(0xEE), 0x00);
  cart.write(0x004834, 0x03);                                 // 64 Mbit: MB 4 is there, MB 5 mirrors
  CHECK_EQ(cart.read(0xD10000).value_or(0), drom(0x410000));
  cart.write(0x004831, 0x05);
  CHECK_EQ(cart.read(0xD10000).value_or(0), drom(0x410000));  // 5 MB data ROM: MB 5 = MB 4 mirrored
  // SRAM at $00-$3F:6000-7FFF, only with $4830 bit 7.
  CHECK_EQ(cart.read(0x006000).value_or(0xEE), 0x00);         // disabled: reads 0
  cart.write(0x006000, 0x11);
  cart.write(0x004830, 0xFF);
  CHECK_EQ(cart.read(0x004830).value_or(0), 0x87);
  CHECK_EQ(cart.read(0x006000).value_or(0xEE), 0x00);         // the disabled write went nowhere
  cart.write(0x016001, 0x22);                                 // bank 1 = 8 KB in: wraps on 8 KB
  CHECK_EQ(cart.sram_data()[0x0001], 0x22);
  CHECK_EQ(cart.read(0x806001).value_or(0), 0x22);
  cart.write(0x004830, 0x00);
  CHECK_EQ(cart.read(0x006001).value_or(0xEE), 0x00);
  // Registers outside the decoded set read as open bus; RTC ports are absent without the RTC.
  CHECK(!cart.read(0x00480D).has_value());
  CHECK(!cart.read(0x004840).has_value());
  return true;
}

// The cartridge check program's flow: test pass 1 leaves $11 x 16 at the end of save RAM, the
// game is reset, pass 2 finds it and writes "SPC7110 CHECK OK", and after power-off (the battery
// file) the next boot reads it back through the $6000 window.
auto test_spc7110_check_marker() -> bool {
  const auto rom = make_spc7110_rom(0x100000, false);
  const fs::path path = scratch_dir() / "spc7110_check.sfc";
  write_file(path, rom);
  System sys;
  CHECK(sys.load_rom(path.string()));
  sys.reset();
  auto& cart = sys.cartridge();
  CHECK_EQ(sys.sram().size(), 8192u);             // header $FFD8 = 3
  sys.clear_sram_dirty();
  cart.write(0x004830, 0x80);
  for (Word i = 0; i < 0x1FF0; i++) cart.write(0x006000 + i, 0xFF);
  for (Word i = 0; i < 16; i++) cart.write(0x007FF0 + i, 0x11);
  CHECK(sys.sram_dirty());
  sys.reset();                                    // soft reset keeps save RAM, disables it
  CHECK_EQ(cart.read(0x004830).value_or(0xEE), 0x00);
  CHECK_EQ(cart.read(0x007FF0).value_or(0xEE), 0x00);  // disabled: reads 0
  cart.write(0x004830, 0x80);
  bool pass1 = true;
  for (Word i = 0; i < 0x1FF0; i++) pass1 = pass1 && cart.read(0x806000 + i).value_or(0) == 0xFF;
  for (Word i = 0; i < 16; i++) pass1 = pass1 && cart.read(0x3F7FF0 + i).value_or(0) == 0x11;  // any bank's window
  CHECK(pass1);
  const char* marker = "SPC7110 CHECK OK";
  for (Word i = 0; i < 16; i++) cart.write(0x007FF0 + i, static_cast<Byte>(marker[i]));
  // Power off: the battery file is the save RAM span; a new System loads it back.
  const auto saved = std::vector<Byte>(sys.sram().begin(), sys.sram().end());
  CHECK(std::memcmp(saved.data() + 0x1FF0, marker, 16) == 0);
  System next;
  CHECK(next.load_rom(path.string()));
  next.reset();
  CHECK_EQ(next.sram().size(), saved.size());
  std::copy(saved.begin(), saved.end(), next.sram().begin());
  next.cartridge().write(0x004830, 0x80);
  bool boot = true;
  for (Word i = 0; i < 16; i++) boot = boot && next.cartridge().read(0x007FF0 + i).value_or(0) == static_cast<Byte>(marker[i]);
  CHECK(boot);
  return true;
}

// Data ROM image with a decompression table at $1000 and streams after it.
struct DcuCase { unsigned mode; Byte r480b; Byte r4805, r4806, r4807; };

auto test_spc7110_dcu() -> bool {
  auto rom = make_spc7110_rom(0x100000, false);
  const uint32_t drom = SPC7110::kProgramRomSize;
  std::array<std::vector<unsigned>, 3> pixels;
  std::array<uint32_t, 3> stream_at = {0x2000, 0x8000, 0x20000};
  for (unsigned mode = 0; mode < 3; mode++) {
    pixels[mode] = spc7110_pixels(mode, 8 * 2000, 99 + mode);
    const auto packed = spc7110_compress(pixels[mode], mode);
    put(rom, drom + stream_at[mode], packed);
    const uint32_t entry = 0x1000 + 4 * (5 + mode);           // table index 5, 6, 7
    put(rom, drom + entry, {static_cast<Byte>(mode), static_cast<Byte>(stream_at[mode] >> 16),
                            static_cast<Byte>(stream_at[mode] >> 8), static_cast<Byte>(stream_at[mode])});
  }
  put(rom, drom + 0x1000 + 4 * 8, {0x03, 0x00, 0x00, 0x00});  // index 8: invalid mode 3
  memory::Cartridge cart;
  SPC7110* chip = load_cart(cart, rom, "spc7110_dcu.sfc");
  CHECK(chip != nullptr);
  if (!chip) return false;
  const DcuCase cases[] = {
    {0, 0x00, 0, 0, 0}, {1, 0x00, 0, 0, 0}, {2, 0x00, 0, 0, 0},
    {1, 0x01, 0, 0, 3},   // skip 3 rows after each row
    {2, 0x02, 7, 0, 0},   // skip 7 rows before the first
    {0, 0x03, 2, 0, 0},   // both, with a row skip of 0 (the row repeats)
  };
  for (const DcuCase& c : cases) {
    const unsigned bpp = 1u << c.mode;
    cart.write(0x004801, 0x00);
    cart.write(0x004802, 0x10);
    cart.write(0x004803, 0x00);
    cart.write(0x004804, static_cast<Byte>(5 + c.mode));
    cart.write(0x00480B, c.r480b);
    cart.write(0x004805, c.r4805);
    cart.write(0x004807, c.r4807);
    cart.write(0x004809, 0x00);
    cart.write(0x00480A, 0x01);
    cart.write(0x004806, c.r4806);                             // start
    CHECK_EQ(cart.read(0x00480C).value_or(0), 0x80);
    // Expected: decode #1 (+ the initial skip) is row 0, then each row skips (r4807 or 1).
    size_t decode = 1 + (c.r480b & 2 ? (c.r4805 | c.r4806 << 8) : 0);
    const size_t step = c.r480b & 1 ? c.r4807 : 1;
    bool same = true;
    for (int tile = 0; tile < 6; tile++) {
      std::array<Byte, 32> want{};
      for (unsigned row = 0; row < 8; row++, decode += step) {
        const uint32_t r = spc7110_row(pixels[c.mode], (decode - 1) * 8, bpp);
        if (bpp == 1) {
          want[row] = static_cast<Byte>(r);
        } else {
          want[row * 2] = static_cast<Byte>(r);
          want[row * 2 + 1] = static_cast<Byte>(r >> 8);
          if (bpp == 4) { want[row * 2 + 16] = static_cast<Byte>(r >> 16); want[row * 2 + 17] = static_cast<Byte>(r >> 24); }
        }
      }
      for (unsigned i = 0; i < 8 * bpp; i++) {
        const Byte got = (tile & 1 ? cart.read(0x500000 + i) : cart.read(0x004800)).value_or(0xEE);  // $50:xxxx mirrors $4800
        same = same && got == want[i];
      }
    }
    if (!same) std::printf("\n    mode %u, $480B=%02X: tile data differs", c.mode, c.r480b);
    CHECK(same);
    const unsigned reads = 6 * 8 * bpp;
    CHECK_EQ(static_cast<unsigned>(cart.read(0x004809).value_or(0) | cart.read(0x00480A).value_or(0) << 8), (0x100 - reads) & 0xFFFF);
  }
  // A state mid-tile resumes exactly.
  cart.write(0x00480B, 0x00);
  cart.write(0x004804, 0x07);
  cart.write(0x004806, 0x00);
  for (int i = 0; i < 40; i++) (void)cart.read(0x004800);
  auto saver = Archive::saver();
  cart.serialize(saver);
  std::vector<Byte> after;
  for (int i = 0; i < 200; i++) after.push_back(cart.read(0x004800).value_or(0));
  memory::Cartridge restored;
  CHECK(load_cart(restored, rom, "spc7110_dcu.sfc") != nullptr);
  auto loader = Archive::loader(saver.data());
  restored.serialize(loader);
  CHECK(loader.ok());
  bool same = true;
  for (int i = 0; i < 200; i++) same = same && restored.read(0x004800).value_or(0) == after[static_cast<size_t>(i)];
  CHECK(same);
  // Mode 3 doesn't start a transfer: status stays clear and $4800 reads 0.
  cart.write(0x004804, 0x08);
  cart.write(0x004806, 0x00);
  CHECK_EQ(cart.read(0x00480C).value_or(0xFF), 0x00);
  CHECK_EQ(cart.read(0x004800).value_or(0xFF), 0x00);
  CHECK_EQ(cart.read(0x004808).value_or(0xFF), 0x00);
  return true;
}

auto test_spc7110_data_port() -> bool {
  auto rom = make_spc7110_rom(0x100000, false);
  const uint32_t drom = SPC7110::kProgramRomSize;
  for (uint32_t i = 0; i < 0x100000; i++) rom[drom + i] = static_cast<Byte>(i * 7 + (i >> 8) * 13);
  memory::Cartridge cart;
  SPC7110* chip = load_cart(cart, rom, "spc7110_port.sfc");
  CHECK(chip != nullptr);
  if (!chip) return false;
  auto at = [&](uint32_t a) { return rom[drom + (a & 0xFFFFF)]; };
  auto port_offset = [&] {
    return static_cast<uint32_t>(cart.read(0x004811).value_or(0) | cart.read(0x004812).value_or(0) << 8 |
                                 cart.read(0x004813).value_or(0) << 16);
  };
  auto port_adjust = [&] { return static_cast<uint32_t>(cart.read(0x004814).value_or(0) | cart.read(0x004815).value_or(0) << 8); };
  // A reference model of the port, run against the chip for every control mode.
  Rng rng{31337};
  int checked = 0;
  for (unsigned control = 0; control < 0x80; control++) {
    uint32_t offset = 0x40000 + rng.below(0x1000), adjust = rng.below(0x10000), stride = rng.below(0x10000);
    if (control & 8) adjust |= 0x8000;   // exercise negative adjusts
    cart.write(0x004818, 0x00);           // no auto-adjust while loading the registers
    cart.write(0x004811, static_cast<Byte>(offset));
    cart.write(0x004812, static_cast<Byte>(offset >> 8));
    cart.write(0x004814, static_cast<Byte>(adjust));
    cart.write(0x004816, static_cast<Byte>(stride));
    cart.write(0x004817, static_cast<Byte>(stride >> 8));
    cart.write(0x004815, static_cast<Byte>(adjust >> 8));
    cart.write(0x004813, static_cast<Byte>(offset >> 16));
    cart.write(0x004818, static_cast<Byte>(control));
    auto s16 = [](uint32_t v) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))); };
    auto expected = [&] {
      uint32_t a = control & 2 ? adjust : 0;
      if (control & 8) a = s16(a);
      return at(offset + a);
    };
    CHECK_EQ(cart.read(0x004810).value_or(0), expected());  // $4818 write loaded the port
    {  // the $4810 read advanced it
      uint32_t st = control & 1 ? stride : 1;
      if (control & 4) st = s16(st);
      uint32_t ad = control & 8 ? s16(adjust) : adjust;
      if (control & 16) adjust = (ad + st) & 0xFFFF; else offset = (offset + st) & 0xFFFFFF;
    }
    CHECK_EQ(cart.read(0x004810).value_or(0), expected());
    { uint32_t st = control & 1 ? stride : 1; if (control & 4) st = s16(st); uint32_t ad = control & 8 ? s16(adjust) : adjust;
      if (control & 16) adjust = (ad + st) & 0xFFFF; else offset = (offset + st) & 0xFFFFFF; }
    CHECK_EQ(port_offset(), offset);
    CHECK_EQ(port_adjust(), adjust);
    // $481A (mode 3), $4814 (mode 1) and $4815 (mode 2) writes/reads add the adjust to the offset.
    const unsigned trigger = control >> 5;
    if (trigger == 3) {
      CHECK_EQ(cart.read(0x00481A).value_or(0xFF), 0x00);
    } else if (trigger == 1) {
      cart.write(0x004814, static_cast<Byte>(adjust));
    } else if (trigger == 2) {
      cart.write(0x004815, static_cast<Byte>(adjust >> 8));
    }
    if (trigger) offset = (offset + (control & 8 ? s16(adjust) : adjust)) & 0xFFFFFF;
    CHECK_EQ(port_offset(), offset);
    CHECK_EQ(cart.read(0x004810).value_or(0), expected());
    checked++;
  }
  CHECK_EQ(checked, 128);
  return true;
}

auto test_spc7110_alu() -> bool {
  memory::Cartridge cart;
  SPC7110* chip = load_cart(cart, make_spc7110_rom(0x100000, false), "spc7110_alu.sfc");
  CHECK(chip != nullptr);
  if (!chip) return false;
  auto put32 = [&](Word base, uint32_t v) { for (Word i = 0; i < 4; i++) cart.write(base + i, static_cast<Byte>(v >> (8 * i))); };
  auto get32 = [&](Word base) {
    uint32_t v = 0;
    for (Word i = 0; i < 4; i++) v |= static_cast<uint32_t>(cart.read(base + i).value_or(0)) << (8 * i);
    return v;
  };
  auto get16 = [&](Word base) { return static_cast<uint32_t>(cart.read(base).value_or(0) | cart.read(base + 1).value_or(0) << 8); };
  Rng rng{2024};
  for (int i = 0; i < 2000; i++) {
    const bool is_signed = i & 1;
    cart.write(0x00482E, is_signed ? 0x01 : 0x00);
    // Multiply: $4824-$4825 x $4820-$4821 -> $4828-$482B; writing $4825 starts it.
    const auto a = static_cast<uint16_t>(rng.next()), b = static_cast<uint16_t>(rng.next());
    cart.write(0x004820, static_cast<Byte>(b));
    cart.write(0x004821, static_cast<Byte>(b >> 8));
    cart.write(0x004824, static_cast<Byte>(a));
    cart.write(0x004825, static_cast<Byte>(a >> 8));
    const uint32_t product = is_signed ? static_cast<uint32_t>(int64_t{static_cast<int16_t>(a)} * static_cast<int16_t>(b))
                                       : static_cast<uint32_t>(a) * b;
    CHECK_EQ(get32(0x4828), product);
    CHECK_EQ(cart.read(0x00482F).value_or(0) & 0x80, 0);  // finished
    // Divide: $4820-$4823 / $4826-$4827 -> quotient $4828-$482B, remainder $482C-$482D.
    const uint32_t dividend = rng.next();
    const auto divisor = static_cast<uint16_t>(i % 50 == 0 ? 0 : rng.next() >> (rng.below(16)));
    put32(0x4820, dividend);
    cart.write(0x004826, static_cast<Byte>(divisor));
    cart.write(0x004827, static_cast<Byte>(divisor >> 8));
    int64_t q = 0, r = 0;
    if (is_signed) {
      const int64_t n = static_cast<int32_t>(dividend), d = static_cast<int16_t>(divisor);
      if (d) { q = n / d; r = n % d; } else { q = 0; r = n; }
    } else {
      if (divisor) { q = dividend / divisor; r = dividend % divisor; } else { q = 0; r = dividend; }
    }
    CHECK_EQ(get32(0x4828), static_cast<uint32_t>(q));
    CHECK_EQ(get16(0x482C), static_cast<uint32_t>(r) & 0xFFFF);
  }
  // The one signed overflow: -2^31 / -1 wraps instead of trapping.
  cart.write(0x00482E, 0x01);
  put32(0x4820, 0x80000000u);
  cart.write(0x004826, 0xFF);
  cart.write(0x004827, 0xFF);
  CHECK_EQ(get32(0x4828), 0x80000000u);
  CHECK_EQ(get16(0x482C), 0u);
  CHECK_EQ(cart.read(0x00482E).value_or(0), 0x01);
  cart.write(0x00482E, 0xFF);
  CHECK_EQ(cart.read(0x00482E).value_or(0), 0x01);
  return true;
}

auto rtc_write(memory::Cartridge& cart, int index, const std::vector<Byte>& nibbles) -> void {
  cart.write(0x004840, 0x01);
  cart.write(0x004841, 0x03);  // write mode
  cart.write(0x004841, static_cast<Byte>(index));
  for (Byte n : nibbles) cart.write(0x004841, n);
  cart.write(0x004840, 0x00);
}

auto rtc_read(memory::Cartridge& cart, int index, int count) -> std::vector<Byte> {
  cart.write(0x004840, 0x01);
  cart.write(0x004841, 0x0C);  // read mode
  cart.write(0x004841, static_cast<Byte>(index));
  std::vector<Byte> out;
  for (int i = 0; i < count; i++) out.push_back(cart.read(0x004841).value_or(0xEE));
  cart.write(0x004840, 0x00);
  return out;
}

// <rom>.rtc: the common layout (nibble pairs + Unix time); loading adds the real time since saving.
auto test_rtc_file() -> bool {
  memory::Cartridge cart;
  SPC7110* chip = load_cart(cart, make_spc7110_rom(0x100000, true), "spc7110_rtcfile.sfc");
  CHECK(chip != nullptr && chip->rtc() != nullptr);
  if (!chip || !chip->rtc()) return false;
  rtc_write(cart, 15, {0x4});
  rtc_write(cart, 0, {8, 5, 9, 5, 3, 2, 1, 3, 2, 1, 9, 9, 5});  // 1999-12-31 23:59:58 Friday
  const int64_t saved_at = 1'700'000'000;
  const auto file = chip->rtc_save(saved_at);
  CHECK_EQ(file.size(), 16u);
  if (file.size() != 16) return false;
  CHECK((std::vector<Byte>(file.begin(), file.begin() + 8)) == (std::vector<Byte>{0x58, 0x59, 0x23, 0x31, 0x12, 0x99, 0x25, 0x40}));
  CHECK_EQ(file[8], static_cast<Byte>(saved_at));
  CHECK_EQ(file[11], static_cast<Byte>(saved_at >> 24));
  // 1 day 2 h 3 min 4 s later (days, hours, minutes, seconds): 2000-01-02 02:03:02 Sunday.
  memory::Cartridge next;
  SPC7110* chip2 = load_cart(next, make_spc7110_rom(0x100000, true), "spc7110_rtcfile2.sfc");
  CHECK(chip2 && chip2->rtc_load(file, saved_at + 86400 + 2 * 3600 + 3 * 60 + 4));
  if (!chip2) return false;
  CHECK(rtc_read(next, 0, 13) == (std::vector<Byte>{2, 0, 3, 0, 2, 0, 2, 0, 1, 0, 0, 0, 0}));
  // Leap day: 2000-02-28 + 1 day = 02-29.
  RTC4513 rtc;
  std::array<Byte, 16> leap = {0x00, 0x00, 0x00, 0x28, 0x02, 0x00, 0x21, 0x40};
  CHECK(rtc.load_file(leap, 86400));
  CHECK_EQ(rtc.reg(6) + rtc.reg(7) * 10, 29);
  CHECK_EQ(rtc.reg(8), 2);
  // STOP set: the time stands still; the host clock going back: unchanged; wrong size: refused.
  std::array<Byte, 16> stopped = {0x00, 0x30, 0x00, 0x11, 0x11, 0x00, 0x20, 0x60};
  CHECK(rtc.load_file(stopped, 100000));
  CHECK_EQ(rtc.reg(2) + rtc.reg(3) * 10, 30);
  std::array<Byte, 16> future = {0x00, 0x30, 0x00, 0x11, 0x11, 0x00, 0x20, 0x40};
  future[8] = 0xFF;
  CHECK(rtc.load_file(future, 10));
  CHECK_EQ(rtc.reg(2) + rtc.reg(3) * 10, 30);
  CHECK(!rtc.load_file(std::span<const Byte>(leap.data(), 15), 0));
  // A board without the clock has no file.
  memory::Cartridge plain;
  SPC7110* chip3 = load_cart(plain, make_spc7110_rom(0x100000, false), "spc7110_nortc.sfc");
  CHECK(chip3 && chip3->rtc_save(saved_at).empty());
  return true;
}

auto test_rtc4513() -> bool {
  memory::Cartridge cart;
  SPC7110* chip = load_cart(cart, make_spc7110_rom(0x100000, true), "spc7110_rtc.sfc");
  CHECK(chip != nullptr && chip->rtc() != nullptr);
  if (!chip || !chip->rtc()) return false;
  // Power-on: the host's local time, 24-hour mode.
  {
    RTC4513 rtc;
    rtc.power_on(0);  // 1970-01-01 00:00:00 UTC, in local time
    std::time_t zero = 0;
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &zero);
#else
    localtime_r(&zero, &local);
#endif
    CHECK_EQ(rtc.reg(2) + rtc.reg(3) * 10, local.tm_min);
    CHECK_EQ(rtc.reg(10) + rtc.reg(11) * 10, local.tm_year % 100);
    CHECK_EQ(rtc.reg(12), local.tm_wday);
    CHECK_EQ(rtc.reg(15), 0x4);
    CHECK_EQ(rtc.reg(13), 0x2);
    CHECK_EQ(rtc.reg(1) & 0x8, 0);  // no battery failure
  }
  CHECK_EQ(cart.read(0x004842).value_or(0), 0x80);
  // Set 1999-12-31 23:59:58, Friday (5), 24 h; then read it back through the serial port.
  rtc_write(cart, 15, {0x4});                        // F: 24 h, running
  rtc_write(cart, 0, {8, 5, 9, 5, 3, 2, 1, 3, 2, 1, 9, 9, 5});
  CHECK(rtc_read(cart, 0, 13) == (std::vector<Byte>{8, 5, 9, 5, 3, 2, 1, 3, 2, 1, 9, 9, 5}));
  // The index wraps after register F.
  CHECK(rtc_read(cart, 14, 4) == (std::vector<Byte>{0, 4, 8, 5}));
  // Two emulated seconds: the year rolls over to 00, January 1st, Saturday. A reader that stays
  // selected across a tick sees the resync flag (bit 3 of registers 3/5/7/9/C) until deselect.
  chip->step(static_cast<uint32_t>(RTC4513::kMasterClock));
  chip->step(static_cast<uint32_t>(RTC4513::kMasterClock));
  cart.write(0x004840, 0x01);
  cart.write(0x004841, 0x0C);
  cart.write(0x004841, 0x00);
  std::vector<Byte> now;
  for (int i = 0; i < 13; i++) now.push_back(cart.read(0x004841).value_or(0xEE));
  CHECK(now == (std::vector<Byte>{0, 0, 0, 8, 0, 8, 1, 8, 1, 8, 0, 0, 6 | 8}));
  cart.write(0x004840, 0x00);
  CHECK(rtc_read(cart, 0, 13) == (std::vector<Byte>{0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 6}));
  // February 28th in a leap year goes to the 29th; in other years to March 1st.
  rtc_write(cart, 0, {9, 5, 9, 5, 3, 2, 8, 2, 2, 0, 4, 2, 0});  // 2024-02-28 23:59:59
  chip->rtc()->tick_second();
  CHECK(rtc_read(cart, 6, 4) == (std::vector<Byte>{9, 2, 2, 0}));
  rtc_write(cart, 0, {9, 5, 9, 5, 3, 2, 8, 2, 2, 0, 3, 2, 0});  // 2023-02-28 23:59:59
  chip->rtc()->tick_second();
  CHECK(rtc_read(cart, 6, 4) == (std::vector<Byte>{1, 0, 3, 0}));
  // Stop holds the time; hold defers a second until it's released.
  rtc_write(cart, 0, {0, 0});
  rtc_write(cart, 15, {0x6});                        // stop
  chip->step(static_cast<uint32_t>(RTC4513::kMasterClock) * 3);
  CHECK_EQ(rtc_read(cart, 0, 1)[0], 0);
  rtc_write(cart, 15, {0x4});
  rtc_write(cart, 13, {0x3});                        // hold (calendar stays on)
  chip->step(static_cast<uint32_t>(RTC4513::kMasterClock));
  CHECK_EQ(rtc_read(cart, 0, 1)[0], 0);
  rtc_write(cart, 13, {0x2});
  CHECK_EQ(rtc_read(cart, 0, 1)[0], 1);
  // Rounding: 0:45 -> the next minute, seconds 0.
  rtc_write(cart, 0, {5, 4, 7, 0});
  rtc_write(cart, 13, {0xA});
  CHECK(rtc_read(cart, 0, 4) == (std::vector<Byte>{0, 0, 8, 0}));
  CHECK_EQ(rtc_read(cart, 13, 1)[0], 0x2);
  // 12-hour mode: 11:59:59 PM -> 12:00:00 AM of the next day, Sunday.
  rtc_write(cart, 15, {0x0});
  rtc_write(cart, 0, {9, 5, 9, 5, 1, 1 | 4, 5, 1, 6, 0, 7, 9, 6});  // 1997-06-15 11:59:59 PM, Saturday
  chip->rtc()->tick_second();
  CHECK(rtc_read(cart, 4, 3) == (std::vector<Byte>{2, 1, 6}));
  CHECK_EQ(rtc_read(cart, 12, 1)[0], 0);
  rtc_write(cart, 0, {9, 5, 9, 5, 1, 1});            // 11:59:59 AM -> 12 PM, same day
  chip->rtc()->tick_second();
  CHECK(rtc_read(cart, 4, 3) == (std::vector<Byte>{2, 1 | 4, 6}));
  rtc_write(cart, 0, {9, 5, 9, 5});                  // 12:59:59 PM -> 1 PM
  chip->rtc()->tick_second();
  CHECK(rtc_read(cart, 4, 2) == (std::vector<Byte>{1, 4}));
  // Calendar off: midnight doesn't advance the date.
  rtc_write(cart, 13, {0x0});
  rtc_write(cart, 0, {9, 5, 9, 5, 1, 1 | 4});
  chip->rtc()->tick_second();
  CHECK(rtc_read(cart, 4, 3) == (std::vector<Byte>{2, 1, 6}));
  // Without chip select the data port is dead and the serial state resets.
  CHECK_EQ(cart.read(0x004841).value_or(0xEE), 0x00);
  cart.write(0x004840, 0x01);
  cart.write(0x004841, 0x05);                        // not a mode: ignored
  cart.write(0x004841, 0x0C);
  cart.write(0x004841, 0x06);
  CHECK_EQ(cart.read(0x004841).value_or(0xEE), 6);
  // RTC state is in the save state.
  auto saver = Archive::saver();
  cart.serialize(saver);
  memory::Cartridge restored;
  SPC7110* other = load_cart(restored, make_spc7110_rom(0x100000, true), "spc7110_rtc.sfc");
  CHECK(other && other->rtc());
  auto loader = Archive::loader(saver.data());
  restored.serialize(loader);
  CHECK(loader.ok());
  for (int i = 0; i < 16; i++) CHECK_EQ(other->rtc()->reg(i), chip->rtc()->reg(i));
  CHECK_EQ(restored.read(0x004841).value_or(0xEE), 1);  // mid-read: the next register follows
  // A state from the RTC board won't load on a board without one.
  memory::Cartridge plain;
  CHECK(load_cart(plain, make_spc7110_rom(0x100000, false), "spc7110_nortc.sfc") != nullptr);
  auto mismatched = Archive::loader(saver.data());
  plain.serialize(mismatched);
  CHECK(!mismatched.ok());
  return true;
}

// An S-CPU program starts the decompressor, then DMAs 4 bpp tiles from $50:0000 into WRAM $2000.
auto make_spc7110_cart(const std::vector<unsigned>& pixels) -> std::vector<Byte> {
  auto rom = make_spc7110_rom(0x100000, false);
  const uint32_t drom = SPC7110::kProgramRomSize;
  put(rom, drom + 0x0400, {0x02, 0x00, 0x08, 0x00});         // table entry 0: mode 2 at $000800
  put(rom, drom + 0x0800, spc7110_compress(pixels, 2));
  put(rom, 0x8000, {0x78, 0x18, 0xFB, 0xE2, 0x30,
                    0x9C, 0x01, 0x48, 0xA9, 0x04, 0x8D, 0x02, 0x48, 0x9C, 0x03, 0x48,  // table $000400
                    0x9C, 0x04, 0x48, 0x9C, 0x0B, 0x48,      // index 0, no skips
                    0x9C, 0x06, 0x48,                        // start
                    0x9C, 0x81, 0x21, 0xA9, 0x20, 0x8D, 0x82, 0x21, 0x9C, 0x83, 0x21,
                    0xA9, 0x08, 0x8D, 0x00, 0x43, 0xA9, 0x80, 0x8D, 0x01, 0x43,
                    0x9C, 0x02, 0x43, 0x9C, 0x03, 0x43, 0xA9, 0x50, 0x8D, 0x04, 0x43,  // $50:0000
                    0x9C, 0x05, 0x43, 0xA9, 0x02, 0x8D, 0x06, 0x43,  // 512 bytes = 16 tiles
                    0xA9, 0x01, 0x8D, 0x0B, 0x42,
                    0xEE, 0x00, 0x00, 0x80, 0xFE});
  rom[0xFFFC] = 0x00;
  rom[0xFFFD] = 0x80;
  return rom;
}

auto test_machine_spc7110() -> bool {
  const auto pixels = spc7110_pixels(2, 8 * 8 * 16, 5);
  const fs::path path = scratch_dir() / "chiptest_spc7110.sfc";
  write_file(path, make_spc7110_cart(pixels));
  System sys;
  CHECK(sys.load_rom(path.string()));
  sys.reset();
  const Byte marker = sys.peek_wram(0);
  sys.run_frame();
  CHECK_EQ(static_cast<Byte>(sys.peek_wram(0) - marker), 1);
  bool same = true;
  for (unsigned tile = 0; tile < 16; tile++) {
    for (unsigned row = 0; row < 8; row++) {
      const uint32_t r = spc7110_row(pixels, (tile * 8 + row) * 8, 4);
      const uint32_t base = 0x2000 + tile * 32;
      same = same && sys.peek_wram(base + row * 2) == static_cast<Byte>(r) &&
             sys.peek_wram(base + row * 2 + 1) == static_cast<Byte>(r >> 8) &&
             sys.peek_wram(base + row * 2 + 16) == static_cast<Byte>(r >> 16) &&
             sys.peek_wram(base + row * 2 + 17) == static_cast<Byte>(r >> 24);
    }
  }
  CHECK(same);
  return check_determinism(path.string(), 2, 10);
}

auto test_state_version() -> bool {
  CHECK_EQ(savestate::kVersion, 5u);
  return true;
}

} // namespace

int main(int argc, char** argv) {
  utils::Logger::set_level(utils::LogLevel::Error);
  int passed = 0, failed = 0;
  auto run_test = [&](const std::string& name, const std::function<bool()>& test) {
    std::printf("  %s...", name.c_str());
    std::fflush(stdout);
    checks_failed = 0;
    const bool ok = test() && checks_failed == 0;
    std::printf(" %s\n", ok ? "PASSED" : "FAILED");
    (ok ? passed : failed)++;
  };

  std::printf("=== uPD77C25 / uPD96050 ===\n");
  run_test("ALU and flags", test_upd_alu);
  run_test("Multiplier, data RAM, DP modifiers", test_upd_multiplier_memory);
  run_test("Jumps, call/return", test_upd_jumps);
  run_test("Host DR/SR handshake", test_upd_host_port);
  run_test("Board decoding and state", test_necdsp_board);
  std::printf("=== Super FX ===\n");
  run_test("ALU ops and flags", test_gsu_alu);
  run_test("MULT/UMULT/FMULT/LMULT", test_gsu_multiply);
  run_test("LOOP, STW/LDW/LDB", test_gsu_loop_memory);
  run_test("PLOT/RPIX bitplanes", test_gsu_plot);
  run_test("S-CPU interface: GO, RON/RAN, IRQ, CLSR", test_gsu_cpu_interface);
  run_test("Save state", test_gsu_state);
  std::printf("=== SA-1 ===\n");
  run_test("Multiply, divide, cumulative sum", test_sa1_arithmetic);
  run_test("Variable-length bit reader", test_sa1_bit_reader);
  run_test("Normal DMA", test_sa1_dma);
  run_test("Character conversion types 1 and 2", test_sa1_character_conversion);
  run_test("Bitmap view, BW-RAM/I-RAM protection", test_sa1_bitmap_and_protection);
  run_test("IRQ/vectors, Super MMC", test_sa1_interrupts_and_mmc);
  std::printf("=== Cx4 (HG51B169) ===\n");
  run_test("Generated data ROM SHA-256", test_cx4_generated_data_rom);
  run_test("ALU ops, shifts, flags", test_cx4_alu);
  run_test("Multiplier, data ROM, data RAM", test_cx4_multiply_rom_ram);
  run_test("Jumps, calls, skips, MAR/MDR bus, IRQ", test_cx4_flow_bus);
  run_test("Cache, DMA, vectors, wait-state timing", test_cx4_cache_dma_timing);
  run_test("Save state", test_cx4_state);
  std::printf("=== Cartridge ===\n");
  run_test("Board detection", test_board_detection);
  run_test("Firmware loading", test_firmware_loading);
  run_test("OBC1", test_obc1);
  run_test("Battery RAM on chip boards", test_chip_battery);
  run_test("Save state version", test_state_version);
  std::printf("=== S-DD1 ===\n");
  run_test("Decompressor round trip, 4 plane x 4 context modes", test_sdd1_decompressor);
  run_test("ROM mapping, MMC, DMA decompression, state", test_sdd1_mapping_dma);
  run_test("6 MB: bank switches between decompression DMAs", test_sdd1_live_mmc);
  std::printf("=== SPC7110 ===\n");
  run_test("Decompressor round trip, modes 0/1/2", test_spc7110_decompressor);
  run_test("Program/data ROM mapping, SRAM enable", test_spc7110_mapping);
  run_test("Decompression unit: table, tiles, skips, state", test_spc7110_dcu);
  run_test("Data port modes", test_spc7110_data_port);
  run_test("ALU multiply/divide", test_spc7110_alu);
  run_test("RTC-4513 registers, clock, state", test_rtc4513);
  run_test("RTC-4513 clock file: layout, real time since saving", test_rtc_file);
  run_test("Check-program marker survives reset and the battery file", test_spc7110_check_marker);
  std::printf("=== Machine ===\n");
  run_test("Cheats: Game Genie / PAR / raw decoding, cheats file", test_cheat_decoding);
  run_test("Cheats on a cart: read overrides, mirror, compare, PAR writes", test_machine_cheats);
  run_test("MSU-1 registers, data, tracks, loop, resampler, state", test_msu1_registers);
  run_test("MSU-1 cart: ID, playback mixed per frame, state", test_machine_msu1);
  run_test("SA-1 cart runs both CPUs, deterministic", test_machine_sa1);
  run_test("Super FX cart, deterministic", test_machine_gsu);
  run_test("S-DD1 cart: DMA decompression, deterministic", test_machine_sdd1);
  run_test("SPC7110 cart: decompressor DMA, deterministic", test_machine_spc7110);
  for (int i = 1; i < argc; i++) {
    run_test(std::string("Determinism ") + fs::path(argv[i]).filename().string(),
             [&] { return check_determinism(argv[i], 60, 60); });
  }

  std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
  return failed == 0 ? 0 : 1;
}
