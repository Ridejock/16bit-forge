// SPDX-License-Identifier: MIT
#pragma once
// The timing report (--timing-report): the core's timing checks (debug/timing_check.hpp), the
// frontend's frame pacing, and, given a game, a fixed scripted input run: its frames, lag frames
// and master clocks, run again, through a rewind snapshot, a save state and a movie replay, to
// show the timing is the same every time. Plain text, for sharing.
#include <cstdint>
#include <string>

namespace bitforge::frontend {

struct TimingReportOptions {
  std::string emulator;  // "16bit Forge <version>"
  std::string rom;       // "" = the checks only
  uint64_t frames = 3600;
  std::string work_dir;  // for the run's state and movie files ("" = the temp folder); removed after
};

struct TimingReport {
  std::string text;
  bool ok = false;  // no check OFF, and every replay matched
};

auto make_timing_report(const TimingReportOptions& options) -> TimingReport;

}  // namespace bitforge::frontend
