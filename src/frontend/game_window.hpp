// SPDX-License-Identifier: MIT
#pragma once
#include "video/video_backend.hpp"

#include <SDL3/SDL.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bitforge::frontend {

struct ViewOptions {
    int scale;
    bool integer_scaling;
    bool aspect_8_7;
};

// The client window that shows the emulated picture. It owns one video backend (SDL_GPU or
// SDL_Renderer) and only decides where the picture goes; the backend draws it.
class GameWindow {
public:
    // The picture is 256 x 224 base pixels. Frames come in 256 or 512 wide (hi-res) and 224 or
    // 239 lines (overscan), doubled to 448/478 when interlaced: hi-res keeps the same display
    // width and interlace the same display height, and overscan shows 15 more lines.
    static constexpr int kBaseWidth = 256;
    static constexpr int kBaseHeight = 224;
    static constexpr int kMaxBaseLines = 239;

    GameWindow() = default;
    GameWindow(const GameWindow&) = delete;
    auto operator=(const GameWindow&) -> GameWindow& = delete;
    ~GameWindow() { close(); }

    // Creates the window and the `wanted` backend, falling back to SDL_Renderer if it fails.
    auto open(const char* title, const ViewOptions& view, video::BackendKind wanted, const video::FilterPreset& filter)
        -> bool;
    auto close() -> void;
    auto is_open() const -> bool { return window_ != nullptr; }
    auto window() const -> SDL_Window* { return window_; }

    // Swaps the backend at runtime (same fallback as open()).
    auto set_backend(video::BackendKind wanted, const video::FilterPreset& filter) -> void;
    auto set_filter(const video::FilterPreset& filter) -> void;
    // e.g. "SDL_GPU (vulkan)", or empty while closed.
    auto backend_description() const -> std::string;

    auto set_title(const char* title) -> void;
    auto apply_scale(const ViewOptions& view) -> void;
    auto toggle_fullscreen() -> void;
    auto is_fullscreen() const -> bool;

    // Uploads the frame (XRGB8888) and presents it letterboxed. A null frame clears to black.
    auto present(const uint32_t* pixels, int width, int height, const ViewOptions& view) -> void;
    // The next present() also saves the window's image to `path` (BMP).
    auto request_screenshot(std::string path) -> void;

private:
    auto create_backend(video::BackendKind wanted, const video::FilterPreset& filter) -> bool;

    SDL_Window* window_ = nullptr;
    std::unique_ptr<video::VideoBackend> backend_;
};

// Display width of the picture in base pixels, with or without the 8:7 pixel aspect.
auto display_width(bool aspect_8_7) -> float;

// Lines of the picture in base pixels for a frame `frame_height` rows tall: 224 or 239, with
// interlaced frames (448/478 rows) counted at half height.
auto picture_lines(int frame_height) -> int;

// Where a picture of `lines` base lines goes in an `out_w` x `out_h` window: centered, aspect
// kept, and whole multiples of the base size when integer scaling is on and the window is big
// enough. Any frame width maps onto the same display width.
auto fit_viewport(int out_w, int out_h, const ViewOptions& view, int lines = GameWindow::kBaseHeight) -> video::Viewport;

} // namespace bitforge::frontend
