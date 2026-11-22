// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>

namespace bitforge::frontend {

// Where the game window opens next to the main window. Pure math on rectangles (SDL_Rect-like,
// screen coordinates of the client areas), so it is testable without a display.
struct WindowRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
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

} // namespace bitforge::frontend
