// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace bitforge::frontend {

// Where the windows open: beside each other, or where they were last time. Pure math on rectangles
// (SDL_Rect-like, screen coordinates of the client areas) and display lists, so it is testable
// without a display.
struct WindowRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    auto operator==(const WindowRect&) const -> bool = default;
};

struct WindowPlacement {
    int main_x = 0;  // where the main window goes (unchanged unless it slid left)
    int game_x = 0;
    int game_y = 0;
};

// Keeps a window of size w x h inside area, top-left first when it can't fit.
inline auto clamp_into(int x, int y, int w, int h, const WindowRect& area) -> WindowRect {
    x = std::max(area.x, std::min(x, area.x + area.w - w));
    y = std::max(area.y, std::min(y, area.y + area.h - h));
    return {x, y, w, h};
}

// Side by side: the game window to the right of the main window, top edges aligned. When the pair
// would spill off the right edge, a movable main window slides left; one that can't move (maximized
// or fullscreen) or a pair wider than the display leaves the game window overlapping it instead.
// The game window always ends up inside usable (the display's usable bounds, less its frame).
inline auto place_beside(const WindowRect& main, bool main_movable, int game_w, int game_h, const WindowRect& usable,
                         int gap) -> WindowPlacement {
    int main_x = main.x;
    const int right = usable.x + usable.w;
    const int needed = main.w + gap + game_w;
    if (main_movable && main_x + needed > right) main_x = std::max(usable.x, right - needed);
    const WindowRect game = clamp_into(main_x + main.w + gap, main.y, game_w, game_h, usable);
    return {main_x, game.x, game.y};
}

// A connected display (the app fills these from SDL; id is its SDL_DisplayID).
struct DisplayInfo {
    uint32_t id = 0;
    std::string name;
    WindowRect bounds;
    WindowRect usable;  // less the taskbar / dock
};

// A display as saved in the settings: indexes change when monitors come and go, so it is found
// again by name and bounds. An empty name means none.
struct DisplayRef {
    std::string name;
    WindowRect bounds;
    auto empty() const -> bool { return name.empty(); }
    auto operator==(const DisplayRef&) const -> bool = default;
};

inline auto display_ref(const DisplayInfo& d) -> DisplayRef { return {d.name, d.bounds}; }

// A window's last restored (not maximized, not fullscreen) place and size, the display it was on,
// and whether it was maximized. A size of 0 keeps the window's own size.
struct WindowGeometry {
    DisplayRef display;
    WindowRect rect;
    bool maximized = false;
    auto operator==(const WindowGeometry&) const -> bool = default;
};

// The display `ref` names: the one with the same name and bounds, else the only one with that name
// (moved in the arrangement or a new resolution). -1 when it isn't connected (or can't be told apart).
inline auto find_display(std::span<const DisplayInfo> displays, const DisplayRef& ref) -> int {
    if (ref.empty()) return -1;
    int named = -1;
    int count = 0;
    for (size_t i = 0; i < displays.size(); i++) {
        if (displays[i].name != ref.name) continue;
        if (displays[i].bounds == ref.bounds) return static_cast<int>(i);
        named = static_cast<int>(i);
        count++;
    }
    return count == 1 ? named : -1;
}

// The id of the display `ref` names, or `fallback` (e.g. the window's own display) when it is gone.
inline auto display_or(std::span<const DisplayInfo> displays, const DisplayRef& ref, uint32_t fallback) -> uint32_t {
    const int i = find_display(displays, ref);
    return i < 0 ? fallback : displays[static_cast<size_t>(i)].id;
}

// A saved rect put back on `display`: the same offset from its top-left corner as on the display it
// was saved on (`saved_bounds`), no bigger than `usable`, and clamped inside it.
inline auto restore_rect(const WindowRect& rect, const WindowRect& saved_bounds, const DisplayInfo& display,
                         const WindowRect& usable) -> WindowRect {
    const int w = std::min(rect.w, usable.w);
    const int h = std::min(rect.h, usable.h);
    return clamp_into(rect.x - saved_bounds.x + display.bounds.x, rect.y - saved_bounds.y + display.bounds.y, w, h, usable);
}

// Menu labels: "Name (1920x1080)", with the position added when two displays share a name.
inline auto display_labels(std::span<const DisplayInfo> displays) -> std::vector<std::string> {
    std::vector<std::string> labels;
    for (size_t i = 0; i < displays.size(); i++) {
        const DisplayInfo& d = displays[i];
        const bool shared = std::count_if(displays.begin(), displays.end(), [&](const DisplayInfo& o) { return o.name == d.name; }) > 1;
        char size[64];
        if (shared) std::snprintf(size, sizeof(size), " (%dx%d at %d,%d)", d.bounds.w, d.bounds.h, d.bounds.x, d.bounds.y);
        else std::snprintf(size, sizeof(size), " (%dx%d)", d.bounds.w, d.bounds.h);
        labels.push_back((d.name.empty() ? "Display " + std::to_string(i + 1) : d.name) + size);
    }
    return labels;
}

// "x,y,w,h" in the settings file.
inline auto format_rect(const WindowRect& r) -> std::string {
    return std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.w) + "," + std::to_string(r.h);
}

inline auto parse_rect(const std::string& text, WindowRect& out) -> bool {
    WindowRect r;
    char tail = 0;
    if (std::sscanf(text.c_str(), "%d,%d,%d,%d%c", &r.x, &r.y, &r.w, &r.h, &tail) != 4 || r.w < 0 || r.h < 0) return false;
    out = r;
    return true;
}

} // namespace bitforge::frontend
