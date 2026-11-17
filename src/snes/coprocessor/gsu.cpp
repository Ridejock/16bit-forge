// SPDX-License-Identifier: MIT
#include "snes/coprocessor/gsu.hpp"

#include <algorithm>
#include <bit>
#include <utility>

namespace bitforge::coprocessor {

// ---- opcode table ------------------------------------------------------------------------------
// One row per run of opcodes that share a handler; the handler gets the opcode's low nibble.
// ALT1/ALT2 (set by the $3D-$3F prefixes) pick the variant inside each handler.
struct GsuOpcodeTable {
  struct Run { Byte first; Byte count; GSU::Handler handler; };
  static constexpr Run kRuns[] = {
    {0x00, 1, &GSU::op_stop},        {0x01, 1, &GSU::op_nop},          {0x02, 1, &GSU::op_cache},
    {0x03, 1, &GSU::op_lsr},         {0x04, 1, &GSU::op_rol},          {0x05, 11, &GSU::op_branch},
    {0x10, 16, &GSU::op_to_move},    {0x20, 16, &GSU::op_with},        {0x30, 12, &GSU::op_store},
    {0x3C, 1, &GSU::op_loop},        {0x3D, 3, &GSU::op_alt},          {0x40, 12, &GSU::op_load},
    {0x4C, 1, &GSU::op_plot_rpix},   {0x4D, 1, &GSU::op_swap},         {0x4E, 1, &GSU::op_color_cmode},
    {0x4F, 1, &GSU::op_not},         {0x50, 16, &GSU::op_add_adc},     {0x60, 16, &GSU::op_sub_sbc_cmp},
    {0x70, 1, &GSU::op_merge},       {0x71, 15, &GSU::op_and_bic},     {0x80, 16, &GSU::op_mult_umult},
    {0x90, 1, &GSU::op_sbk},         {0x91, 4, &GSU::op_link},         {0x95, 1, &GSU::op_sex},
    {0x96, 1, &GSU::op_asr_div2},    {0x97, 1, &GSU::op_ror},          {0x98, 6, &GSU::op_jmp_ljmp},
    {0x9E, 1, &GSU::op_lob},         {0x9F, 1, &GSU::op_fmult_lmult},  {0xA0, 16, &GSU::op_ibt_lms_sms},
    {0xB0, 16, &GSU::op_from_moves}, {0xC0, 1, &GSU::op_hib},          {0xC1, 15, &GSU::op_or_xor},
    {0xD0, 15, &GSU::op_inc},        {0xDF, 1, &GSU::op_getc_ramb_romb}, {0xE0, 15, &GSU::op_dec},
    {0xEF, 1, &GSU::op_getb},        {0xF0, 16, &GSU::op_iwt_lm_sm},
  };

