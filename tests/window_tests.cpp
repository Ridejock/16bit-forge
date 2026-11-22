// SPDX-License-Identifier: MIT
// Window placement (frontend/window_placement.hpp): the game window beside the main window, sliding
// it left when there's room and never off the display (a maximized main window fills it); saved
// places found again on their display by name and bounds, the fallback when it's gone, clamping,
// the fullscreen display choice, menu labels and the settings text. Headless, no SDL.
#include "frontend/window_placement.hpp"

#include <cstdio>
#include <vector>

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

// Two displays side by side, the second one to the left of the primary.
const std::vector<DisplayInfo> kTwo = {
    {1, "DELL U2419H", {0, 0, 1920, 1080}, {0, 0, 1920, 1040}},
    {2, "LG ULTRAGEAR", {-2560, 0, 2560, 1440}, {-2560, 0, 2560, 1400}},
};

auto test_find_display() -> void {
    CHECK(find_display(kTwo, {"LG ULTRAGEAR", {-2560, 0, 2560, 1440}}) == 1);
    CHECK(find_display(kTwo, {"DELL U2419H", {0, 0, 1920, 1080}}) == 0);
    // Moved in the arrangement (or a new resolution): still found by its name.
    CHECK(find_display(kTwo, {"LG ULTRAGEAR", {1920, 0, 2560, 1440}}) == 1);
    // Unplugged, or never saved.
    CHECK(find_display(kTwo, {"BenQ XL2546", {1920, 0, 1920, 1080}}) == -1);
    CHECK(find_display(kTwo, {}) == -1);
    // Two of the same model: the bounds tell them apart; with neither matching it's ambiguous.
    const std::vector<DisplayInfo> twins = {
        {1, "DELL U2419H", {0, 0, 1920, 1080}, {0, 0, 1920, 1040}},
        {2, "DELL U2419H", {1920, 0, 1920, 1080}, {1920, 0, 1920, 1080}},
    };
    CHECK(find_display(twins, {"DELL U2419H", {1920, 0, 1920, 1080}}) == 1);
    CHECK(find_display(twins, {"DELL U2419H", {0, 1080, 1920, 1080}}) == -1);
}

auto test_fullscreen_display() -> void {
    constexpr uint32_t kOwn = 1;
    CHECK(display_or(kTwo, {}, kOwn) == kOwn);  // "Current window's display"
    CHECK(display_or(kTwo, {"LG ULTRAGEAR", {-2560, 0, 2560, 1440}}, kOwn) == 2);
    CHECK(display_or(kTwo, {"BenQ XL2546", {1920, 0, 1920, 1080}}, kOwn) == kOwn);  // unplugged
    CHECK(display_or({}, {"LG ULTRAGEAR", {-2560, 0, 2560, 1440}}, kOwn) == kOwn);
}

auto test_restore_rect() -> void {
    const DisplayInfo& lg = kTwo[1];
    // Same display, same place.
    CHECK(restore_rect({-2000, 100, 960, 640}, lg.bounds, lg, lg.usable) == (WindowRect{-2000, 100, 960, 640}));
    // The display moved to the right of the primary: same offset from its corner.
    const DisplayInfo moved{2, "LG ULTRAGEAR", {1920, 0, 2560, 1440}, {1920, 0, 2560, 1400}};
    CHECK(restore_rect({-2000, 100, 960, 640}, lg.bounds, moved, moved.usable) == (WindowRect{1920 + 560, 100, 960, 640}));
    // Now a smaller resolution: the window is clamped on screen and no bigger than the usable area.
    const DisplayInfo small{2, "LG ULTRAGEAR", {-1280, 0, 1280, 720}, {-1280, 0, 1280, 680}};
    const WindowRect r = restore_rect({-600, 600, 2000, 640}, lg.bounds, small, small.usable);
    CHECK(r.w == 1280 && r.h == 640);
    CHECK(r.x == -1280 && r.y == 680 - 640);
}

auto test_labels() -> void {
    const std::vector<std::string> labels = display_labels(kTwo);
    CHECK(labels.size() == 2 && labels[0] == "DELL U2419H (1920x1080)" && labels[1] == "LG ULTRAGEAR (2560x1440)");
    const std::vector<DisplayInfo> twins = {{1, "DELL", {0, 0, 1920, 1080}, {}}, {2, "DELL", {1920, 0, 1920, 1080}, {}}, {3, "", {0, 1080, 800, 600}, {}}};
    const std::vector<std::string> named = display_labels(twins);
    CHECK(named[0] == "DELL (1920x1080 at 0,0)" && named[1] == "DELL (1920x1080 at 1920,0)" && named[2] == "Display 3 (800x600)");
}

auto test_rect_text() -> void {
    WindowRect r;
    CHECK(parse_rect(format_rect({-2560, 40, 960, 640}), r) && r == (WindowRect{-2560, 40, 960, 640}));
    CHECK(parse_rect("10,20,0,0", r) && r == (WindowRect{10, 20, 0, 0}));  // size from the window scale
    r = {1, 2, 3, 4};
    CHECK(!parse_rect("10,20,30", r) && !parse_rect("a,b,c,d", r) && !parse_rect("1,2,3,4,5", r) && !parse_rect("1,2,-3,4", r));
    CHECK(r == (WindowRect{1, 2, 3, 4}));  // left alone
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
        {"displays: found by name and bounds, or name alone", test_find_display},
        {"displays: fullscreen target, fallback when gone", test_fullscreen_display},
        {"displays: saved place moved and clamped", test_restore_rect},
        {"displays: menu labels", test_labels},
        {"settings: rect text", test_rect_text},
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
