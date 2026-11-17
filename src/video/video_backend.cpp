// SPDX-License-Identifier: MIT
#include "video/video_backend.hpp"
#include "utils/logging.hpp"
#include "video/gpu_backend.hpp"
#include "video/renderer_backend.hpp"

namespace bitforge::video {

auto backend_kind_name(BackendKind kind) -> const char* {
    return kind == BackendKind::Gpu ? "gpu" : "sdl";
}

auto parse_backend_kind(std::string_view name) -> std::optional<BackendKind> {
    if (name == "gpu") return BackendKind::Gpu;
    if (name == "sdl") return BackendKind::Renderer;
    return std::nullopt;
}

auto make_backend(BackendKind kind) -> std::unique_ptr<VideoBackend> {
    if (kind == BackendKind::Gpu) return std::make_unique<GpuBackend>();
    return std::make_unique<RendererBackend>();
}

auto save_bmp(const std::string& path, const uint32_t* pixels, int width, int height) -> bool {
    SDL_Surface* surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_XRGB8888, const_cast<uint32_t*>(pixels),
                                                 width * static_cast<int>(sizeof(uint32_t)));
    const bool ok = surface && SDL_SaveBMP(surface, path.c_str());
    if (ok) {
        utils::Logger::info(utils::LogChannel::Video, "Screenshot saved to %s (%dx%d)", path.c_str(), width, height);
    } else {
        utils::Logger::error(utils::LogChannel::Video, "Screenshot %s failed: %s", path.c_str(), SDL_GetError());
    }
    SDL_DestroySurface(surface);
    return ok;
}

} // namespace bitforge::video
