// SPDX-License-Identifier: MIT
#include "frontend/game_window.hpp"
#include "utils/logging.hpp"

#include <algorithm>
#include <cmath>

namespace snes::frontend {

namespace {

constexpr float kPixelAspect87 = 8.0f / 7.0f;
constexpr float kFitEpsilon = 1e-3f;  // absorbs float error before integer flooring

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

auto GameWindow::open(const char* title, const ViewOptions& view, video::BackendKind wanted,
                      const video::FilterPreset& filter) -> bool {
    if (window_) return true;

    window_ = SDL_CreateWindow(title, window_width(view), kBaseHeight * view.scale,
                               SDL_WINDOW_RESIZABLE);
    if (!window_) return false;
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
    backend_->present(fit_viewport(out_w, out_h, view, height > 0 ? picture_lines(height) : kBaseHeight));
}

auto GameWindow::request_screenshot(std::string path) -> void {
    if (backend_) backend_->request_screenshot(std::move(path));
}

} // namespace snes::frontend
