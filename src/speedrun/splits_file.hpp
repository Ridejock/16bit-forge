// SPDX-License-Identifier: MIT
#pragma once
// Splits on disk: a .lss with its .autosplit and .race sidecars, and which file each game used last.
// Games keep their splits in <data dir>/splits/<rom name>.lss unless another file was opened
// for them; <rom name>.last there then holds that file's path.
#include "speedrun/splits.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace bitforge::speedrun {

inline constexpr std::string_view kSplitsFolder = "splits";
inline constexpr std::string_view kLastExtension = ".last";

// A run as it stands in its files.
struct SplitsFile {
  std::string path;      // the .lss
  std::string original;  // its text when last read or written: what a write keeps
  auto operator==(const SplitsFile&) const -> bool = default;
};

struct LoadedSplits {
  Run run;
  SplitsFile file;
};

auto default_splits_path(std::string_view data_dir, std::string_view rom_stem) -> std::string;
// The game's last splits file (the default when it has none on record).
auto remembered_splits_path(std::string_view data_dir, std::string_view rom_stem) -> std::string;
// Makes `path` the game's file from now on (the default needs no record). False if not written.
auto remember_splits_path(std::string_view data_dir, std::string_view rom_stem, std::string_view path) -> bool;

// Reads a .lss, its sidecar's rules and its race-verified attempts. nullopt (with `error`) if
// the file can't be read or isn't a splits file; a missing sidecar just means none.
auto load_splits(const std::string& path, std::string* error = nullptr) -> std::optional<LoadedSplits>;
// Writes `run` over `file` (keeping what the .lss had that we don't use) and its sidecars
// (each removed when the run has nothing for it); `file.original` becomes the new text. To write a copy
// elsewhere (export), pass a file with the new path and the original text.
auto save_splits(const Run& run, SplitsFile& file, std::string* error = nullptr) -> bool;

}  // namespace bitforge::speedrun
