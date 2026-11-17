// SPDX-License-Identifier: MIT
#pragma once
#include <SDL3/SDL.h>

namespace bitforge::frontend {

// Gives a window the 16bit Forge icon (assets/icons/16bitForge_64.png, embedded at build time).
// On Windows this does nothing: SDL already uses the icon resource in 16bitForge.exe, which
// carries every size including the hand-drawn small ones.
auto set_app_icon(SDL_Window* window) -> void;

} // namespace bitforge::frontend
