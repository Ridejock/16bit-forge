// SPDX-License-Identifier: MIT
#include "video/renderer_backend.hpp"
#include "utils/logging.hpp"

#include <cstddef>
#include <vector>

namespace bitforge::video {

auto RendererBackend::create(SDL_Window* window) -> bool {
    destroy();
    renderer_ = SDL_CreateRenderer(window, nullptr);
    if (!renderer_) {
        utils::Logger::error(utils::LogChannel::Video, "SDL_CreateRenderer failed: %s", SDL_GetError());
        return false;
    }
    // The app paces frames itself; vsync here would stall the main window as well.
    SDL_SetRenderVSync(renderer_, 0);
    utils::Logger::info(utils::LogChannel::Video, "Game window renderer: %s", description().c_str());
    return true;
}

auto RendererBackend::destroy() -> void {
    if (texture_) SDL_DestroyTexture(texture_);
    for (Overlay& overlay : overlays_) {
        if (overlay.texture) SDL_DestroyTexture(overlay.texture);
        overlay = Overlay{};
    }
    if (renderer_) SDL_DestroyRenderer(renderer_);
    texture_ = nullptr;
    renderer_ = nullptr;
    tex_width_ = 0;
    tex_height_ = 0;
    has_frame_ = false;
}

auto RendererBackend::description() const -> std::string {
    const char* name = renderer_ ? SDL_GetRendererName(renderer_) : nullptr;
    return std::string("SDL_Renderer (") + (name ? name : "?") + ")";
}

auto RendererBackend::ensure_texture(int width, int height) -> bool {
    if (texture_ && width == tex_width_ && height == tex_height_) return true;
    if (texture_) SDL_DestroyTexture(texture_);
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!texture_) return false;
    SDL_SetTextureScaleMode(texture_, scale_mode_);
    tex_width_ = width;
    tex_height_ = height;
    return true;
}

auto RendererBackend::upload_frame(const uint32_t* pixels, int width, int height, int pitch) -> void {
    has_frame_ = renderer_ && pixels && ensure_texture(width, height) &&
                 SDL_UpdateTexture(texture_, nullptr, pixels, pitch);
}

auto RendererBackend::upload_overlay(OverlayLayer layer, const uint32_t* pixels, int width, int height) -> void {
    Overlay& overlay = overlays_[static_cast<size_t>(layer)];
    const bool keep = overlay.texture && pixels && width == overlay.w && height == overlay.h;
    if (!keep) {
        if (overlay.texture) SDL_DestroyTexture(overlay.texture);
        overlay = Overlay{};
        if (!renderer_ || !pixels || width <= 0 || height <= 0) return;
        overlay.texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, width, height);
        if (!overlay.texture) return;
        SDL_SetTextureBlendMode(overlay.texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(overlay.texture, SDL_SCALEMODE_NEAREST);
        overlay.w = width;
        overlay.h = height;
    }
    SDL_UpdateTexture(overlay.texture, nullptr, pixels, width * static_cast<int>(sizeof(uint32_t)));
}

auto RendererBackend::present(const Viewport& viewport, const OverlayPlacements& overlays) -> void {
    if (!renderer_) return;
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer_);
    if (has_frame_) {
        const SDL_FRect dst{static_cast<float>(viewport.x), static_cast<float>(viewport.y),
                            static_cast<float>(viewport.w), static_cast<float>(viewport.h)};
        SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
    }
    for (size_t i = 0; i < overlays_.size(); i++) {
        const OverlayPlacement& place = overlays[i];
        SDL_Texture* texture = overlays_[i].texture;
        if (!texture || place.opacity <= 0.0f) continue;
        const SDL_FRect dst{static_cast<float>(place.rect.x), static_cast<float>(place.rect.y), static_cast<float>(place.rect.w),
                            static_cast<float>(place.rect.h)};
        SDL_SetTextureAlphaModFloat(texture, place.opacity);
        SDL_RenderTexture(renderer_, texture, nullptr, &dst);
    }
    if (const auto path = take_screenshot_request()) {
        SDL_Surface* shot = SDL_RenderReadPixels(renderer_, nullptr);
        SDL_Surface* xrgb = shot ? SDL_ConvertSurface(shot, SDL_PIXELFORMAT_XRGB8888) : nullptr;
        if (xrgb) {
            // save_bmp wants tightly packed rows; SDL surfaces may pad them.
            std::vector<uint32_t> pixels(static_cast<size_t>(xrgb->w) * static_cast<size_t>(xrgb->h));
            for (int y = 0; y < xrgb->h; y++) {
                SDL_memcpy(&pixels[static_cast<size_t>(y) * static_cast<size_t>(xrgb->w)],
                           static_cast<const uint8_t*>(xrgb->pixels) + static_cast<ptrdiff_t>(y) * xrgb->pitch,
                           static_cast<size_t>(xrgb->w) * sizeof(uint32_t));
            }
            save_bmp(*path, pixels.data(), xrgb->w, xrgb->h);
        } else {
            utils::Logger::error(utils::LogChannel::Video, "Screenshot failed: %s", SDL_GetError());
        }
        SDL_DestroySurface(xrgb);
        SDL_DestroySurface(shot);
    }
    SDL_RenderPresent(renderer_);
}

auto RendererBackend::set_filter(const FilterPreset& preset) -> bool {
    const bool linear = !preset.passes.empty() && preset.passes.front().filter == FilterMode::Linear;
    scale_mode_ = linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST;
    if (texture_) SDL_SetTextureScaleMode(texture_, scale_mode_);
    return true;
}

} // namespace bitforge::video
