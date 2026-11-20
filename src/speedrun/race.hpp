// SPDX-License-Identifier: MIT
#pragma once
// Race mode: the emulator in a state a leaderboard can trust. Turning it on restarts the game
// from power-on, and until it is turned off every way to change the game's memory or timing
// from outside (the guards below) is off: the UI greys them out and the core refuses them.
//
// A run started in race mode that finishes with race mode still on, no guard used and no memory
// tampering (debug::TamperBits) is "race verified" (RunTimer::race_status). Verified attempts are
// listed in a sidecar beside the .lss (<name>.race), so the .lss stays LiveSplit's, and each
// verified finish can be written as a small JSON summary for a leaderboard submission.
// No SDL or ImGui: the frontend shows it, the tests drive it.
#include "speedrun/splits.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bitforge::speedrun {

// Everything race mode turns off.
enum class Guard : uint8_t {
  Cheats,        // cheat codes are not applied
  Freeze,        // memory watches can't be frozen
  MemoryEdit,    // no writes from the memory panel
  Rewind,        // the rewind hotkey and its snapshots
  Timeline,      // playing from (or saving) a timeline moment
  LoadState,     // save-state loads
  SaveState,     // save-state saves (a run is one sitting from power-on)
  FastForward,   // fast-forward, hold or toggle
  FrameAdvance,  // stepping frames while paused
  TimerPause,    // pausing the run timer (real time would stop counting)
};
struct GuardInfo {
  Guard id;
  std::string_view key;    // the verification file's "blocked" list
  std::string_view label;  // "Loading states": UI text, starts a sentence
};
inline constexpr std::array<GuardInfo, 10> kGuards = {{
    {Guard::Cheats, "cheats", "Cheats"},
    {Guard::Freeze, "memory_freeze", "Memory freezes"},
    {Guard::MemoryEdit, "memory_edit", "Memory edits"},
    {Guard::Rewind, "rewind", "Rewind"},
    {Guard::Timeline, "timeline", "The timeline"},
    {Guard::LoadState, "load_state", "Loading states"},
    {Guard::SaveState, "save_state", "Saving states"},
    {Guard::FastForward, "fast_forward", "Fast-forward"},
    {Guard::FrameAdvance, "frame_advance", "Frame advance"},
    {Guard::TimerPause, "timer_pause", "Pausing the timer"},
}};
inline auto info(Guard g) -> const GuardInfo& { return kGuards[static_cast<size_t>(g)]; }
inline constexpr auto guard_bit(Guard g) -> uint32_t { return 1u << static_cast<uint32_t>(g); }
// What race mode blocks: all of them.
inline constexpr uint32_t kRaceBlocks = (1u << kGuards.size()) - 1;
inline constexpr auto race_allows(bool race, Guard g) -> bool { return !race || (kRaceBlocks & guard_bit(g)) == 0; }

// "Loading states is off in race mode." (status bar, game window message)
auto blocked_message(Guard g) -> std::string;
// The tooltip on a greyed-out control.
auto blocked_tooltip(Guard g) -> std::string;

// Where a run stands for race verification.
enum class RaceStatus : uint8_t {
  Off,       // not a race run (started without race mode, or no run)
  Clean,     // a race run, so far untouched
  Verified,  // finished in race mode, untouched
  Broken,    // a race run that stopped counting (see RaceBreak)
  Replay,    // a movie played (some of) this run: it isn't a player's
};
// Why a race run stopped counting (bits).
enum RaceBreak : uint32_t {
  kBreakRaceOff = 1u << 0,  // race mode was turned off mid-run
  kBreakTamper = 1u << 1,   // a cheat, freeze or memory edit was active
  kBreakJump = 1u << 2,     // a state load, rewind or timeline jump happened
};
struct RaceStatusInfo {
  RaceStatus id;
  std::string_view tag;   // the timer's badge ("" = none)
  std::string_view help;  // its tooltip
};
inline constexpr std::array<RaceStatusInfo, 5> kRaceStatuses = {{
    {RaceStatus::Off, "", ""},
    {RaceStatus::Clean, "RACE", "Race mode run: nothing that changes the game or its timing has been used so far."},
    {RaceStatus::Verified, "RACE VERIFIED", "Finished from power-on in race mode with nothing that changes the game or its timing used."},
    {RaceStatus::Broken, "NOT VERIFIED", "This run started in race mode but no longer counts as race verified."},
    {RaceStatus::Replay, "REPLAY", "A movie played this run (or part of it): it is a replay, never race verified."},
}};
inline auto info(RaceStatus s) -> const RaceStatusInfo& { return kRaceStatuses[static_cast<size_t>(s)]; }
// "race mode was turned off, a cheat or memory edit was active": for the Broken tooltip.
auto describe_breaks(uint32_t breaks) -> std::string;

// ---- Verified attempts: <name>.race beside the .lss ------------------------------------------
// One line per verified attempt: "verified <attempt id>". '#' starts a comment.
inline constexpr std::string_view kRaceExtension = ".race";
auto race_sidecar_path(std::string_view lss_path) -> std::string;
auto format_race_sidecar(const Run& run) -> std::string;  // "" when no attempt is verified
auto parse_race_sidecar(std::string_view text, Run& run) -> int;  // marks the attempts; returns how many
auto verified_runs(const Run& run) -> int;

// ---- The verification summary, for a leaderboard submission ---------------------------------
struct Verification {
  std::string emulator;  // "16bit Forge 0.1.1"
  std::string game;
  std::string category;
  std::string rom_file;
  uint32_t rom_crc32 = 0;  // of the ROM as the core loaded it (no copier header)
  std::string rom_sha1;    // of the file, hex
  VideoStandard standard = VideoStandard::Ntsc;
  int attempt = 0;
  std::string started;  // UTC, as the .lss stamps it
  std::string ended;
  Times final_time;
  int64_t game_frames = 0;
  struct Split {
    std::string name;
    Times time;  // cumulative; empty = skipped
  };
  std::vector<Split> splits;
  // The run's movie (movie.hpp), when one recorded it all: its file and that file's SHA-1.
  struct MovieRef {
    std::string file;  // relative to the emulator's folder, '/' separated
    std::string sha1;
    uint64_t frames = 0;
    uint32_t rerecords = 0;
  };
  std::optional<MovieRef> movie;
};
// A small JSON document; the "blocked" list comes from kGuards.
auto format_verification(const Verification& v) -> std::string;
// "dir/Game.lss", attempt 12 -> "dir/Game.attempt-12.verified.json"
auto verification_path(std::string_view lss_path, int attempt) -> std::string;

}  // namespace bitforge::speedrun
