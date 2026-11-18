// SPDX-License-Identifier: MIT
#pragma once
#include "video/video_backend.hpp"

#include <SDL3/SDL.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bitforge::frontend {

struct ViewOptions {
    int scale;
    bool integer_scaling;
    bool aspect_8_7;
};

// The client window that shows the emulated picture. It owns one video backend (SDL_GPU or
// SDL_Renderer) and only decides where the picture goes; the backend draws it. It also shows
// short on-screen messages (show_message) over the picture, drawn after the filter.
class GameWindow {
public:
    // The picture is 256 x 224 base pixels. Frames come in 256 or 512 wide (hi-res) and 224 or
    // 239 lines (overscan), doubled to 448/478 when interlaced: hi-res keeps the same display
    // width and interlace the same display height, and overscan shows 15 more lines.
    static constexpr int kBaseWidth = 256;
    static constexpr int kBaseHeight = 224;
    static constexpr int kMaxBaseLines = 239;

    // On-screen messages: fully shown for kMessageHoldNs, then faded out over kMessageFadeNs.
    static constexpr uint64_t kMessageHoldNs = 2'000'000'000;
    static constexpr uint64_t kMessageFadeNs = 250'000'000;

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
    // The next present() also saves the window's image to `path` (BMP), message included.
    auto request_screenshot(std::string path) -> void;

    // Shows `text` (ASCII; anything else draws as '?') in the top-left corner in the 8x8 debug
    // font, replacing any current message. Empty text clears it.
    auto show_message(std::string_view text) -> void;

private:
    auto create_backend(video::BackendKind wanted, const video::FilterPreset& filter) -> bool;
    // Where the message goes this frame and how opaque it is (0 once it has faded out).
    auto message_placement() -> video::OverlayPlacement;

    SDL_Window* window_ = nullptr;
    std::unique_ptr<video::VideoBackend> backend_;

    // The current message, drawn once into an ARGB image; re-sent to the backend when it changes
    // or the backend is replaced.
    std::vector<uint32_t> message_pixels_;
    int message_w_ = 0;
    int message_h_ = 0;
    uint64_t message_start_ns_ = 0;
    bool message_dirty_ = false;
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

// Draws `text` in the 8x8 font as an ARGB8888 image (straight alpha): light text with a dark
// drop shadow on a translucent dark box. Sets `width`/`height`; empty text gives an empty image.
auto render_message(std::string_view text, int& width, int& height) -> std::vector<uint32_t>;

} // namespace bitforge::frontend
