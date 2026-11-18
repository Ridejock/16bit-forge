// SPDX-License-Identifier: MIT
#include "frontend/game_window.hpp"
#include "frontend/app_icon.hpp"
#include "frontend/font8x8.hpp"
#include "utils/logging.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace bitforge::frontend {

namespace {

constexpr float kPixelAspect87 = 8.0f / 7.0f;
constexpr float kFitEpsilon = 1e-3f;  // absorbs float error before integer flooring

// On-screen message look, in font pixels (multiplied by the message scale on screen).
constexpr int kMessageMargin = 4;      // from the window's top-left corner
constexpr int kMessagePadding = 3;     // box around the text
constexpr size_t kMessageMaxChars = 64;
constexpr uint32_t kMessageText = 0xFFF0F0F0;    // ARGB
constexpr uint32_t kMessageShadow = 0xFF000000;  // one pixel down-right of the text
constexpr uint32_t kMessageBox = 0xA0101010;

// Round up so an N-times window always fits N whole picture widths (8:7 is fractional).
auto window_width(const ViewOptions& view) -> int {
    return static_cast<int>(std::ceil(display_width(view.aspect_8_7) * static_cast<float>(view.scale)));
}

} // namespace

auto display_width(bool aspect_8_7) -> float {
    return static_cast<float>(GameWindow::kBaseWidth) * (aspect_8_7 ? kPixelAspect87 : 1.0f);
}

auto picture_lines(int frame_height) -> int {
    return frame_height > GameWindow::kMaxBaseLines ? frame_height / 2 : frame_height;
}

auto fit_viewport(int out_w, int out_h, const ViewOptions& view, int lines) -> video::Viewport {
    // Fit the picture in base pixels (any internal resolution maps onto it) into the window.
    const float pic_w = display_width(view.aspect_8_7);
    const float pic_h = static_cast<float>(lines);
    float fit = std::min(static_cast<float>(out_w) / pic_w, static_cast<float>(out_h) / pic_h);
    if (view.integer_scaling && fit >= 1.0f) fit = std::floor(fit + kFitEpsilon);

    const float w = std::round(pic_w * fit);
    const float h = std::round(pic_h * fit);
    return {static_cast<int>(std::floor((static_cast<float>(out_w) - w) * 0.5f)),
            static_cast<int>(std::floor((static_cast<float>(out_h) - h) * 0.5f)), static_cast<int>(w),
            static_cast<int>(h)};
}

auto render_message(std::string_view text, int& width, int& height) -> std::vector<uint32_t> {
    text = text.substr(0, kMessageMaxChars);
    if (text.empty()) {
        width = height = 0;
        return {};
    }
    // The shadow adds one pixel right and down.
    width = static_cast<int>(text.size()) * kFontGlyphSize + 2 * kMessagePadding + 1;
    height = kFontGlyphSize + 2 * kMessagePadding + 1;
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height), kMessageBox);
    for (const auto& [offset, color] : {std::pair{1, kMessageShadow}, std::pair{0, kMessageText}}) {
        for (size_t i = 0; i < text.size(); i++) {
            const char c = text[i] >= kFontFirstChar && text[i] <= kFontLastChar ? text[i] : '?';
            const auto& glyph = kFont8x8[static_cast<size_t>(c - kFontFirstChar)];
            const int left = kMessagePadding + static_cast<int>(i) * kFontGlyphSize + offset;
            for (int y = 0; y < kFontGlyphSize; y++) {
                for (int x = 0; x < kFontGlyphSize; x++) {
                    if (!((glyph[static_cast<size_t>(y)] >> x) & 1)) continue;
                    const int row = kMessagePadding + y + offset;
                    pixels[static_cast<size_t>(row) * static_cast<size_t>(width) + static_cast<size_t>(left + x)] = color;
                }
            }
        }
    }
    return pixels;
}

auto GameWindow::open(const char* title, const ViewOptions& view, video::BackendKind wanted,
                      const video::FilterPreset& filter) -> bool {
    if (window_) return true;

    window_ = SDL_CreateWindow(title, window_width(view), kBaseHeight * view.scale,
                               SDL_WINDOW_RESIZABLE);
    if (!window_) return false;
    set_app_icon(window_);
    SDL_SetWindowMinimumSize(window_, kBaseWidth, kBaseHeight);
    if (!create_backend(wanted, filter)) {
        close();
        return false;
    }
    return true;
}

