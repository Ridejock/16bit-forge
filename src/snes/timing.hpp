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

// Interrupt timing (Anomie's timing.txt "Interrupts" and "Instructions"; fullsnes "H/V Events").
inline constexpr int kNmiClock = 2;      // the NMI flag and /NMI: H=0.5 of the first V-blank line
inline constexpr int kVIrqClock = 1374;  // V-IRQ (and H-IRQ at HTIME 0): after dot 0 of the line before
inline constexpr int kHIrqClock = 14;    // H-IRQ: 14 + HTIME*4 after dot 0 of the line (H=HTIME+3.5)
// Dots 323 and 327 are 6 clocks long (fullsnes's H counter latch counts; 2 x 2 extra make the
// 1364-clock line), except on the short line, where every dot is 4. They push later H-IRQs back.
inline constexpr int kLongDot1 = 323;
inline constexpr int kLongDot2 = 327;
inline constexpr int kLongDotExtra = 2;
inline constexpr int kWaiExitClocks = 12;  // WAI ends 12 clocks (2 IO cycles) after /NMI or /IRQ goes low

}  // namespace bitforge::timing