  static auto build() -> std::array<GSU::Handler, 256> {
    std::array<GSU::Handler, 256> table{};
    for (const Run& run : kRuns) {
      for (int i = 0; i < run.count; i++) table[static_cast<size_t>(run.first + i)] = run.handler;
    }
    return table;
  }
  static auto get() -> const std::array<GSU::Handler, 256>& {
    static const std::array<GSU::Handler, 256> table = build();
    return table;
  }
};

namespace {

// S-CPU ROM reads while the GSU owns the ROM bus: every vector points into WRAM ($01xx).
constexpr std::array<Byte, 16> kRomLockedVectors = {
  0x00, 0x01, 0x00, 0x01, 0x04, 0x01, 0x00, 0x01, 0x00, 0x01, 0x08, 0x01, 0x00, 0x01, 0x0C, 0x01,
};

// Bits per pixel for SCMR MD (0: 2bpp, 1: 4bpp, 2: undefined, decodes as 4bpp, 3: 8bpp).
constexpr std::array<unsigned, 4> kBpp = {2, 4, 4, 8};

// Byte offset of bitplane n inside an 8x8 tile row: 0, 1, 16, 17, 32, 33, 48, 49.
constexpr auto plane_offset(unsigned n) -> unsigned { return ((n >> 1) << 4) + (n & 1); }

constexpr auto pow2_mask(size_t size) -> uint32_t {
  return (size != 0 && std::has_single_bit(size)) ? static_cast<uint32_t>(size - 1) : 0;
}

} // namespace

GSU::GSU(const std::vector<Byte>& rom, std::vector<Byte>& ram)
    : rom_(rom), ram_(ram), rom_mask_(pow2_mask(rom.size())), ram_mask_(pow2_mask(ram.size())) {
  reset();
}

auto GSU::reset() -> void {
  r_.fill(0);
  sfr_ = 0;
  pbr_ = rombr_ = 0;
  rambr_ = false;
  cbr_ = 0;
  scbr_ = scmr_ = colr_ = por_ = 0;
  bramr_ = false;
  vcr_ = 0x04;
  cfgr_ = 0;
  clsr_ = false;
  prefetch_ = 0x01;
  last_ram_word_ = 0;
  src_ = dst_ = 0;
  r14_written_ = r15_written_ = false;
  rom_port_ = {};
  ram_port_ = {};
  code_cache_.fill(0);
  invalidate_cache();
  plot_now_ = {};
  plot_prev_ = {};
  clock_budget_ = 0;
}

// ---- memory ------------------------------------------------------------------------------------

auto GSU::rom_at(uint32_t linear) const -> Byte {
  if (rom_.empty()) return 0;
  return rom_[rom_mask_ ? (linear & rom_mask_) : (linear % rom_.size())];
}

auto GSU::ram_at(uint32_t linear) -> Byte& {
  static Byte dummy = 0;
  if (ram_.empty()) return dummy = 0;
  return ram_[ram_mask_ ? (linear & ram_mask_) : (linear % ram_.size())];
}

// GSU address space: $00-$3F LoROM halves, $40-$5F linear ROM, $60-$7F RAM.
auto GSU::bus_read(uint32_t address) -> Byte {
  if ((address & 0xC00000) == 0x000000) return rom_at(((address & 0x3F0000) >> 1) | (address & 0x7FFF));
  if ((address & 0xE00000) == 0x400000) return rom_at(address & 0x1FFFFF);
  if ((address & 0xE00000) == 0x600000) return ram_at(address & 0x1FFFF);
  return 0;
}

auto GSU::bus_write(uint32_t address, Byte value) -> void {
  if ((address & 0xE00000) == 0x600000) ram_at(address & 0x1FFFF) = value;
}

// Advances time. Background port operations complete when their countdown reaches zero.
auto GSU::spend(unsigned clocks) -> void {
  clock_budget_ -= clocks;
  if (rom_port_.busy != 0) {
    rom_port_.busy = clocks >= rom_port_.busy ? 0 : rom_port_.busy - clocks;
    if (rom_port_.busy == 0) {  // ROM byte at ROMBR:R14 has arrived
      rom_port_.data = bus_read((static_cast<uint32_t>(rombr_) << 16) + r_[14]);
      set_flag(kR, false);
    }
  }
  if (ram_port_.busy != 0) {
    ram_port_.busy = clocks >= ram_port_.busy ? 0 : ram_port_.busy - clocks;
    if (ram_port_.busy == 0) bus_write(ram_address(ram_port_.address), ram_port_.data);
  }
}

// Writing R14 starts a ROM read in the background (SFR.R is set until it completes).
auto GSU::start_rom_read() -> void {
  rom_port_.busy = mem_cycle();
  set_flag(kR, true);
}

auto GSU::ram_load(Word address) -> Byte {
  wait_ram_port();
  return bus_read(ram_address(address));
}

// A byte store is posted to the RAM port; a second access waits for the first.
auto GSU::ram_store(Word address, Byte value) -> void {
  wait_ram_port();
  ram_port_ = {mem_cycle(), address, value};
}

// Word accesses touch the even/odd byte pair: low byte at `address`, high byte at `address ^ 1`.
auto GSU::ram_load_word(Word address) -> Word {
  const Byte low = ram_load(address);
  const Byte high = ram_load(static_cast<Word>(address ^ 1));
  return static_cast<Word>((high << 8) | low);
}

auto GSU::ram_store_word(Word address, Word value) -> void {
  ram_store(address, static_cast<Byte>(value));
  ram_store(static_cast<Word>(address ^ 1), static_cast<Byte>(value >> 8));
}

// ---- instruction fetch -------------------------------------------------------------------------
// Code inside the 512-byte window at CBR runs from the instruction cache, which is filled one
// 16-byte line at a time on first use. Code outside it is read directly from ROM/RAM at PBR.

auto GSU::load_cache_line(Word offset) -> void {
  const Word first = static_cast<Word>(offset & 0x1F0);
  const uint32_t from = (static_cast<uint32_t>(pbr_) << 16) | static_cast<Word>((cbr_ + first) & 0xFFF0);
  for (unsigned i = 0; i < 16; i++) {
    spend(mem_cycle());
    code_cache_[first + i] = bus_read(from + i);
  }
  line_loaded_[first >> 4] = true;
}

auto GSU::fetch_code(Word address) -> Byte {
  const Word offset = static_cast<Word>(address - cbr_);
  if (offset >= 512) {
    if (pbr_ < 0x60) wait_rom_port(); else wait_ram_port();
    spend(mem_cycle());
    return bus_read((static_cast<uint32_t>(pbr_) << 16) | address);
  }
  if (line_loaded_[offset >> 4]) {
    spend(cycle(2, 1));
  } else {
    load_cache_line(offset);
  }
  return code_cache_[offset];
}

auto GSU::take_prefetch() -> Byte {
  const Byte current = prefetch_;
  prefetch_ = fetch_code(r_[15]);
  r15_written_ = false;
  return current;
}

auto GSU::fetch_operand() -> Byte {
  r_[15] = static_cast<Word>(r_[15] + 1);
  return take_prefetch();
}

auto GSU::write_reg(unsigned n, Word value) -> void {
  r_[n] = value;
  r14_written_ = r14_written_ || n == 14;
  r15_written_ = r15_written_ || n == 15;
}

auto GSU::set_sz(Word value) -> void {
  set_flag(kZ, value == 0);
  set_flag(kS, (value & 0x8000) != 0);
}

auto GSU::commit(Word value) -> void {
  to_dst(value);
  set_sz(value);
  end_prefix();
}

auto GSU::end_prefix() -> void {
  set_flag(kB | kAlt1 | kAlt2, false);
  src_ = 0;
  dst_ = 0;
}

auto GSU::exec_one() -> void {
  const Byte opcode = take_prefetch();
  (this->*GsuOpcodeTable::get()[opcode])(opcode & 0x0F);
  if (std::exchange(r14_written_, false)) start_rom_read();
  if (!std::exchange(r15_written_, false)) r_[15] = static_cast<Word>(r_[15] + 1);
}

auto GSU::step(uint32_t master_clocks) -> void {
  if (running()) {
    clock_budget_ += master_clocks;
    while (running() && clock_budget_ > 0) exec_one();
    if (running()) return;
  } else if (rom_port_.busy != 0 || ram_port_.busy != 0) {
    spend(std::max(rom_port_.busy, ram_port_.busy));  // let posted port operations land
  }
  clock_budget_ = 0;
}

auto GSU::run_until_stop(uint64_t max_clocks) -> uint64_t {
  set_flag(kGo, true);
  uint64_t used = 0;
  for (; running() && used < max_clocks;) {
    const int64_t budget = clock_budget_;
    exec_one();
    used += static_cast<uint64_t>(budget - clock_budget_);
  }
  clock_budget_ = 0;
  return used;
}

// ---- bitmap ------------------------------------------------------------------------------------
// Pixel formats and the character layout follow fullsnes, "SNES Cart GSU-n Bitmap I/O Ports".

// COLOR/GETC input path: POR.2 replaces the low nibble with the high one, POR.3 keeps the old
// high nibble of COLOR.
auto GSU::color(Byte source) const -> Byte {
  if (por_ & 0x04) return static_cast<Byte>((colr_ & 0xF0) | (source >> 4));
  if (por_ & 0x08) return static_cast<Byte>((colr_ & 0xF0) | (source & 0x0F));
  return source;
}

auto GSU::bpp() const -> unsigned { return kBpp[scmr_ & 3]; }

// 0: 128 lines, 1: 160, 2: 192, 3: OBJ layout (SCMR HT1:HT0, or POR.4).
auto GSU::screen_height_mode() const -> unsigned {
  if (por_ & 0x10) return 3;
  const unsigned ht0 = (scmr_ >> 2) & 1, ht1 = (scmr_ >> 5) & 1;
  return (ht1 << 1) | ht0;
}

// Address of bitplane 0 of the tile row holding pixel (x, y).
auto GSU::char_address(Byte x, Byte y) const -> uint32_t {
  const unsigned column = x >> 3, row = y >> 3;
  unsigned tile = 0;
  switch (screen_height_mode()) {
    case 0: tile = column * 16 + row; break;
    case 1: tile = column * 20 + row; break;
    case 2: tile = column * 24 + row; break;
    default:  // 2D OBJ arrangement: four 128x128 quadrants of 16x16 tiles
      tile = (y >> 7) * 0x200 + (x >> 7) * 0x100 + (row & 15) * 16 + (column & 15);
      break;
  }
  const uint32_t tile_bytes = bpp() * 8;
  return 0x700000 + static_cast<uint32_t>(scbr_) * 0x400 + tile * tile_bytes + (y & 7u) * 2;
}

// Plotted pixels are gathered per 8-pixel tile row. When the plotter moves on, the previous
// row is written out (and the row before that is retired first).
auto GSU::advance_plot_row(Word key) -> void {
  retire_row(plot_prev_);
  plot_prev_ = plot_now_;
  plot_now_.filled = 0;
  plot_now_.key = key;
}

auto GSU::plot(Byte x, Byte y) -> void {
  const bool wide = (scmr_ & 3) == 3;  // 256 colours
  if (!(por_ & 0x01)) {               // colour 0 is transparent unless POR.0
    const Byte mask = (wide && !(por_ & 0x08)) ? 0xFF : 0x0F;
    if ((colr_ & mask) == 0) return;
  }
  Byte colour = colr_;
  if (!wide && (por_ & 0x02)) {  // dither: on odd (x ^ y) use the high nibble
    colour = static_cast<Byte>((((x ^ y) & 1) ? colour >> 4 : colour) & 0x0F);
  }

  const Word key = static_cast<Word>((y << 5) + (x >> 3));
  if (key != plot_now_.key) advance_plot_row(key);
  const unsigned bit = 7 - (x & 7u);  // pixel 0 is the MSB of each plane byte
  plot_now_.colour[bit] = colour;
  plot_now_.filled = static_cast<Byte>(plot_now_.filled | (1u << bit));
  if (plot_now_.filled == 0xFF) advance_plot_row(plot_now_.key);
}

auto GSU::rpix(Byte x, Byte y) -> Byte {
  retire_row(plot_prev_);
  retire_row(plot_now_);
  const uint32_t base = char_address(x, y);
  const unsigned bit = 7 - (x & 7u);
  unsigned colour = 0;
  for (unsigned plane = 0; plane < bpp(); plane++) {
    spend(mem_cycle());
    colour |= ((bus_read(base + plane_offset(plane)) >> bit) & 1u) << plane;
  }
  return static_cast<Byte>(colour);
}

// Writes a gathered row as bitplanes; pixels not plotted keep the bits already in RAM.
auto GSU::retire_row(PlotRow& row) -> void {
  if (row.filled == 0) return;
  const Byte x = static_cast<Byte>(row.key << 3);
  const Byte y = static_cast<Byte>(row.key >> 5);
  const uint32_t base = char_address(x, y);
  const bool partial = row.filled != 0xFF;
  for (unsigned plane = 0; plane < bpp(); plane++) {
    unsigned bits = 0;
    for (unsigned b = 0; b < 8; b++) bits |= ((row.colour[b] >> plane) & 1u) << b;
    const uint32_t at = base + plane_offset(plane);
    if (partial) {
      spend(mem_cycle());
      bits = (bits & row.filled) | (bus_read(at) & ~row.filled & 0xFFu);
    }
    spend(mem_cycle());
    bus_write(at, static_cast<Byte>(bits));
  }
  row.filled = 0;
}

// ---- S-CPU side --------------------------------------------------------------------------------

auto GSU::read(Address address) -> std::optional<Byte> {
  const Byte bank = static_cast<Byte>((address >> 16) & 0x7F);
  const Word offset = static_cast<Word>(address);
  const bool locked_rom = running() && (scmr_ & 0x10);
  const bool locked_ram = running() && (scmr_ & 0x08);

  if (bank < 0x40) {
    if (offset >= 0x3000 && offset < 0x3500) return read_io(offset);
    if (offset >= 0x6000 && offset < 0x8000) {
      if (locked_ram) return std::nullopt;
      return ram_at(offset & 0x1FFF);
    }
    if (offset >= 0x8000) {
      if (locked_rom) return kRomLockedVectors[offset & 15];
      return rom_at((static_cast<uint32_t>(bank & 0x3F) << 15) | (offset & 0x7FFF));
    }
    return std::nullopt;
  }
  if (bank < 0x60) {
    if (locked_rom) return kRomLockedVectors[offset & 15];
    return rom_at((static_cast<uint32_t>(bank & 0x1F) << 16) | offset);
  }
  if (bank >= 0x60 && bank < 0x7E) {
    if (locked_ram) return std::nullopt;
    return ram_at((static_cast<uint32_t>(bank & 0x1F) << 16) | offset);
  }
  return std::nullopt;
}

auto GSU::write(Address address, Byte value) -> bool {
  const Byte bank = static_cast<Byte>((address >> 16) & 0x7F);
  const Word offset = static_cast<Word>(address);
  if (bank < 0x40) {
    if (offset >= 0x3000 && offset < 0x3500) {
      write_io(offset, value);
      return true;
    }
    if (offset >= 0x6000 && offset < 0x8000) {
      ram_at(offset & 0x1FFF) = value;
      return true;
    }
    return false;
  }
  if (bank >= 0x60 && bank < 0x7E) {
    ram_at((static_cast<uint32_t>(bank & 0x1F) << 16) | offset) = value;
    return true;
  }
  return false;
}

auto GSU::read_io(Word address) -> Byte {
  address = static_cast<Word>(0x3000 | (address & 0x3FF));
  if (address >= 0x3100 && address < 0x3300) return code_cache_[(address - 0x3100 + cbr_) & 511];
  if (address < 0x3020) {
    const Word value = r_[(address >> 1) & 15];
    return static_cast<Byte>((address & 1) ? value >> 8 : value);
  }
  switch (address) {
    case 0x3030: return static_cast<Byte>(sfr_ & 0x7E);
    case 0x3031: {
      const Byte value = static_cast<Byte>((sfr_ & 0x9F00) >> 8);
      set_flag(kIrq, false);  // reading the high byte acknowledges the IRQ
      return value;
    }
    case 0x3034: return pbr_;
    case 0x3036: return rombr_;
    case 0x303B: return vcr_;
    case 0x303C: return rambr_ ? 1 : 0;
    case 0x303E: return static_cast<Byte>(cbr_);
    case 0x303F: return static_cast<Byte>(cbr_ >> 8);
    default: return 0x00;
  }
}

auto GSU::write_io(Word address, Byte value) -> void {
  address = static_cast<Word>(0x3000 | (address & 0x3FF));
  if (address >= 0x3100 && address < 0x3300) {
    const size_t at = (address - 0x3100 + cbr_) & 511;
    code_cache_[at] = value;
    if ((at & 15) == 15) line_loaded_[at >> 4] = true;  // writing a line's last byte validates it
    return;
  }
  if (address < 0x3020) {
    const unsigned n = (address >> 1) & 15;
    r_[n] = (address & 1) ? static_cast<Word>((value << 8) | (r_[n] & 0xFF))
                          : static_cast<Word>((r_[n] & 0xFF00) | value);
    if (n == 14) start_rom_read();
    if (address == 0x301F) set_flag(kGo, true);  // writing R15's high byte starts the GSU
    return;
  }
  switch (address) {
    case 0x3030: {
      const bool was_running = running();
      sfr_ = static_cast<Word>((sfr_ & 0xFF00) | value);
      if (was_running && !running()) {  // the S-CPU stopped it: cache base back to 0
        cbr_ = 0;
        invalidate_cache();
      }
      break;
    }
    case 0x3031: sfr_ = static_cast<Word>((value << 8) | (sfr_ & 0x00FF)); break;
    case 0x3033: bramr_ = value & 0x01; break;
    case 0x3034: pbr_ = value & 0x7F; invalidate_cache(); break;
    case 0x3037: cfgr_ = value & 0xA0; break;
    case 0x3038: scbr_ = value; break;
    case 0x3039: clsr_ = value & 0x01; break;
    case 0x303A: scmr_ = value & 0x3F; break;
    default: break;
  }
}

// ---- instructions ------------------------------------------------------------------------------
// Semantics from fullsnes "SNES Cart GSU-n CPU ... Opcodes" sections. Prefix state (B from WITH,
// ALT1/ALT2, FROM/TO selection) is consumed by every instruction except the prefixes and branches.

auto GSU::op_stop(unsigned) -> void {
  set_flag(kGo, false);
  if ((cfgr_ & 0x80) == 0) set_flag(kIrq, true);  // CFGR.7 masks the IRQ
  prefetch_ = 0x01;
  end_prefix();
}

auto GSU::op_nop(unsigned) -> void { end_prefix(); }

auto GSU::op_cache(unsigned) -> void {
  const Word base = static_cast<Word>(r_[15] & 0xFFF0);
  if (base != cbr_) {
    cbr_ = base;
    invalidate_cache();
  }
  end_prefix();
}

auto GSU::op_lsr(unsigned) -> void {
  const Word value = src();
  set_flag(kCy, value & 1);
  commit(static_cast<Word>(value >> 1));
}

auto GSU::op_rol(unsigned) -> void {
  const Word value = src();
  const Word result = static_cast<Word>((value << 1) | (flag(kCy) ? 1 : 0));
  set_flag(kCy, (value & 0x8000) != 0);
  commit(result);
}

// $05 BRA, $06 BGE, $07 BLT, then BNE/BEQ, BPL/BMI, BCC/BCS, BVC/BVS: odd opcodes branch when
// the tested flag is set, even ones when it is clear.
auto GSU::op_branch(unsigned cond) -> void {
  const auto offset = static_cast<int8_t>(fetch_operand());
  bool taken = true;
  if (cond == 0x6 || cond == 0x7) {
    taken = (flag(kS) != flag(kOv)) == (cond == 0x7);
  } else if (cond >= 0x8) {
    static constexpr Word kTested[4] = {kZ, kS, kCy, kOv};
    taken = flag(kTested[(cond - 8) >> 1]) == ((cond & 1) != 0);
  }
  if (taken) write_reg(15, static_cast<Word>(r_[15] + offset));
}

auto GSU::op_to_move(unsigned n) -> void {
  if (flag(kB)) {  // MOVE rN, after WITH
    write_reg(n, src());
    end_prefix();
    return;
  }
  dst_ = n;  // TO
}

auto GSU::op_with(unsigned n) -> void {
  src_ = n;
  dst_ = n;
  set_flag(kB, true);
}

auto GSU::op_store(unsigned n) -> void {  // STW (rN) / STB (rN)
  last_ram_word_ = r_[n];
  if (flag(kAlt1)) {
    ram_store(last_ram_word_, static_cast<Byte>(src()));
  } else {
    ram_store_word(last_ram_word_, src());
  }
  end_prefix();
}

auto GSU::op_loop(unsigned) -> void {
  const Word remaining = static_cast<Word>(r_[12] - 1);
  write_reg(12, remaining);
  set_sz(remaining);
  if (remaining) write_reg(15, r_[13]);
  end_prefix();
}

auto GSU::op_alt(unsigned which) -> void {  // $3D ALT1, $3E ALT2, $3F ALT3 (both)
  set_flag(kB, false);
  const unsigned bits = which - 0xC;  // 1, 2, 3
  if (bits & 1) set_flag(kAlt1, true);
  if (bits & 2) set_flag(kAlt2, true);
}

auto GSU::op_load(unsigned n) -> void {  // LDW (rN) / LDB (rN)
  last_ram_word_ = r_[n];
  to_dst(flag(kAlt1) ? ram_load(last_ram_word_) : ram_load_word(last_ram_word_));
  end_prefix();
}

auto GSU::op_plot_rpix(unsigned) -> void {
  const auto x = static_cast<Byte>(r_[1]);
  const auto y = static_cast<Byte>(r_[2]);
  if (flag(kAlt1)) {
    commit(rpix(x, y));
    return;
  }
  plot(x, y);
  write_reg(1, static_cast<Word>(r_[1] + 1));
  end_prefix();
}

auto GSU::op_swap(unsigned) -> void {
  const Word value = src();
  commit(static_cast<Word>((value << 8) | (value >> 8)));
}

auto GSU::op_color_cmode(unsigned) -> void {
  if (flag(kAlt1)) {
    por_ = static_cast<Byte>(src() & 0x1F);  // CMODE
  } else {
    colr_ = color(static_cast<Byte>(src()));  // COLOR
  }
  end_prefix();
}

auto GSU::op_not(unsigned) -> void { commit(static_cast<Word>(~src())); }

auto GSU::op_add_adc(unsigned n) -> void {  // ADD/ADC with rN, or #n under ALT2
  const unsigned a = src(), b = immediate_or_reg(n);
  const unsigned carry_in = (flag(kAlt1) && flag(kCy)) ? 1 : 0;
  const unsigned sum = a + b + carry_in;
  set_flag(kCy, sum > 0xFFFF);
  set_flag(kOv, ((a ^ sum) & (b ^ sum) & 0x8000) != 0);
  set_flag(kS, (sum & 0x8000) != 0);
  set_flag(kZ, (sum & 0xFFFF) == 0);
  to_dst(static_cast<Word>(sum));
  end_prefix();
}

// SUB rN, SBC rN (ALT1), SUB #n (ALT2), CMP rN (ALT3: flags only).
auto GSU::op_sub_sbc_cmp(unsigned n) -> void {
  const bool alt1 = flag(kAlt1), alt2 = flag(kAlt2);
  const int a = src();
  const int b = (alt2 && !alt1) ? static_cast<int>(n) : r_[n];
  const int borrow_in = (alt1 && !alt2 && !flag(kCy)) ? 1 : 0;
  const int diff = a - b - borrow_in;
  set_flag(kCy, diff >= 0);  // carry = no borrow
  set_flag(kOv, ((a ^ b) & (a ^ diff) & 0x8000) != 0);
  set_flag(kS, (diff & 0x8000) != 0);
  set_flag(kZ, (diff & 0xFFFF) == 0);
  if (!(alt1 && alt2)) to_dst(static_cast<Word>(diff));
  end_prefix();
}

// MERGE: high bytes of R7 and R8; the flags test the top bits of both bytes.
auto GSU::op_merge(unsigned) -> void {
  const Word merged = static_cast<Word>((r_[7] & 0xFF00) | (r_[8] >> 8));
  to_dst(merged);
  set_flag(kS, (merged & 0x8080) != 0);
  set_flag(kOv, (merged & 0xC0C0) != 0);
  set_flag(kCy, (merged & 0xE0E0) != 0);
  set_flag(kZ, (merged & 0xF0F0) != 0);
  end_prefix();
}

auto GSU::op_and_bic(unsigned n) -> void {
  Word mask = immediate_or_reg(n);
  if (flag(kAlt1)) mask = static_cast<Word>(~mask);  // BIC
  commit(static_cast<Word>(src() & mask));
}

auto GSU::op_mult_umult(unsigned n) -> void {  // 8 x 8 -> 16, signed (MULT) or unsigned (UMULT)
  const Word a = src(), b = immediate_or_reg(n);
  const int product = flag(kAlt1) ? (a & 0xFF) * (b & 0xFF)
                                  : static_cast<int8_t>(a) * static_cast<int8_t>(b);
  commit(static_cast<Word>(product));
  if ((cfgr_ & 0x20) == 0) spend(cycle(2, 1));  // CFGR.MS0 clear: slow multiplier
}

auto GSU::op_sbk(unsigned) -> void {
  ram_store_word(last_ram_word_, src());
  end_prefix();
}

auto GSU::op_link(unsigned n) -> void {
  write_reg(11, static_cast<Word>(r_[15] + n));
  end_prefix();
}

auto GSU::op_sex(unsigned) -> void { commit(static_cast<Word>(static_cast<int8_t>(src() & 0xFF))); }

// ASR, or DIV2 (ALT1), which rounds -1 to 0.
auto GSU::op_asr_div2(unsigned) -> void {
  const Word value = src();
  set_flag(kCy, value & 1);
  Word result = static_cast<Word>(static_cast<int16_t>(value) >> 1);
  if (flag(kAlt1) && value == 0xFFFF) result = 0;
  commit(result);
}

auto GSU::op_ror(unsigned) -> void {
  const Word value = src();
  const Word result = static_cast<Word>((value >> 1) | (flag(kCy) ? 0x8000 : 0));
  set_flag(kCy, value & 1);
  commit(result);
}

auto GSU::op_jmp_ljmp(unsigned n) -> void {
  if (flag(kAlt1)) {  // LJMP: PBR from rN, R15 from the source; the cache restarts there
    pbr_ = static_cast<Byte>(r_[n] & 0x7F);
    write_reg(15, src());
    cbr_ = static_cast<Word>(src() & 0xFFF0);
    invalidate_cache();
  } else {
    write_reg(15, r_[n]);
  }
  end_prefix();
}

auto GSU::op_lob(unsigned) -> void {
  const Word low = static_cast<Word>(src() & 0x00FF);
  to_dst(low);
  set_flag(kZ, low == 0);
  set_flag(kS, (low & 0x80) != 0);
  end_prefix();
}

// FMULT: (source * R6) >> 16, signed; LMULT (ALT1) also leaves the low word in R4.
auto GSU::op_fmult_lmult(unsigned) -> void {
  const int32_t product = static_cast<int16_t>(src()) * static_cast<int16_t>(r_[6]);
  const auto bits = static_cast<uint32_t>(product);
  if (flag(kAlt1)) write_reg(4, static_cast<Word>(bits));
  const auto high = static_cast<Word>(bits >> 16);
  to_dst(high);
  set_flag(kZ, high == 0);
  set_flag(kS, (high & 0x8000) != 0);
  set_flag(kCy, (bits & 0x8000) != 0);
  end_prefix();
  spend(((cfgr_ & 0x20) ? 3u : 7u) * cycle(2, 1));
}

// IBT rN,#pp (sign-extended), LMS rN,(yy*2) under ALT1, SMS (yy*2),rN under ALT2.
auto GSU::op_ibt_lms_sms(unsigned n) -> void {
  const bool alt1 = flag(kAlt1), alt2 = flag(kAlt2);
  const Byte operand = fetch_operand();
  if (alt1) {
    last_ram_word_ = static_cast<Word>(operand * 2);
    write_reg(n, ram_load_word(last_ram_word_));
  } else if (alt2) {
    last_ram_word_ = static_cast<Word>(operand * 2);
    ram_store_word(last_ram_word_, r_[n]);
  } else {
    write_reg(n, static_cast<Word>(static_cast<int8_t>(operand)));
  }
  end_prefix();
}

auto GSU::op_from_moves(unsigned n) -> void {
  if (!flag(kB)) {
    src_ = n;  // FROM
    return;
  }
  const Word value = r_[n];  // MOVES: copy with flags
  to_dst(value);
  set_sz(value);
  set_flag(kOv, (value & 0x0080) != 0);
  end_prefix();
}

auto GSU::op_hib(unsigned) -> void {
  const Word high = static_cast<Word>(src() >> 8);
  to_dst(high);
  set_flag(kZ, high == 0);
  set_flag(kS, (high & 0x80) != 0);
  end_prefix();
}

auto GSU::op_or_xor(unsigned n) -> void {
  const Word b = immediate_or_reg(n);
  commit(static_cast<Word>(flag(kAlt1) ? (src() ^ b) : (src() | b)));
}

auto GSU::op_inc(unsigned n) -> void {
  const Word value = static_cast<Word>(r_[n] + 1);
  write_reg(n, value);
  set_sz(value);
  end_prefix();
}

auto GSU::op_dec(unsigned n) -> void {
  const Word value = static_cast<Word>(r_[n] - 1);
  write_reg(n, value);
  set_sz(value);
  end_prefix();
}

// $DF: GETC (COLOR from the ROM port), RAMB (ALT2), ROMB (ALT3).
auto GSU::op_getc_ramb_romb(unsigned) -> void {
  if (flag(kAlt2) && flag(kAlt1)) {
    wait_rom_port();
    rombr_ = static_cast<Byte>(src() & 0x7F);
  } else if (flag(kAlt2)) {
    wait_ram_port();
    rambr_ = (src() & 1) != 0;
  } else {
    colr_ = color(rom_port_byte());
  }
  end_prefix();
}

// $EF: GETB, GETBH (ALT1), GETBL (ALT2), GETBS (ALT3). No flags.
auto GSU::op_getb(unsigned) -> void {
  const Word byte = rom_port_byte();
  Word value = byte;
  if (flag(kAlt1) && flag(kAlt2)) {
    value = static_cast<Word>(static_cast<int8_t>(byte));
  } else if (flag(kAlt1)) {
    value = static_cast<Word>((byte << 8) | (src() & 0x00FF));
  } else if (flag(kAlt2)) {
    value = static_cast<Word>((src() & 0xFF00) | byte);
  }
  to_dst(value);
  end_prefix();
}

// IWT rN,#xxxx, LM rN,(xxxx) under ALT1, SM (xxxx),rN under ALT2. Operands are little-endian.
auto GSU::op_iwt_lm_sm(unsigned n) -> void {
  const bool alt1 = flag(kAlt1), alt2 = flag(kAlt2);
  const Byte low = fetch_operand();
  const Word operand = static_cast<Word>((fetch_operand() << 8) | low);
  if (alt1) {
    last_ram_word_ = operand;
    write_reg(n, ram_load_word(operand));
  } else if (alt2) {
    last_ram_word_ = operand;
    ram_store_word(operand, r_[n]);
  } else {
    write_reg(n, operand);
  }
  end_prefix();
}

// ---- save state --------------------------------------------------------------------------------
// Field order is part of the savestate format (version 4+).

auto GSU::serialize(Archive& ar) -> void {
  ar(r_, sfr_, pbr_, rombr_, rambr_, cbr_, scbr_, scmr_, colr_, por_, bramr_, vcr_, cfgr_, clsr_);
  ar(prefetch_, last_ram_word_, src_, dst_, r14_written_, r15_written_);
  ar(rom_port_.busy, rom_port_.data, ram_port_.busy, ram_port_.address, ram_port_.data);
  ar(code_cache_, line_loaded_, plot_now_, plot_prev_, clock_budget_);
}

} // namespace bitforge::coprocessor
