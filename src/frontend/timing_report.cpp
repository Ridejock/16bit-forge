// SPDX-License-Identifier: MIT
#include "frontend/timing_report.hpp"

#include "debug/timing_check.hpp"
#include "frontend/emu_thread.hpp"
#include "frontend/emulator.hpp"
#include "snes/timing.hpp"
#include "speedrun/movie.hpp"
#include "speedrun/run_timer.hpp"
#include "speedrun/splits.hpp"
#include "utils/logging.hpp"

#include <array>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace bitforge::frontend {

namespace {

namespace fs = std::filesystem;

// The input script: Start to leave the title, then walk right, jumping (B) and attacking (Y)
// on a beat. The same every run, so any difference between runs is the emulator's.
struct Press {
  uint64_t from;
  uint64_t period;  // 0: held from `from` on
  uint64_t length;
  uint16_t buttons;  // $4218 order: B Y Select Start Up Down Left Right A X L R
};
constexpr uint16_t kB = 0x8000, kY = 0x4000, kStart = 0x1000, kRight = 0x0100;
constexpr std::array<Press, 4> kScript = {{
    {120, 0, 6, kStart},
    {200, 0, 0, kRight},
    {300, 90, 12, kB},
    {330, 150, 4, kY},
}};

auto script(uint64_t frame) -> uint16_t {
  uint16_t pad = 0;
  for (const Press& p : kScript) {
    if (frame < p.from) continue;
    const uint64_t into = p.period ? (frame - p.from) % p.period : frame - p.from;
    if (p.length == 0 || into < p.length) pad = static_cast<uint16_t>(pad | p.buttons);
  }
  return pad;
}

// What a run leaves behind: its timing and a fingerprint of the machine.
struct RunResult {
  uint64_t frames = 0;
  uint64_t lag_frames = 0;
  uint64_t clocks = 0;
  std::vector<bool> lag;  // per frame
  uint64_t hash = 0;      // WRAM and the picture at the end
  auto operator==(const RunResult&) const -> bool = default;
};

auto fnv(uint64_t h, std::span<const uint8_t> bytes) -> uint64_t {
  for (uint8_t b : bytes) h = (h ^ b) * 0x100000001B3ull;
  return h;
}

auto fingerprint(Emulator& emu) -> uint64_t {
  uint64_t h = 0xCBF29CE484222325ull;
  h = fnv(h, emu.memory(debug::Region::Wram));
  const auto pixels = static_cast<size_t>(emu.width()) * static_cast<size_t>(emu.height());
  h = fnv(h, {reinterpret_cast<const uint8_t*>(emu.framebuffer()), pixels * sizeof(uint32_t)});
  return h;
}

// Runs from the current frame to `end` with the script; the result covers frames [from, end).
auto play(Emulator& emu, uint64_t end, RunResult& r) -> void {
  while (emu.frame_count() < end) {
    emu.set_joypad(0, script(emu.frame_count()));
    emu.run_frame();
    r.lag.push_back(emu.lagged());
  }
  r.frames = emu.frame_count();
  r.lag_frames = emu.lag_frames();
  r.clocks = emu.master_clocks();
  r.hash = fingerprint(emu);
}

auto lag_count(const std::vector<bool>& lag) -> uint64_t {
  uint64_t n = 0;
  for (bool l : lag) n += l ? 1 : 0;
  return n;
}

auto line(std::string& out, const char* fmt, ...) -> void {
  char text[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  out += text;
  out += '\n';
}

auto same(bool ok) -> const char* { return ok ? "same" : "DIFFERENT"; }

// The game section; false if a replay differed or the game wouldn't run.
auto game_section(const TimingReportOptions& o, std::string& out) -> bool {
  const fs::path dir = (o.work_dir.empty() ? fs::temp_directory_path() : fs::path(o.work_dir)) / "16bitforge_timing";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const uint64_t n = o.frames;
  const uint64_t half = n / 2;
  bool ok = true;

  RunResult first, second, rewound, reloaded, replayed;
  RunResult first_tail;  // frames [half, n) of the first run, for the jumps to match
  std::string why;
  {
    Emulator emu;
    emu.set_data_dir(dir.string());
    if (!emu.load(o.rom)) {
      line(out, "Could not load %s: %s", o.rom.c_str(), emu.last_error().c_str());
      return false;
    }
    line(out, "Game: %s", fs::path(o.rom).filename().string().c_str());
    line(out, "  CRC32 %08X  SHA-1 %s", emu.rom_crc32(), emu.rom_sha1_hex().c_str());
    play(emu, half, first);
    const std::vector<uint8_t> snapshot = emu.save_snapshot();
    why = emu.save_state(1);
    RunResult tail;
    play(emu, n, tail);
    first.lag.insert(first.lag.end(), tail.lag.begin(), tail.lag.end());
    first.frames = tail.frames;
    first.lag_frames = tail.lag_frames;
    first.clocks = tail.clocks;
    first.hash = tail.hash;
    first_tail = tail;

    // Rewind: the snapshot the rewind buffer keeps.
    if (!emu.load_snapshot(snapshot)) ok = false;
    play(emu, n, rewound);
    // A save state file.
    if (!why.empty() || !emu.load_state(1).empty()) ok = false;
    play(emu, n, reloaded);
  }
  {
    Emulator emu;
    emu.set_data_dir(dir.string());
    if (emu.load(o.rom)) play(emu, n, second);
  }
  // A movie of the run, recorded from power-on, then played back by a fresh emulator.
  const std::string movie_path = (dir / "timing.bfm").string();
  {
    Emulator emu;
    emu.set_data_dir(dir.string());
    if (emu.load(o.rom) && emu.record_movie(speedrun::MovieStart::PowerOn, movie_path, {o.emulator, "timing report"}).empty()) {
      RunResult recorded;
      play(emu, n, recorded);
      if (!emu.stop_movie().empty()) ok = false;
    } else {
      ok = false;
    }
  }
  {
    Emulator emu;
    emu.set_data_dir(dir.string());
    speedrun::Movie movie;
    if (emu.load(o.rom) && speedrun::read_movie(movie_path, movie).empty() && emu.play_movie(std::move(movie), movie_path).empty()) {
      play(emu, n, replayed);
      emu.stop_movie();  // closed before the folder goes
    } else {
      ok = false;
    }
  }
  fs::remove_all(dir, ec);

  const double seconds = static_cast<double>(speedrun::frames_to_nanos(static_cast<int64_t>(first.frames), speedrun::VideoStandard::Ntsc)) / 1e9;
  const double clocks_per_frame = first.frames ? static_cast<double>(first.clocks) / static_cast<double>(first.frames) : 0.0;
  line(out, "  Scripted input: Start at frame 120, walk right from 200, B every 90 frames from 300, Y every 150 from 330.");
  line(out, "  Frames:            %" PRIu64 " (%.3f s of game time)", first.frames, seconds);
  line(out, "  Lag frames:        %" PRIu64 " (frames in which the game read no controller)", first.lag_frames);
  line(out, "  Master clocks:     %" PRIu64 " (%.2f per frame, %.7f Hz; hardware averages 357366, %.7f Hz)", first.clocks, clocks_per_frame,
       clocks_per_frame > 0 ? timing::kMasterClockHz / clocks_per_frame : 0.0, timing::kNtscFrameRate);

  const bool again = second == first;
  auto tail_matches = [&](const RunResult& r, bool with_counter) {
    const bool lag = r.lag == first_tail.lag && lag_count(r.lag) == lag_count(first_tail.lag);
    return lag && r.hash == first.hash && r.clocks == first.clocks && r.frames == first.frames &&
           (!with_counter || r.lag_frames == first.lag_frames);
  };
  const bool rewind_ok = tail_matches(rewound, true);
  const bool state_ok = tail_matches(reloaded, false);  // a state load leaves the lag counter as it was
  const bool movie_ok = replayed == first;
  line(out, "  Determinism (frames, lag frames frame by frame, master clocks, WRAM and picture at the end):");
  const std::string at = std::to_string(half);
  line(out, "    %-37s %s", "Second run from power-on:", same(again));
  line(out, "    %-37s %s", ("Rewind to frame " + at + " and replay:").c_str(), same(rewind_ok));
  line(out, "    %-37s %s (lag in frames %" PRIu64 "-%" PRIu64 ": %" PRIu64 " vs %" PRIu64 ")",
       ("Save state at frame " + at + " reloaded:").c_str(), same(state_ok), half, n - 1, lag_count(reloaded.lag),
       lag_count(first_tail.lag));
  line(out, "    %-37s %s (%" PRIu64 " frames, %" PRIu64 " lag frames)", "Movie recorded, played back:", same(movie_ok),
       replayed.frames, replayed.lag_frames);
  return ok && again && rewind_ok && state_ok && movie_ok;
}

}  // namespace

auto make_timing_report(const TimingReportOptions& o) -> TimingReport {
  const utils::LogLevel level = utils::Logger::level();
  utils::Logger::set_level(utils::LogLevel::Error);  // the core's per-frame logging stays out of it

  TimingReport report;
  std::string& out = report.text;
  const std::string stamp = speedrun::utc_stamp(static_cast<int64_t>(std::time(nullptr)));
  line(out, "%s timing report, %s UTC", o.emulator.c_str(), stamp.c_str());
  line(out, "NTSC console timing measured in the emulator core, against the documented hardware values.");
  line(out, "");
  line(out, "1. Core timing");
  line(out, "");
  const std::vector<debug::TimingCheck> checks = debug::run_timing_checks();
  out += debug::format_timing_checks(checks);
  const int off = debug::checks_off(checks);

  line(out, "");
  line(out, "2. Game time and pacing");
  line(out, "  Game time (IGT) counts emulated frames, each 357366 master clocks on average (%.10f Hz),", timing::kNtscFrameRate);
  line(out, "  so pausing, slowdown on the PC, fast-forward and rewind don't change it. Real time (RTA) is the PC's clock.");
  const double paced_hz = 1e9 / static_cast<double>(EmuThread::kFrameNs);
  line(out, "  Frame pacing: one frame every %" PRIu64 " ns (%.7f Hz), %+.3f ms per hour against the console.", EmuThread::kFrameNs, paced_hz,
       (1.0 / paced_hz - 1.0 / timing::kNtscFrameRate) * paced_hz * 3600.0 * 1000.0);
  line(out, "  With audio sync on, the sound card's clock paces instead, at the core's 32040 Hz sample rate.");

  bool game_ok = true;
  if (!o.rom.empty()) {
    line(out, "");
    line(out, "3. A game");
    game_ok = game_section(o, out);
  }
  line(out, "");
  line(out, "Result: %d of %zu checks exact or within a known gap%s.", static_cast<int>(checks.size()) - off, checks.size(),
       o.rom.empty() ? "" : (game_ok ? "; every replay matched" : "; a replay DIFFERED"));
  report.ok = off == 0 && game_ok;
  utils::Logger::set_level(level);
  return report;
}

}  // namespace bitforge::frontend
