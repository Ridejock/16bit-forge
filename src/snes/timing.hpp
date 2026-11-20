// SPDX-License-Identifier: MIT
#pragma once
// NTSC console timing as the hardware documents give it (SNESdev wiki "Timing", fullsnes
// "SNES Timing H/V Counters" and "H/V Events", Anomie's timing.txt). The core runs on these, the
// run timer converts frames to time with them, and the timing checks compare the core against
// them. Master clocks throughout.
#include <cstdint>

namespace bitforge::timing {

// 945/44 MHz (six times the NTSC colour carrier), kept as a fraction: Hz = kMasterNum / kMasterDen.
inline constexpr int64_t kMasterNum = 236'250'000;
inline constexpr int64_t kMasterDen = 11;
inline constexpr double kMasterClockHz = static_cast<double>(kMasterNum) / static_cast<double>(kMasterDen);

inline constexpr int kLineClocks = 1364;       // 341 dots
inline constexpr int kShortLineClocks = 1360;  // line 240 of every other frame, interlace off
inline constexpr int kShortLine = 240;
inline constexpr int kLinesPerFrame = 262;     // 263 on interlaced field-0 frames

// Two non-interlaced frames: one of 262 full lines, one with the short line.
inline constexpr int64_t kFrameClocks = int64_t{kLinesPerFrame} * kLineClocks;           // 357368
inline constexpr int64_t kFramePairClocks = 2 * kFrameClocks - (kLineClocks - kShortLineClocks);  // 714732
// 60.0988139 Hz: the rate game time counts frames at.
inline constexpr double kNtscFrameRate = kMasterClockHz * 2.0 / static_cast<double>(kFramePairClocks);

}  // namespace bitforge::timing
