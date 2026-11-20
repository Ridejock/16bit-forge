// SPDX-License-Identifier: MIT
#pragma once
// The auto-split rules of a splits file, kept beside it in <name>.autosplit so the .lss stays
// LiveSplit's. One rule per line: what it does, the test (debug::kTests keys) and its value in
// the watch's format, then '|' and the watch as in the watches file:
//
//   start     changed       | 2 wram 000100 8 dec Screen
//   split 1   lt 3          | 1 wram 001FE8 8 dec HP
//   split 2   to $04        | 3 wram 000102 8 hex Stage
//   reset     eq 0          | 1 wram 001FE8 8 dec HP
//
// Segments count from 1. '#' starts a comment; lines that don't parse are skipped.
#include "speedrun/splits.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace bitforge::speedrun {

inline constexpr std::string_view kAutoSplitExtension = ".autosplit";

// "dir/Game.lss" -> "dir/Game.autosplit"
auto autosplit_path(std::string_view lss_path) -> std::string;
// The rules in `run` as the sidecar's text ("" when it has none).
auto format_autosplits(const Run& run) -> std::string;
// Reads the rules into `run` (replacing any it had). Returns how many were read.
auto parse_autosplits(std::string_view text, Run& run) -> int;

// Ties every rule to a watch in `watches` (the game's list): the same id and address, else the
// same address and type, else the rule's watch is added (with a new id). Returns true when
// `watches` grew.
auto resolve_rules(Run& run, std::vector<debug::Watch>& watches) -> bool;

// "HP < 3", "Stage changed to $04": for the UI.
auto describe(const Rule& rule) -> std::string;

}  // namespace bitforge::speedrun
