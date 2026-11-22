// SPDX-License-Identifier: MIT
// Game window placement (frontend/window_placement.hpp): beside the main window, sliding it left
// when there's room, and never off the display (a maximized main window fills it). Headless, no SDL.
#include "frontend/window_placement.hpp"

#include <cstdio>

using namespace bitforge::frontend;

namespace {

int checks_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("\n    check failed: %s (line %d)", #cond, __LINE__);    \
            checks_failed++;                                                     \
        }                                                                        \
    } while (0)

constexpr WindowRect kUsable{0, 0, 1920, 1040};  // a 1080p display less its taskbar
constexpr int kGap = 16;
constexpr int kGameW = 768;
constexpr int kGameH = 672;

auto inside(const WindowPlacement& p, const WindowRect& area) -> bool {
    return p.game_x >= area.x && p.game_y >= area.y && p.game_x + kGameW <= area.x + area.w &&
           p.game_y + kGameH <= area.y + area.h;
}

auto test_side_by_side() -> void {
    const WindowPlacement p = place_beside({100, 50, 960, 640}, true, kGameW, kGameH, kUsable, kGap);
    CHECK(p.main_x == 100);
    CHECK(p.game_x == 100 + 960 + kGap);
    CHECK(p.game_y == 50);
}

auto test_slides_main_left() -> void {
    const WindowPlacement p = place_beside({600, 50, 960, 640}, true, kGameW, kGameH, kUsable, kGap);
    CHECK(p.main_x == 1920 - (960 + kGap + kGameW));
    CHECK(p.game_x == p.main_x + 960 + kGap);
    CHECK(inside(p, kUsable));
}

// The bug: a maximized main window fills the display, so "to its right" was off screen.
auto test_maximized_main_stays_on_screen() -> void {
    const WindowRect maximized{kUsable.x, kUsable.y, kUsable.w, kUsable.h};
    const WindowPlacement p = place_beside(maximized, false, kGameW, kGameH, kUsable, kGap);
    CHECK(p.main_x == maximized.x);  // not moved
    CHECK(inside(p, kUsable));
    // Even if it is reported as movable, the game window still lands on screen.
    CHECK(inside(place_beside(maximized, true, kGameW, kGameH, kUsable, kGap), kUsable));
}

// A second display to the left (negative coordinates), and a display whose usable area is offset.
auto test_other_displays() -> void {
    const WindowRect left_display{-1920, 0, 1920, 1040};
    const WindowPlacement p = place_beside(left_display, false, kGameW, kGameH, left_display, kGap);
    CHECK(inside(p, left_display));
    const WindowRect offset{1920, 40, 2560, 1400};
    CHECK(inside(place_beside(offset, false, kGameW, kGameH, offset, kGap), offset));
}

// A main window low on screen: the game window moves up rather than hanging off the bottom.
auto test_bottom_edge() -> void {
    const WindowPlacement p = place_beside({0, 900, 600, 400}, true, kGameW, kGameH, kUsable, kGap);
    CHECK(inside(p, kUsable));
    CHECK(p.game_y == kUsable.h - kGameH);
}

// A game window bigger than the display: its top-left (title bar) stays visible.
auto test_too_big() -> void {
    const WindowRect r = clamp_into(500, 500, 4000, 3000, kUsable);
    CHECK(r.x == kUsable.x);
    CHECK(r.y == kUsable.y);
}

}  // namespace

int main() {
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"placement: side by side", test_side_by_side},
        {"placement: slides the main window left", test_slides_main_left},
        {"placement: maximized main window, game on screen", test_maximized_main_stays_on_screen},
        {"placement: other displays", test_other_displays},
        {"placement: bottom edge", test_bottom_edge},
        {"placement: larger than the display", test_too_big},
    };
    std::printf("=== Window placement tests ===\n");
    int passed = 0, failed = 0;
    for (const auto& t : tests) {
        const int before = checks_failed;
        std::printf("  %-52s", t.name);
        t.fn();
        if (checks_failed == before) { std::printf(" PASS\n"); passed++; } else { std::printf("\n  FAIL\n"); failed++; }
    }
    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
