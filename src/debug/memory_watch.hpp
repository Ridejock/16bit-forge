// SPDX-License-Identifier: MIT
#pragma once
// Memory watches: named addresses shown live, optionally frozen (their value written back every
// frame), saved per game in watches/<rom>.wch, and turned into a Pro Action Replay cheat.
// Freezing is for this session only: the file keeps the watches, not the freezes, so a game
// never boots with a value forced into memory it hasn't set up yet. A cheat is the lasting way.
//
// Watches are also the building block for a run timer or auto-splitter: each has a stable id
// (saved in the file), sample_watches() reads them all once per frame on the emulation thread
// (a few bytes, no allocation once warm, the same result for the same frames), and a Condition
// tests one reading against a value. memory_tampered() says whether anything wrote to memory
// (a freeze, a cheat, a one-shot edit), so a race mode can refuse or flag the run.
#include "debug/memory_value.hpp"
#include "snes/cheats.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bitforge::debug {

// Stable for the life of the watch: given once, saved, never reused while the watch exists.
using WatchId = uint32_t;
constexpr WatchId kNoWatch = 0;

struct Watch {
  std::string label;
  Region region = Region::Wram;
  uint32_t offset = 0;
  ValueType type;
  bool frozen = false;  // not saved
  uint32_t value = 0;   // the frozen value (raw)
  WatchId id = kNoWatch;
  auto operator==(const Watch&) const -> bool = default;
};

// Every region's memory, indexed by Region (empty when the game has none).
using RegionMemory = std::array<std::span<uint8_t>, kRegions.size()>;

// Writes each frozen watch's value. Watches outside their region's memory are skipped. Returns
// a bit per Region (1 << id) whose bytes changed.
auto apply_freezes(std::span<const Watch> watches, const RegionMemory& memory) -> uint32_t;

// Gives each watch without an id, or with one an earlier watch has, the next unused id (one
// above the largest). Ids already given are kept.
auto assign_watch_ids(std::vector<Watch>& watches) -> void;
auto find_watch(std::span<const Watch> watches, WatchId id) -> const Watch*;
auto any_frozen(std::span<const Watch> watches) -> bool;

// The watch's raw value in `memory`; nullopt when it doesn't fit (no SRAM, past the end).
auto read_watch(const Watch& watch, const RegionMemory& memory) -> std::optional<uint32_t>;

// ---- Per-frame readings and conditions (for splits) -------------------------------------------
struct WatchReading {
  WatchId id = kNoWatch;
  uint32_t now = 0;     // raw
  uint32_t before = 0;  // raw, the previous frame's reading (== now on the first frame)
  bool valid = false;   // false when the watch can't be read; conditions on it are false
};

// One reading per watch, in list order. `before` comes from `last` (matched by id); a watch
// with no earlier reading starts with before == now, so nothing "changes" on its first frame.
// `out` is reused, so a steady list allocates nothing.
auto sample_watches(std::span<const Watch> watches, const RegionMemory& memory, std::span<const WatchReading> last,
                    std::vector<WatchReading>& out) -> void;

enum class Test : uint8_t { Equal, NotEqual, Greater, Less, Changed, ChangedFrom, ChangedTo };

// before/now/value are numbers in the watch's format (signed formats sign-extend).
using TestFn = bool (*)(int64_t before, int64_t now, int64_t value);

struct TestInfo {
  Test id;
  std::string_view key;    // in a splits file's auto-split rules (speedrun/autosplit)
  std::string_view label;
  bool uses_value;
  TestFn test;
};

inline constexpr std::array kTests = {
    TestInfo{Test::Equal, "eq", "== value", true, [](int64_t, int64_t now, int64_t v) { return now == v; }},
    TestInfo{Test::NotEqual, "ne", "!= value", true, [](int64_t, int64_t now, int64_t v) { return now != v; }},
    TestInfo{Test::Greater, "gt", "> value", true, [](int64_t, int64_t now, int64_t v) { return now > v; }},
    TestInfo{Test::Less, "lt", "< value", true, [](int64_t, int64_t now, int64_t v) { return now < v; }},
    TestInfo{Test::Changed, "changed", "changed", false, [](int64_t b, int64_t now, int64_t) { return now != b; }},
    TestInfo{Test::ChangedFrom, "from", "changed from value", true,
             [](int64_t b, int64_t now, int64_t v) { return b == v && now != v; }},
    TestInfo{Test::ChangedTo, "to", "changed to value", true,
             [](int64_t b, int64_t now, int64_t v) { return b != v && now == v; }},
};
static_assert(kTests.size() == static_cast<size_t>(Test::ChangedTo) + 1);
constexpr auto info(Test t) -> const TestInfo& { return kTests[static_cast<size_t>(t)]; }

struct Condition {
  WatchId watch = kNoWatch;
  Test test = Test::Equal;
  uint32_t value = 0;  // raw, in the watch's size (two's complement for signed)
  auto operator==(const Condition&) const -> bool = default;
};

// True when the watch's reading passes the test. False for an unknown or unreadable watch.
auto evaluate(const Condition& condition, std::span<const Watch> watches, std::span<const WatchReading> readings)
    -> bool;

// ---- Tamper flags (for a race mode) -----------------------------------------------------------
// Bits of Emulator::memory_tampered(): what has written to the game's memory.
enum TamperBits : uint32_t {
  kTamperFreeze = 1u << 0,  // a watch is frozen now
  kTamperCheat = 1u << 1,   // a cheat is enabled now
  kTamperPoke = 1u << 2,    // a value was written by hand since the game was loaded
};

// The watches file, one per line:
//   [<id>] <region> <offset, hex> <bits> <format> <label...>
//   1 wram 000DBF 8 dec Lives
// The id is optional (a hand-written line gets a fresh one). Blank lines, '#' comments and
// lines that don't parse are skipped.
auto parse_watch_file(std::string_view text) -> std::vector<Watch>;
auto format_watch_file(std::span<const Watch> watches) -> std::string;
// One line of it (no newline), and back: nullopt if it doesn't parse; the id is kNoWatch when
// the line has none. The splits sidecar (speedrun/autosplit.hpp) uses the same text.
auto format_watch_line(const Watch& watch) -> std::string;
auto parse_watch_line(std::string_view line) -> std::optional<Watch>;

// A cheat that holds `value` at the watch's address: one Pro Action Replay code per byte, joined
// with '+' (written to WRAM every frame), described by the label. Nullopt for a region cheats
// can't write (SRAM).
auto watch_to_cheat(const Watch& watch, uint32_t value) -> std::optional<CheatEntry>;

}  // namespace bitforge::debug
