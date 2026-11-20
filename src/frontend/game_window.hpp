// SPDX-License-Identifier: MIT
#pragma once
#include "video/video_backend.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <span>
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

// Where the run timer's overlay sits on the picture.
enum class Corner : uint8_t { TopLeft, TopRight, BottomLeft, BottomRight };
struct CornerInfo {
    std::string_view id;     // settings value
    std::string_view label;  // UI
    bool right;
    bool bottom;
};
inline constexpr std::array<CornerInfo, 4> kCorners = {{
    {"top_left", "Top left", false, false},
    {"top_right", "Top right", true, false},
    {"bottom_left", "Bottom left", false, true},
    {"bottom_right", "Bottom right", true, true},
}};
inline auto info(Corner c) -> const CornerInfo& { return kCorners[static_cast<size_t>(c)]; }

// One line of an overlay's text, in the 8x8 font: its colour (ARGB) and glyph size (1 = 8x8).
struct TextLine {
    std::string text;
    uint32_t color = 0;
    int scale = 1;
};

// The client window that shows the emulated picture. It owns one video backend (SDL_GPU or
// SDL_Renderer) and only decides where the picture goes; the backend draws it. It also shows
// short on-screen messages (show_message) and the run timer (show_timer) over the picture, drawn
// after the filter.
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
    // The timer overlay is scaled in whole steps to about this share of the picture's height,
    // the movie badge (one line) to about this one.
    static constexpr float kTimerHeightShare = 0.16f;
    static constexpr float kBadgeHeightShare = 0.05f;

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
    // Shows an image (ARGB8888, w x h; empty hides it) as the run timer overlay in `corner` of
    // the picture, scaled with it. Re-sent to the backend only when it changes.
    auto show_timer(std::vector<uint32_t> pixels, int width, int height, Corner corner) -> void;
    // The same for the movie badge ("REC 1234").
    auto show_badge(std::vector<uint32_t> pixels, int width, int height, Corner corner) -> void;

private:
    auto create_backend(video::BackendKind wanted, const video::FilterPreset& filter) -> bool;
    // Where the message goes this frame and how opaque it is (0 once it has faded out).
    auto message_placement() -> video::OverlayPlacement;
    // An image shown in a corner of the picture, re-sent to the backend when it changes.
    struct CornerImage {
        std::vector<uint32_t> pixels;  // ARGB, as message_pixels_
        int w = 0;
        int h = 0;
        Corner corner = Corner::TopRight;
        bool dirty = false;
        auto set(std::vector<uint32_t> image, int width, int height, Corner where) -> void;
    };
    static auto corner_placement(const CornerImage& image, const video::Viewport& picture, float height_share) -> video::OverlayPlacement;

    SDL_Window* window_ = nullptr;
    std::unique_ptr<video::VideoBackend> backend_;

    // The current message, drawn once into an ARGB image; re-sent to the backend when it changes
    // or the backend is replaced.
    std::vector<uint32_t> message_pixels_;
    int message_w_ = 0;
    int message_h_ = 0;
    uint64_t message_start_ns_ = 0;
    bool message_dirty_ = false;

    CornerImage timer_;  // the run timer overlay
    CornerImage badge_;  // the movie's REC / PLAY badge
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
// The same look for several lines, each in its own colour and size, left- or right-aligned in
// a box at least `min_width` pixels wide (so a changing number doesn't make the box jump).
auto render_text_block(std::span<const TextLine> lines, bool align_right, int min_width, int& width, int& height)
    -> std::vector<uint32_t>;

} // namespace bitforge::frontend