auto GameWindow::create_backend(video::BackendKind wanted, const video::FilterPreset& filter) -> bool {
    backend_.reset();  // release the window before the next backend claims it
    backend_ = video::make_backend(wanted);
    if (!backend_->create(window_)) {
        if (wanted == video::BackendKind::Renderer) {
            backend_.reset();
            return false;
        }
        utils::Logger::warning(utils::LogChannel::Video, "SDL_GPU unavailable; falling back to SDL_Renderer (nearest/linear only)");
        backend_ = video::make_backend(video::BackendKind::Renderer);
        if (!backend_->create(window_)) {
            backend_.reset();
            return false;
        }
    }
    backend_->set_filter(filter);
    message_dirty_ = true;  // the new backend has no overlay yet
    return true;
}

auto GameWindow::close() -> void {
    backend_.reset();
    if (window_) SDL_DestroyWindow(window_);
    window_ = nullptr;
}

auto GameWindow::set_backend(video::BackendKind wanted, const video::FilterPreset& filter) -> void {
    if (!window_ || (backend_ && backend_->kind() == wanted)) return;
    if (!create_backend(wanted, filter)) utils::Logger::error(utils::LogChannel::Video, "No video backend works for the game window");
}

auto GameWindow::set_filter(const video::FilterPreset& filter) -> void {
    if (backend_) backend_->set_filter(filter);
}

auto GameWindow::backend_description() const -> std::string {
    return backend_ ? backend_->description() : std::string();
}

auto GameWindow::set_title(const char* title) -> void {
    if (window_) SDL_SetWindowTitle(window_, title);
}

auto GameWindow::apply_scale(const ViewOptions& view) -> void {
    if (!window_ || is_fullscreen()) return;
    SDL_SetWindowSize(window_, window_width(view), kBaseHeight * view.scale);
}

auto GameWindow::is_fullscreen() const -> bool {
    return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN);
}

auto GameWindow::toggle_fullscreen() -> void {
    if (window_) SDL_SetWindowFullscreen(window_, !is_fullscreen());
}

auto GameWindow::present(const uint32_t* pixels, int width, int height, const ViewOptions& view) -> void {
    if (!backend_) return;
    int out_w = 0;
    int out_h = 0;
    SDL_GetWindowSizeInPixels(window_, &out_w, &out_h);
    backend_->upload_frame(pixels, width, height, width * static_cast<int>(sizeof(uint32_t)));
    if (message_dirty_) {
        backend_->upload_overlay(message_pixels_.empty() ? nullptr : message_pixels_.data(), message_w_, message_h_);
        message_dirty_ = false;
    }
    backend_->present(fit_viewport(out_w, out_h, view, height > 0 ? picture_lines(height) : kBaseHeight),
                      message_placement());
}

auto GameWindow::show_message(std::string_view text) -> void {
    message_pixels_ = render_message(text, message_w_, message_h_);
    message_start_ns_ = SDL_GetTicksNS();
    message_dirty_ = true;
}

auto GameWindow::message_placement() -> video::OverlayPlacement {
    if (message_pixels_.empty()) return {};
    const uint64_t elapsed = SDL_GetTicksNS() - message_start_ns_;
    if (elapsed >= kMessageHoldNs + kMessageFadeNs) {
        show_message({});  // faded out: drop it
        return {};
    }
    const float fade = elapsed <= kMessageHoldNs ? 0.0f
                                                 : static_cast<float>(elapsed - kMessageHoldNs) / static_cast<float>(kMessageFadeNs);
    // Whole-pixel scale: 1x, or the display scale (2x on a 200% / high-DPI screen).
    const int scale = std::max(1, static_cast<int>(SDL_GetWindowDisplayScale(window_) + kFitEpsilon));
    return {{kMessageMargin * scale, kMessageMargin * scale, message_w_ * scale, message_h_ * scale}, 1.0f - fade};
}

auto GameWindow::request_screenshot(std::string path) -> void {
    if (backend_) backend_->request_screenshot(std::move(path));
}

} // namespace bitforge::frontend
