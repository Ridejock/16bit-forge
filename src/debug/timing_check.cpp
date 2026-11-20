// SPDX-License-Identifier: MIT
#include "debug/timing_check.hpp"

#include "snes/memory/bus.hpp"
#include "snes/ppu/ppu.hpp"
#include "snes/system.hpp"
#include "snes/timing.hpp"
#include "speedrun/splits.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>

namespace bitforge::debug {

namespace {

constexpr std::array<SourceInfo, 4> kSources = {{
    {Source::Wiki, "W", "nesdev wiki, Timing", "https://snes.nesdev.org/wiki/Timing"},
    {Source::Fullsnes, "F", "fullsnes, Timing H/V Counters and H/V Events",
     "https://problemkaputt.de/fullsnes.htm#snestiminghvevents"},
    {Source::Anomie, "A", "Anomie's timing.txt (mirror)", "https://raw.githubusercontent.com/gilligan/snesdev/master/docs/timing.txt"},
    {Source::Derived, "D", "derived from the values above", ""},
}};

// The PPU and bus with nothing else: no CPU, so the bus only moves when a check pokes it.
struct Rig {
  memory::Bus bus;
  ppu::PPU ppu;

  Rig() {
    bus.connect_ppu(&ppu);
    ppu.connect_bus(&bus);
    bus.load_test_rom(std::vector<Byte>(0x8000, 0xEA));
    bus.reset();
    ppu.reset();
  }
  auto tick() -> bool { return ppu.step(1); }
  auto at(int v, int c) const -> bool { return ppu.vcounter() == v && ppu.hclock() == c; }
  auto run_to(int v, int c) -> void {
    while (!at(v, c)) tick();
  }
  auto frame() -> int64_t {  // master clocks to the end of this frame
    int64_t n = 1;
    while (!tick()) n++;
    return n;
  }
  auto line() -> int64_t {  // master clocks to the start of the next line
    const int v = ppu.vcounter();
    int64_t n = 0;
    while (ppu.vcounter() == v) {
      tick();
      n++;
    }
    return n;
  }
  // Master clocks the bus has charged the CPU since the last call (DMA, HDMA, accesses).
  auto charged() -> int64_t {
    bus.begin_cpu_step();
    return static_cast<int64_t>(bus.end_cpu_step(0));
  }
  // Ticks until `seen` holds; the beam position (clocks into line `v`) where it first does.
  auto first(int v, const std::function<bool()>& seen) -> int64_t {
    for (int64_t n = 0; n < 2 * timing::kFramePairClocks; n++) {
      if (seen()) return (ppu.vcounter() - v) * int64_t{timing::kLineClocks} + ppu.hclock();
      tick();
    }
    return -1;
  }
  auto write(Address a, Byte v) -> void { bus.write(a, v); }
};

auto frame_clocks(int field) -> double {
  Rig r;
  int64_t n = r.frame();
  if (field == 1) n = r.frame();
  return static_cast<double>(n);
}

auto short_line() -> double {
  Rig r;
  r.frame();  // to the field-1 frame
  r.run_to(timing::kShortLine, 0);
  return static_cast<double>(r.line());
}

auto normal_line() -> double {
  Rig r;
  r.run_to(10, 0);
  return static_cast<double>(r.line());
}

auto interlace_pair() -> double {
  Rig r;
  r.write(0x2133, 0x01);  // SETINI: interlace
  const int64_t a = r.frame();
  return static_cast<double>(a + r.frame());
}

auto frame_rate() -> double {
  Rig r;
  const int64_t pair = r.frame() + r.frame();
  return timing::kMasterClockHz * 2.0 / static_cast<double>(pair);
}

// The whole machine on the probe program: master clocks per frame over 600 frames.
auto cpu_frame_rate() -> double {
  System sys;
  sys.cartridge().load_raw(make_probe_rom());
  sys.reset();
  sys.run_frame();
  const uint64_t start = sys.master_clocks();
  constexpr int kFrames = 600;
  for (int i = 0; i < kFrames; i++) sys.run_frame();
  const double per_frame = static_cast<double>(sys.master_clocks() - start) / kFrames;
  return timing::kMasterClockHz / per_frame;
}

auto vblank_line() -> double {
  Rig r;
  r.write(0x4200, 0x80);
  return static_cast<double>(r.first(0, [&] { return r.ppu.in_vblank(); }) / timing::kLineClocks);
}

auto nmi_clock() -> double {
  Rig r;
  r.write(0x4200, 0x80);
  r.run_to(224, 0);
  return static_cast<double>(r.first(225, [&] { return r.bus.poll_nmi(); }));
}

constexpr int kVtime = 100;
constexpr int kHtime = 100;

auto virq_clock() -> double {
  Rig r;
  r.write(0x4209, kVtime);
  r.write(0x420A, 0);
  r.write(0x4200, 0x20);
  r.run_to(kVtime - 1, 0);
  return static_cast<double>(r.first(kVtime, [&] { return r.bus.irq_pending(); }));
}

auto hirq_clock() -> double {
  Rig r;
  r.write(0x4207, kHtime);
  r.write(0x4208, 0);
  r.write(0x4200, 0x10);
  r.run_to(50, 0);
  r.bus.read(0x4211);  // acknowledge the IRQs of the lines before
  return static_cast<double>(r.first(50, [&] { return r.bus.irq_pending(); }) - kHtime * 4);
}

// Master clocks per CPU access at `address`, with MEMSEL ($420D) set to `memsel`.
auto access(Address address, Byte memsel) -> double {
  Rig r;
  r.write(0x420D, memsel);
  return static_cast<double>(r.bus.access_clocks(address));
}

auto joypad_busy(Rig& r) -> bool { return (r.bus.read(0x4212) & 0x01) != 0; }

auto autojoy_start() -> double {
  Rig r;
  r.write(0x4200, 0x01);
  r.run_to(225, 0);
  return static_cast<double>(r.first(225, [&] { return joypad_busy(r); }));
}

auto autojoy_clocks() -> double {
  Rig r;
  r.write(0x4200, 0x01);
  r.run_to(225, 0);
  const int64_t start = r.first(225, [&] { return joypad_busy(r); });
  return static_cast<double>(r.first(225, [&] { return !joypad_busy(r); }) - start);
}

auto refresh_start() -> double {
  for (uint64_t c = 1; c <= static_cast<uint64_t>(timing::kLineClocks); c++) {
    if (memory::dram_refresh_stall(0, c) > 0) return static_cast<double>(c);
  }
  return -1;
}

auto refresh_clocks() -> double {
  const auto start = static_cast<uint64_t>(refresh_start());
  return static_cast<double>(memory::dram_refresh_stall(0, start));
}

// One GP-DMA of `bytes` bytes on each of `channels` channels, WRAM to VRAM, in master clocks
// (less the CPU's write to $420B). Run mid-line, clear of the refresh.
auto dma_clocks(int channels, int bytes) -> int64_t {
  Rig r;
  r.run_to(10, 600);
  for (int ch = 0; ch < channels; ch++) {
    const Address base = 0x4300 + static_cast<Address>(ch) * 0x10;
    r.write(base + 0, 0x01);  // A to B, two registers
    r.write(base + 1, 0x18);  // $2118/$2119
    r.write(base + 2, 0x00);
    r.write(base + 3, 0x00);
    r.write(base + 4, 0x7E);
    r.write(base + 5, static_cast<Byte>(bytes & 0xFF));
    r.write(base + 6, static_cast<Byte>(bytes >> 8));
  }
  r.charged();
  r.bus.begin_cpu_step();
  r.bus.write(0x00420B, static_cast<Byte>((1 << channels) - 1));
  const auto total = static_cast<int64_t>(r.bus.end_cpu_step(1));
  return total - r.bus.access_clocks(0x00420B);
}

auto dma_byte() -> double { return static_cast<double>(dma_clocks(1, 32) - dma_clocks(1, 16)) / 16.0; }
auto dma_channel() -> double { return static_cast<double>(dma_clocks(2, 16) - dma_clocks(1, 16) - 16 * 8); }
auto dma_overhead() -> double { return static_cast<double>(dma_clocks(1, 16) - 16 * 8 - 8); }

// HDMA channel 0 in mode 0 ($2132), from a table at $7E:1000: 127 lines, one byte on the first.
auto hdma_rig(Rig& r, bool indirect) -> void {
  r.bus.poke_wram(0x1000, 0x7F);
  r.bus.poke_wram(0x1001, indirect ? 0x00 : 0xE0);  // the byte, or the indirect address $7E:2000
  r.bus.poke_wram(0x1002, indirect ? 0x20 : 0x00);
  r.bus.poke_wram(0x2000, 0xE0);
  r.write(0x4300, indirect ? 0x40 : 0x00);
  r.write(0x4301, 0x32);
  r.write(0x4302, 0x00);
  r.write(0x4303, 0x10);
  r.write(0x4304, 0x7E);
  r.write(0x4307, 0x7E);
  r.write(0x420C, 0x01);
  r.charged();
}

auto hdma_init(bool indirect) -> double {
  Rig r;
  hdma_rig(r, indirect);
  r.run_to(0, 100);  // past the reload at dot 6 of line 0
  return static_cast<double>(r.charged());
}
auto hdma_init_direct() -> double { return hdma_init(false); }
auto hdma_init_indirect() -> double { return hdma_init(true); }

struct HdmaLine {
  int64_t clock;
  int64_t clocks;
};
auto hdma_line(int line) -> HdmaLine {
  Rig r;
  hdma_rig(r, false);
  r.run_to(line, 1000);
  r.charged();
  while (true) {
    r.tick();
    if (const int64_t n = r.charged(); n > 0) return {r.ppu.hclock(), n};
  }
}
auto hdma_line_transfer() -> double { return static_cast<double>(hdma_line(0).clocks); }
auto hdma_line_idle() -> double { return static_cast<double>(hdma_line(1).clocks); }
auto hdma_line_clock() -> double { return static_cast<double>(hdma_line(1).clock); }

// Game time for an hour of frames against the exact period (357366 clocks / 945/44 MHz).
auto game_time_hour() -> double {
  constexpr int64_t kFrames = 216'000;
  return static_cast<double>(speedrun::frames_to_nanos(kFrames, speedrun::VideoStandard::Ntsc));
}
constexpr double kHourExactNs = 216'000.0 * 357'366.0 * 44'000.0 / 945.0;

struct Spec {
  TimingCheck check;
  double (*measure)();
};

// clang-format off
const std::array<Spec, 31> kSpecs = {{
    {{"line_clocks", "Line length", "clocks", 1364, 1364, 0, Source::Wiki, {}}, normal_line},
    {{"short_line_clocks", "Short line (V=240, field 1, interlace off)", "clocks", 1360, 1360, 0, Source::Wiki, {}}, short_line},
    {{"frame_clocks_0", "Frame, field 0 (262 lines)", "clocks", 357368, 357368, 0, Source::Derived, {}}, [] { return frame_clocks(0); }},
    {{"frame_clocks_1", "Frame, field 1 (with the short line)", "clocks", 357364, 357364, 0, Source::Derived, {}}, [] { return frame_clocks(1); }},
    {{"interlace_pair_clocks", "Two interlaced frames (263 + 262 lines)", "clocks", 716100, 716100, 0, Source::Fullsnes, {}}, interlace_pair},
    {{"frame_rate", "Frame rate (PPU)", "Hz", timing::kNtscFrameRate, timing::kNtscFrameRate, 1e-9, Source::Fullsnes, {}}, frame_rate},
    {{"frame_rate_cpu", "Frame rate, whole machine over 600 frames", "Hz", timing::kNtscFrameRate, timing::kNtscFrameRate, 1e-4, Source::Fullsnes,
      "averaged over whole instructions: a frame ends after the instruction that crosses it"}, cpu_frame_rate},
    {{"game_time_hour_ns", "Game time of 216000 frames (1 h)", "ns", kHourExactNs, kHourExactNs, 1, Source::Derived, {}}, game_time_hour},
    {{"vblank_line", "V-blank starts", "line", 225, 225, 0, Source::Fullsnes, {}}, vblank_line},
    {{"nmi_clock", "NMI flag set, clocks into line 225", "clocks", 2, 2, 2, Source::Anomie,
      "set at the line's start (H=0); hardware sets it at H=0.5 dot"}, nmi_clock},
    {{"virq_clock", "V-IRQ, clocks into line VTIME", "clocks", 10, 10, 9, Source::Anomie,
      "fires at H=0; hardware fires ~2.5 dots in (1374 clocks after dot 0 of the line before)"}, virq_clock},
    {{"hirq_clock", "H-IRQ, clocks after HTIME*4", "clocks", 14, 14, 13, Source::Anomie,
      "fires at HTIME*4; hardware fires ~3.5 dots later (14 + HTIME*4), long dots aside"}, hirq_clock},
    {{"autojoy_start", "Auto-joypad read starts, clocks into line 225", "clocks", 130, 382, 0, Source::Anomie,
      "fixed at the earliest point; hardware moves within dots 32.5-95.5 from frame to frame"}, autojoy_start},
    {{"autojoy_clocks", "Auto-joypad read length", "clocks", 4224, 4224, 0, Source::Anomie,
      "the pad latches fill at its start rather than bit by bit"}, autojoy_clocks},
    {{"refresh_start", "WRAM refresh starts, clocks into the line", "clocks", 538, 538, 4, Source::Anomie,
      "538 on every line; hardware: 538 on the first, then the multiple of 8 nearest 536"}, refresh_start},
    {{"refresh_clocks", "WRAM refresh length", "clocks", 40, 40, 0, Source::Wiki, {}}, refresh_clocks},
    {{"dma_byte_clocks", "DMA per byte", "clocks", 8, 8, 0, Source::Anomie, {}}, dma_byte},
    {{"dma_channel_clocks", "DMA per channel", "clocks", 8, 8, 0, Source::Anomie, {}}, dma_channel},
    {{"dma_overhead_clocks", "DMA per transfer (start, alignment, resume)", "clocks", 12, 24, 4, Source::Anomie,
      "8 flat; hardware adds 2-8 to align to 8 clocks and 2-8 to resync with the CPU clock"}, dma_overhead},
    {{"hdma_init_direct_clocks", "HDMA table reload, one direct channel", "clocks", 26, 26, 0, Source::Anomie, {}}, hdma_init_direct},
    {{"hdma_init_indirect_clocks", "HDMA table reload, one indirect channel", "clocks", 42, 42, 0, Source::Anomie, {}}, hdma_init_indirect},
    {{"hdma_line_clocks", "HDMA line, one channel, one byte", "clocks", 34, 34, 0, Source::Anomie, {}}, hdma_line_transfer},
    {{"hdma_line_idle_clocks", "HDMA line, one channel, no transfer", "clocks", 26, 26, 0, Source::Anomie, {}}, hdma_line_idle},
    {{"hdma_line_start", "HDMA line transfer starts, clocks into the line", "clocks", 1112, 1112, 16, Source::Anomie,
      "runs at dot 274 (the H-blank flag); hardware starts it at dot 278"}, hdma_line_clock},
    {{"access_wram_clocks", "CPU access: WRAM", "clocks", 8, 8, 0, Source::Wiki, {}}, [] { return access(0x7E0000, 0); }},
    {{"access_slowrom_clocks", "CPU access: ROM, MEMSEL off", "clocks", 8, 8, 0, Source::Wiki, {}}, [] { return access(0x808000, 0); }},
    {{"access_fastrom_clocks", "CPU access: ROM at $80:8000+, MEMSEL on", "clocks", 6, 6, 0, Source::Wiki, {}}, [] { return access(0x808000, 1); }},
    {{"access_io_clocks", "CPU access: I/O registers ($21xx, $42xx)", "clocks", 6, 6, 0, Source::Wiki, {}}, [] { return access(0x004200, 0); }},
    {{"access_joyser_clocks", "CPU access: $4016/$4017", "clocks", 12, 12, 0, Source::Wiki, {}}, [] { return access(0x004016, 0); }},
    {{"master_clock_hz", "Master clock", "Hz", 236250000.0 / 11.0, 236250000.0 / 11.0, 0, Source::Wiki, {}}, [] { return timing::kMasterClockHz; }},
    {{"cpu_clocks_per_line", "CPU clocks per line (less the refresh)", "clocks", 1324, 1324, 0, Source::Wiki, {}},
     [] { return static_cast<double>(timing::kLineClocks) - refresh_clocks(); }},
}};
// clang-format on

auto number(double v, std::string_view unit) -> std::string {
  char text[48];
  if (unit == "Hz") std::snprintf(text, sizeof(text), "%.7f", v);
  else std::snprintf(text, sizeof(text), "%.0f", v);
  return text;
}

constexpr std::array<std::string_view, 3> kVerdicts = {"exact", "close", "OFF"};

}  // namespace

auto info(Source s) -> const SourceInfo& { return kSources[static_cast<size_t>(s)]; }
auto sources() -> std::span<const SourceInfo> { return kSources; }

auto TimingCheck::delta() const -> double {
  if (measured < low) return measured - low;
  if (measured > high) return measured - high;
  return 0;
}

auto TimingCheck::verdict() const -> Verdict {
  const double d = std::fabs(delta());
  const double exact = unit == "Hz" ? 1e-9 : 0.5;  // whole clocks; rates to rounding
  if (d <= exact) return Verdict::Exact;
  return d <= tolerance ? Verdict::Close : Verdict::Off;
}

auto run_timing_checks() -> std::vector<TimingCheck> {
  std::vector<TimingCheck> out;
  out.reserve(kSpecs.size());
  for (const Spec& s : kSpecs) {
    TimingCheck c = s.check;
    c.measured = s.measure();
    out.push_back(c);
  }
  return out;
}

auto checks_off(std::span<const TimingCheck> checks) -> int {
  return static_cast<int>(std::count_if(checks.begin(), checks.end(), [](const TimingCheck& c) { return c.verdict() == Verdict::Off; }));
}

auto format_timing_checks(std::span<const TimingCheck> checks) -> std::string {
  std::string out;
  char line[256];
  std::snprintf(line, sizeof(line), "%-58s %17s %25s %8s  %-6s %s\n", "Check", "Measured", "Documented", "Delta", "Result", "Src");
  out += line;
  out += std::string(122, '-') + "\n";
  for (const TimingCheck& c : checks) {
    std::string doc = number(c.low, c.unit);
    if (c.high != c.low) doc += " - " + number(c.high, c.unit);
    const std::string label = std::string(c.label) + " (" + std::string(c.unit) + ")";
    std::snprintf(line, sizeof(line), "%-58s %17s %25s %8s  %-6s %s\n", label.c_str(), number(c.measured, c.unit).c_str(), doc.c_str(),
                  c.verdict() == Verdict::Exact ? "0" : (c.unit == "Hz" ? number(c.delta(), c.unit) : (c.delta() > 0 ? "+" : "") + number(c.delta(), c.unit)).c_str(),
                  kVerdicts[static_cast<size_t>(c.verdict())].data(), info(c.source).tag.data());
    out += line;
  }
  out += "\nexact: matches the documented value (or falls in its range).\n"
         "close: a known gap, listed below; the tests hold it to that size.\n"
         "OFF:   further from the documented value than the known gap.\n\nKnown gaps:\n";
  for (const TimingCheck& c : checks) {
    if (c.verdict() == Verdict::Exact || c.note.empty()) continue;
    out += "  " + std::string(c.label) + ": " + std::string(c.note) + ".\n";
  }
  out += "\nSources:\n";
  for (const SourceInfo& s : kSources) {
    out += "  [" + std::string(s.tag) + "] " + std::string(s.title);
    if (!s.url.empty()) out += "  " + std::string(s.url);
    out += "\n";
  }
  return out;
}

auto make_probe_rom(uint8_t mask, ProbeRead read) -> std::vector<uint8_t> {
  std::vector<uint8_t> rom(0x8000, 0xFF);
  const std::array<uint8_t, 32> reset = {
      0x78,              // sei
      0x18, 0xFB,        // clc / xce: native mode
      0xE2, 0x30,        // sep #$30
      0x9C, 0x00, 0x00,  // stz $0000: frame counter
      0x9C, 0x21, 0x21,  // stz $2121
      0xA9, 0x1F,        // lda #$1F
      0x8D, 0x22, 0x21,  // sta $2122: a red backdrop
      0x9C, 0x22, 0x21,  // stz $2122
      0xA9, 0x0F,        // lda #$0F
      0x8D, 0x00, 0x21,  // sta $2100: screen on
      0xA9, 0x81,        // lda #$81
      0x8D, 0x00, 0x42,  // sta $4200: NMI + auto-joypad read
      0xCB,              // wai
      0x80, 0xFD,        // bra wai
  };
  std::array<uint8_t, 3> load = {0xAD, 0x18, 0x42};             // lda $4218
  if (read == ProbeRead::Serial) load = {0xAD, 0x16, 0x40};     // lda $4016
  if (read == ProbeRead::None) load = {0xEA, 0xEA, 0xEA};       // nop x3
  const std::array<uint8_t, 18> nmi = {
      0x48,                          // pha
      0xAD, 0x10, 0x42,              // lda $4210: acknowledge
      0xEE, 0x00, 0x00,              // inc $0000
      0xAD, 0x00, 0x00,              // lda $0000
      0x29, mask,                    // and #mask
      0xD0, 0x03,                    // bne past the read
      load[0], load[1], load[2],
      0x68,                          // pla
  };
  std::copy(reset.begin(), reset.end(), rom.begin());
  std::copy(nmi.begin(), nmi.end(), rom.begin() + 0x100);
  rom[0x100 + nmi.size()] = 0x40;    // rti
  const std::string_view title = "TIMING PROBE         ";
  std::copy(title.begin(), title.end(), rom.begin() + 0x7FC0);
  rom[0x7FD5] = 0x20;  // LoROM, SlowROM
  rom[0x7FD6] = 0x00;
  rom[0x7FD7] = 0x05;  // 32 KB
  rom[0x7FD8] = 0x00;
  rom[0x7FD9] = 0x01;
  auto put16 = [&](size_t at, uint16_t v) {
    rom[at] = static_cast<uint8_t>(v & 0xFF);
    rom[at + 1] = static_cast<uint8_t>(v >> 8);
  };
  put16(0x7FEA, 0x8100);  // native NMI
  put16(0x7FFA, 0x8100);  // emulation NMI
  put16(0x7FFC, 0x8000);  // reset
  put16(0x7FDC, 0xFFFF);
  put16(0x7FDE, 0x0000);
  uint16_t sum = 0;
  for (uint8_t b : rom) sum = static_cast<uint16_t>(sum + b);
  put16(0x7FDC, static_cast<uint16_t>(~sum));
  put16(0x7FDE, sum);
  return rom;
}

}  // namespace bitforge::debug
