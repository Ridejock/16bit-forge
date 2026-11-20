// SPDX-License-Identifier: MIT
#pragma once
// LiveSplit splits files (.lss), written from the published format description (no LiveSplit
// code): an XML <Run> with GameName, CategoryName, Offset, AttemptCount, AttemptHistory and
// Segments; each Segment has a Name, SplitTimes (we use the "Personal Best" one),
// BestSegmentTime and SegmentHistory, each time as <RealTime>/<GameTime> TimeSpan text.
// Older files with a bare time as the element's text are read as real time.
//
// Writing back over the file it came from keeps everything we don't use (icons, layout path,
// metadata, other comparisons, auto splitter settings, attempt attributes): only the fields
// above are updated. The auto-split rules don't go in the .lss (see autosplit.hpp), so the file
// stays valid for LiveSplit.
#include "speedrun/splits.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace bitforge::speedrun {

inline constexpr std::string_view kLssExtension = ".lss";
inline constexpr std::string_view kPersonalBest = "Personal Best";

// The run in `text`; nullopt (with `error`) when it isn't a splits file.
auto read_lss(std::string_view text, std::string* error = nullptr) -> std::optional<Run>;
// `run` as a .lss. `original`: the file's current text, whose other contents are kept.
auto write_lss(const Run& run, std::string_view original = {}) -> std::string;

}  // namespace bitforge::speedrun
