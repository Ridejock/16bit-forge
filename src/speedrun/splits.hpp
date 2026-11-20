// SPDX-License-Identifier: MIT
#pragma once
// A splits file's contents: the game and category, the segments with their personal best (PB)
// split times and best segment ("gold") times, and the attempt history. Times are nanoseconds,
// kept for both timing methods. The layout follows LiveSplit's .lss (see lss.hpp), so a file
// round-trips; the auto-split rules are ours and live in a sidecar file (autosplit.hpp).
// No SDL or ImGui: the frontend shows it, the tests drive it.
#include "debug/memory_watch.hpp"
#include "snes/timing.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bitforge::speedrun {

using Nanos = int64_t;
inline constexpr Nanos kNanosPerSecond = 1'000'000'000;

// Real time is the wall clock (it runs on through pauses and menus); game time counts emulated
// frames at the console's frame rate, so pausing, fast-forward and rewind don't distort it.
enum class Method : uint8_t { Real, Game };
struct MethodInfo {
  Method id;
  std::string_view key;    // settings and the .lss element name
  std::string_view label;  // UI
  std::string_view short_label;
};
inline constexpr std::array<MethodInfo, 2> kMethods = {{
    {Method::Real, "RealTime", "Real time", "RTA"},
    {Method::Game, "GameTime", "Game time", "IGT"},
}};
inline auto info(Method m) -> const MethodInfo& { return kMethods[static_cast<size_t>(m)]; }

// The console's video standard: how many frames a second of game time holds.
enum class VideoStandard : uint8_t { Ntsc, Pal };
struct VideoStandardInfo {
  VideoStandard id;
  std::string_view label;
  double frame_rate;  // master clock / clocks per (non-interlaced) frame
};
inline constexpr std::array<VideoStandardInfo, 2> kVideoStandards = {{
    {VideoStandard::Ntsc, "NTSC", timing::kNtscFrameRate},                      // 60.0988 Hz (every other frame has a line 4 clocks short)
    {VideoStandard::Pal, "PAL", 21281370.0 / (312.0 * 1364.0)},                  // 50.0070 Hz
}};
inline auto info(VideoStandard v) -> const VideoStandardInfo& { return kVideoStandards[static_cast<size_t>(v)]; }
// `frames` of game time at `standard`'s rate.
auto frames_to_nanos(int64_t frames, VideoStandard standard) -> Nanos;

// A moment or a duration in both methods; either may be missing (not reached, skipped).
struct Times {
  std::optional<Nanos> real;
  std::optional<Nanos> game;
  auto get(Method m) const -> std::optional<Nanos> { return m == Method::Real ? real : game; }
  auto set(Method m, std::optional<Nanos> t) -> void { (m == Method::Real ? real : game) = t; }
  auto empty() const -> bool { return !real && !game; }
  auto operator==(const Times&) const -> bool = default;
};

// An auto-split (or auto-start / auto-reset) rule: a condition on a memory watch, with the
// watch's definition so a splits file can be moved to another setup (the watch is found by its
// address, or added, when the file is loaded).
struct Rule {
  debug::Condition condition;
  debug::Watch watch;  // id == condition.watch
  auto operator==(const Rule&) const -> bool = default;
};

struct HistoryTime {  // one attempt's time for one segment (LiveSplit's SegmentHistory)
  int attempt = 0;
  Times time;
  auto operator==(const HistoryTime&) const -> bool = default;
};

struct Segment {
  std::string name;
  Times pb;    // the PB run's split time (cumulative)
  Times best;  // the best time for this segment alone (gold)
  std::vector<HistoryTime> history;
  std::optional<Rule> auto_split;
  int source = -1;  // its <Segment> in the file it was read from (keeps icons etc.), -1 = new
  auto operator==(const Segment&) const -> bool = default;
};

struct Attempt {
  int id = 0;
  Times time;           // the final time; empty when it was reset before the end
  std::string started;  // "MM/dd/yyyy HH:mm:ss" UTC, as LiveSplit writes it ("" = unknown)
  std::string ended;
  bool race_verified = false;  // finished in race mode, untouched (kept in the .race sidecar, see race.hpp)
  auto finished() const -> bool { return !time.empty(); }
  auto operator==(const Attempt&) const -> bool = default;
};

struct Run {
  std::string game;
  std::string category;
  Nanos offset = 0;  // the timer starts here (negative: a countdown before the run)
  int attempt_count = 0;
  std::vector<Attempt> attempts;
  std::vector<Segment> segments;
  std::optional<Rule> auto_start;
  std::optional<Rule> auto_reset;
  auto operator==(const Run&) const -> bool = default;
};

auto completed_runs(const Run& run) -> int;
// The sum of every segment's best time; nullopt while a segment has none.
auto sum_of_best(const Run& run, Method m) -> std::optional<Nanos>;
// The PB's final time (the last segment's PB split).
auto pb_time(const Run& run, Method m) -> std::optional<Nanos>;
// Segment `i`'s PB duration (its PB split minus the one before it).
auto pb_segment(const Run& run, size_t i, Method m) -> std::optional<Nanos>;
auto next_attempt_id(const Run& run) -> int;
// A new run with `count` segments named "Split 1".."Split N".
auto make_run(std::string game, std::string category, size_t count) -> Run;

// ---- Text -----------------------------------------------------------------------------------
// Timer display: "1:23:45.67", "3:05.12", "9.87", "-5.00" (centiseconds, truncated so it never
// shows a time not yet reached). `decimals` 0..3.
auto format_time(Nanos t, int decimals = 2) -> std::string;
// A delta: "+1.23", "-0.45", "+1:02.3" (one decimal past a minute). Always signed.
auto format_delta(Nanos d) -> std::string;
// LiveSplit's TimeSpan text: "01:23:45.6700000", "-00:00:05", "1.02:03:04.5000000" (days).
auto format_timespan(Nanos t) -> std::string;
auto parse_timespan(std::string_view text) -> std::optional<Nanos>;

}  // namespace bitforge::speedrun
