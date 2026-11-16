// SPDX-License-Identifier: MIT
#pragma once
// Super FX (GSU-1 / GSU-2, a.k.a. MARIO chip): a 16-bit RISC CPU with a 512-byte instruction
// cache, buffered ROM/RAM ports and a pixel plotter that writes SNES bitplane tiles.
//
// It runs at 21.477 MHz (CLSR=1) or 10.74 MHz (CLSR=0); all costs here are in master clocks, so
// a 10 MHz cycle costs 2. The S-CPU sees the registers at $3000-$34FF, ROM in LoROM banks
// $00-$3F/$80-$BF and linear banks $40-$5F/$C0-$DF, and the game RAM at $6000-$7FFF and banks
// $70-$71. While the GSU runs and owns the ROM (SCMR RON) the S-CPU reads fixed interrupt
// vectors from ROM instead; while it owns the RAM (RAN) RAM reads are open bus.
#include "snes/coprocessor/coprocessor.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace snes::coprocessor {

class GSU final : public Coprocessor {
public:
  // `rom` and `ram` belong to the cartridge and outlive the chip.
  GSU(const std::vector<Byte>& rom, std::vector<Byte>& ram);

  auto chip() const -> Chip override { return Chip::SuperFX; }
  auto reset() -> void override;
  auto owns_map() const -> bool override { return true; }
  auto read(Address address) -> std::optional<Byte> override;
  auto write(Address address, Byte value) -> bool override;
  auto step(uint32_t master_clocks) -> void override;
  auto irq() const -> bool override { return (sfr_ & kIrq) != 0; }
  auto serialize(Archive& ar) -> void override;

  // ---- for tests ------------------------------------------------------------------------------
  auto reg(int n) const -> Word { return r_[static_cast<size_t>(n)]; }
  auto set_reg(int n, Word value) -> void { r_[static_cast<size_t>(n)] = value; }
  auto sfr() const -> Word { return sfr_; }
  auto running() const -> bool { return (sfr_ & kGo) != 0; }
  auto colr() const -> Byte { return colr_; }
  // Runs until STOP (or `max_clocks`), as if the S-CPU just wrote R15. Returns clocks used.
  auto run_until_stop(uint64_t max_clocks = 10'000'000) -> uint64_t;

  enum : Word {
    kZ = 0x0002, kCy = 0x0004, kS = 0x0008, kOv = 0x0010, kGo = 0x0020, kR = 0x0040,
    kAlt1 = 0x0100, kAlt2 = 0x0200, kIl = 0x0400, kIh = 0x0800, kB = 0x1000, kIrq = 0x8000,
  };

  // One 8-pixel row of a character being plotted; written back as bitplanes when the plotter
  // moves to another row, the row is full, or a pixel is read back.
  struct PlotRow {
    Word key = 0xFFFF;  // (y << 5) | (x >> 3), 0xFFFF = none
    Byte filled = 0;    // bit 7 - n: pixel n has been plotted
    std::array<Byte, 8> colour{};
    auto serialize(Archive& ar) -> void { ar(key, filled, colour); }
  };

  // Opcode handler: `n` is the low nibble of the opcode (the register or immediate).
  using Handler = void (GSU::*)(unsigned n);

private:
  const std::vector<Byte>& rom_;
  std::vector<Byte>& ram_;
  uint32_t rom_mask_ = 0;  // size - 1 when a power of two, else 0 (use modulo)
  uint32_t ram_mask_ = 0;

  // Registers
  std::array<Word, 16> r_{};
  Word sfr_ = 0;
  Byte pbr_ = 0, rombr_ = 0;
  bool rambr_ = false;
  Word cbr_ = 0;
  Byte scbr_ = 0;
  Byte scmr_ = 0;   // raw: HT1 - RON RAN HT0 MD1 MD0
  Byte colr_ = 0;
  Byte por_ = 0;    // OBJ FREEZEHIGH HIGHNIBBLE DITHER TRANSPARENT
  bool bramr_ = false;
  Byte vcr_ = 0x04;
  Byte cfgr_ = 0;   // IRQ mask (bit 7), MS0 fast multiply (bit 5)
  bool clsr_ = false;

  // Instruction stream: the GSU fetches one byte ahead of execution.
  Byte prefetch_ = 0x01;
  Word last_ram_word_ = 0;   // address used by the last word load/store (SBK writes back there)
  unsigned src_ = 0, dst_ = 0;  // FROM / TO selection
  bool r14_written_ = false, r15_written_ = false;

  // The ROM and RAM ports work in the background: a ROM read (started by writing R14) and a
  // RAM byte store finish `busy` clocks later; anything that needs the port waits for it.
  struct RomPort { unsigned busy = 0; Byte data = 0; } rom_port_;
  struct RamPort { unsigned busy = 0; Word address = 0; Byte data = 0; } ram_port_;

  std::array<Byte, 512> code_cache_{};
  std::array<bool, 32> line_loaded_{};
  PlotRow plot_now_{}, plot_prev_{};

  int64_t clock_budget_ = 0;  // master clocks the GSU may still run

