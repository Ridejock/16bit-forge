// SPDX-License-Identifier: MIT
// Frame pacing (frontend/frame_pacer.hpp), in virtual time: the frame schedule the emulation
// thread runs on, its catch-up and resync. Headless, no SDL.
#include "frontend/frame_pacer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace snes::frontend;

namespace {

int checks_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("\n    check failed: %s (line %d)", #cond, __LINE__);    \
            checks_failed++;                                                     \
        }                                                                        \
    } while (0)

constexpr uint64_t kMs = 1'000'000;
constexpr uint64_t kFrameNs = static_cast<uint64_t>(1'000'000'000.0 / 60.0988);
constexpr uint64_t kMaxLag = 4;

auto test_pacer_schedule() -> void {
    FramePacer p(kFrameNs, kMaxLag);
    uint64_t t = 1000 * kMs;
    p.reset(t);
    // A loop that does 5 ms of work and sleeps what advance() says runs at exactly the frame rate.
    int frames = 0;
    while (t < 1000 * kMs + 10'000 * kMs) {
        t += 5 * kMs;
        t += p.advance(t);
        frames++;
    }
    CHECK(std::abs(frames - 601) <= 1);
    CHECK(p.next_ns() == t);  // woke exactly on schedule
}

auto test_pacer_lag() -> void {
    FramePacer p(kFrameNs, kMaxLag);
    p.reset(0);
    // 3 frames late: catch up without sleeping, keep the schedule.
    uint64_t t = 3 * kFrameNs + kFrameNs / 2;
    CHECK(p.advance(t) == 0);
    CHECK(p.next_ns() == kFrameNs);
    CHECK(p.due(t));
    // 10 frames late: resync to now instead of fast-forwarding.
    FramePacer q(kFrameNs, kMaxLag);
    q.reset(0);
    t = 10 * kFrameNs;
    CHECK(q.advance(t) == 0);
    CHECK(q.next_ns() == t);
    CHECK(q.advance(t) == kFrameNs);
    q.sync(5 * kMs);
    CHECK(q.next_ns() == 5 * kMs && q.due(5 * kMs) && !q.due(5 * kMs - 1));
}

}  // namespace

int main() {
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"pacer: on-time loop runs at 60.0988 fps", test_pacer_schedule},
        {"pacer: catch up within 4 frames, resync past", test_pacer_lag},
    };
    std::printf("=== Frame pacing tests ===\n");
    int passed = 0, failed = 0;
    for (const auto& t : tests) {
        const int before = checks_failed;
        std::printf("  %-48s", t.name);
        t.fn();
        if (checks_failed == before) { std::printf(" PASS\n"); passed++; } else { std::printf("\n  FAIL\n"); failed++; }
    }
    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
