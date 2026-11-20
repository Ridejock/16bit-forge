// SPDX-License-Identifier: MIT
#pragma once
// Timing checks: the core's timing measured headlessly (the PPU and bus a master clock at a time,
// the whole machine on a tiny built-in program) and set against the values the hardware documents
// give (snes/timing.hpp). The timing_tests suite asserts them; the timing report prints them for
// people who need to know how close the emulator is to the console, e.g. leaderboard moderators.
// No game, ROM file or firmware is needed.
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bitforge::debug {

// Where a documented value comes from (the report lists the links).
enum class Source : uint8_t { Wiki, Fullsnes, Anomie, Derived };
struct SourceInfo {
  Source id;
  std::string_view tag;
  std::string_view title;
  std::string_view url;
};
auto info(Source s) -> const SourceInfo&;
auto sources() -> std::span<const SourceInfo>;

enum class Verdict : uint8_t { Exact, Close, Off };  // Close: a known gap, within `tolerance`

struct TimingCheck {
  std::string_view key;
  std::string_view label;
  std::string_view unit;
  double low = 0;        // documented value, or the low end of a documented range
  double high = 0;       // = low for a single value
  double tolerance = 0;  // the gap accepted beyond [low, high]: 0 unless the core is known to differ
  Source source = Source::Derived;
  std::string_view note; // why it differs, when it does
  double measured = 0;

  auto delta() const -> double;  // how far measured is outside [low, high] (0 inside)
  auto verdict() const -> Verdict;
};

// Every check, measured now. A few hundred milliseconds.
auto run_timing_checks() -> std::vector<TimingCheck>;
// The checks as a text table with the sources, and the count of checks that are Off.
auto format_timing_checks(std::span<const TimingCheck> checks) -> std::string;
auto checks_off(std::span<const TimingCheck> checks) -> int;

// A 32 KB LoROM program for frame-level checks: a red screen, NMI and auto-joypad read on, the
// main loop waits for NMI, and the NMI handler reads the controller on frames where (its frame counter & `mask`)
// is 0: mask 0 reads every frame, 1 every other frame.
enum class ProbeRead : uint8_t { AutoRead, Serial, None };  // $4218, $4016, or no read at all
auto make_probe_rom(uint8_t mask = 0, ProbeRead read = ProbeRead::AutoRead) -> std::vector<uint8_t>;

}  // namespace bitforge::debug
