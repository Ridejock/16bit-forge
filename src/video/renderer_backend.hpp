// SPDX-License-Identifier: MIT
#pragma once
#include "video/video_backend.hpp"

namespace snes::video {

// The SDL_Renderer fallback: no shaders, so a preset only picks nearest or linear scaling
// (its first pass's filter). Used when SDL_GPU is unavailable or chosen in View > Renderer.
class RendererBackend final : public VideoBackend {
public:
    ~RendererBackend() override { destroy(); }

    auto create(SDL_Window* window) -> bool override;
    auto destroy() -> void override;
    auto upload_frame(const uint32_t* pixels, int width, int height, int pitch) -> void override;
    auto present(const Viewport& viewport) -> void override;
    auto set_filter(const FilterPreset& preset) -> bool override;
    auto kind() const -> BackendKind override { return BackendKind::Renderer; }
    auto description() const -> std::string override;

private:
    auto ensure_texture(int width, int height) -> bool;

    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int tex_width_ = 0;
    int tex_height_ = 0;
    bool has_frame_ = false;
    SDL_ScaleMode scale_mode_ = SDL_SCALEMODE_NEAREST;
};

} // namespace snes::video