  // ---- helpers ------------------------------------------------------------------------------
  auto flag(Word f) const -> bool { return (sfr_ & f) != 0; }
  auto set_flag(Word f, bool on) -> void { sfr_ = on ? static_cast<Word>(sfr_ | f) : static_cast<Word>(sfr_ & ~f); }
  auto write_reg(unsigned n, Word value) -> void;
  auto src() const -> Word { return r_[src_]; }
  auto to_dst(Word value) -> void { write_reg(dst_, value); }
  auto set_sz(Word value) -> void;   // S and Z from a 16-bit result
  auto commit(Word value) -> void;   // to_dst + S/Z + end_prefix: the common ALU tail
  auto end_prefix() -> void;         // clears B/ALT1/ALT2 and the FROM/TO selection
  auto cycle(unsigned slow, unsigned fast) const -> unsigned { return clsr_ ? fast : slow; }
  auto mem_cycle() const -> unsigned { return clsr_ ? 5 : 6; }
  auto bpp() const -> unsigned;
  auto screen_height_mode() const -> unsigned;
  auto immediate_or_reg(unsigned n) const -> Word { return flag(kAlt2) ? static_cast<Word>(n) : r_[n]; }

  auto rom_at(uint32_t linear) const -> Byte;
  auto ram_at(uint32_t linear) -> Byte&;
  auto bus_read(uint32_t address) -> Byte;  // GSU address space
  auto bus_write(uint32_t address, Byte value) -> void;
  auto ram_address(Word offset) const -> uint32_t { return 0x700000 + (static_cast<uint32_t>(rambr_) << 16) + offset; }

  auto spend(unsigned clocks) -> void;
  auto exec_one() -> void;
  auto fetch_code(Word address) -> Byte;
  auto load_cache_line(Word offset) -> void;
  auto take_prefetch() -> Byte;   // the byte to execute; prefetches at R15
  auto fetch_operand() -> Byte;   // R15 + 1, then prefetch there
  auto invalidate_cache() -> void { line_loaded_.fill(false); }

  auto start_rom_read() -> void;
  auto wait_rom_port() -> void { if (rom_port_.busy) spend(rom_port_.busy); }
  auto rom_port_byte() -> Byte { wait_rom_port(); return rom_port_.data; }
  auto wait_ram_port() -> void { if (ram_port_.busy) spend(ram_port_.busy); }
  auto ram_load(Word address) -> Byte;
  auto ram_store(Word address, Byte value) -> void;
  auto ram_load_word(Word address) -> Word;
  auto ram_store_word(Word address, Word value) -> void;

  auto color(Byte source) const -> Byte;
  auto char_address(Byte x, Byte y) const -> uint32_t;
  auto plot(Byte x, Byte y) -> void;
  auto rpix(Byte x, Byte y) -> Byte;
  auto retire_row(PlotRow& row) -> void;
  auto advance_plot_row(Word key) -> void;

  auto read_io(Word address) -> Byte;
  auto write_io(Word address, Byte value) -> void;

  // ---- instructions -------------------------------------------------------------------------
  friend struct GsuOpcodeTable;
  auto op_stop(unsigned) -> void;
  auto op_nop(unsigned) -> void;
  auto op_cache(unsigned) -> void;
  auto op_lsr(unsigned) -> void;
  auto op_rol(unsigned) -> void;
  auto op_branch(unsigned cond) -> void;
  auto op_to_move(unsigned n) -> void;
  auto op_with(unsigned n) -> void;
  auto op_store(unsigned n) -> void;
  auto op_loop(unsigned) -> void;
  auto op_alt(unsigned which) -> void;
  auto op_load(unsigned n) -> void;
  auto op_plot_rpix(unsigned) -> void;
  auto op_swap(unsigned) -> void;
  auto op_color_cmode(unsigned) -> void;
  auto op_not(unsigned) -> void;
  auto op_add_adc(unsigned n) -> void;
  auto op_sub_sbc_cmp(unsigned n) -> void;
  auto op_merge(unsigned) -> void;
  auto op_and_bic(unsigned n) -> void;
  auto op_mult_umult(unsigned n) -> void;
  auto op_sbk(unsigned) -> void;
  auto op_link(unsigned n) -> void;
  auto op_sex(unsigned) -> void;
  auto op_asr_div2(unsigned) -> void;
  auto op_ror(unsigned) -> void;
  auto op_jmp_ljmp(unsigned n) -> void;
  auto op_lob(unsigned) -> void;
  auto op_fmult_lmult(unsigned) -> void;
  auto op_ibt_lms_sms(unsigned n) -> void;
  auto op_from_moves(unsigned n) -> void;
  auto op_hib(unsigned) -> void;
  auto op_or_xor(unsigned n) -> void;
  auto op_inc(unsigned n) -> void;
  auto op_getc_ramb_romb(unsigned) -> void;
  auto op_dec(unsigned n) -> void;
  auto op_getb(unsigned) -> void;
  auto op_iwt_lm_sm(unsigned n) -> void;
};

} // namespace snes::coprocessor
