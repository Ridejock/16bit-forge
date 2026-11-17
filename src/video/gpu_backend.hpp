// SPDX-License-Identifier: MIT
#pragma once
#include "video/video_backend.hpp"

#include <array>
#include <vector>

namespace snes::video {

// SDL_GPU backend (Vulkan, Direct3D 12 or Metal, whichever SDL picks): uploads each frame to a
// texture and runs the preset's passes, each a fullscreen triangle with its own fragment shader,
// through intermediate render targets. The last pass draws into the letterboxed viewport. A pass
// reads the previous pass's output, plus any extra inputs it names (the emulator frame or an
// earlier pass's output), and gets the preset's values for its shader's parameters.
// Device, swapchain, upload, pass-chain and screenshot code follow Emerald's Renderer,
// Texture and PostChain (MIT, Ervin Ashley); see gpu_backend.cpp.
class GpuBackend final : public VideoBackend {
public:
    ~GpuBackend() override { destroy(); }

    auto create(SDL_Window* window) -> bool override;
    auto destroy() -> void override;
    auto upload_frame(const uint32_t* pixels, int width, int height, int pitch) -> void override;
    auto present(const Viewport& viewport) -> void override;
    auto set_filter(const FilterPreset& preset) -> bool override;
    auto kind() const -> BackendKind override { return BackendKind::Gpu; }
    auto description() const -> std::string override;

private:
    struct Pass {
        FilterPass desc;
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
        bool has_uniforms = false;
        std::array<float, kMaxShaderParams> params{};  // FilterUniforms.Parameters
        SDL_GPUTexture* target = nullptr;  // intermediate output; null for the last pass
        Uint32 target_w = 0;
        Uint32 target_h = 0;
    };

    auto build_passes(const FilterPreset& preset) -> bool;
    auto release_passes() -> void;
    auto ensure_source(Uint32 width, Uint32 height) -> bool;
    auto ensure_target(Pass& pass, Uint32 width, Uint32 height) -> bool;
    auto input_texture(const std::string& input) const -> SDL_GPUTexture*;
    auto ensure_capture(Uint32 width, Uint32 height) -> bool;
    auto run_passes(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* output, const Viewport& viewport) -> void;
    auto finish_screenshot(SDL_GPUFence* fence, const std::string& path) -> void;

    SDL_GPUDevice* device_ = nullptr;
    SDL_Window* window_ = nullptr;
    SDL_GPUShader* vertex_shader_ = nullptr;
    SDL_GPUSampler* nearest_ = nullptr;
    SDL_GPUSampler* linear_ = nullptr;
    SDL_GPUTextureFormat swapchain_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    std::vector<Pass> passes_;

    // The emulator frame: CPU -> transfer buffer -> source texture (in present()).
    SDL_GPUTransferBuffer* upload_ = nullptr;
    SDL_GPUTexture* source_ = nullptr;
    Uint32 source_w_ = 0;
    Uint32 source_h_ = 0;
    bool upload_pending_ = false;
    bool has_frame_ = false;
    Uint32 frame_count_ = 0;

    // Screenshots: render into this instead of the swapchain, then download and blit.
    SDL_GPUTexture* capture_ = nullptr;
    SDL_GPUTransferBuffer* capture_download_ = nullptr;
    Uint32 capture_w_ = 0;
    Uint32 capture_h_ = 0;
};

} // namespace snes::video
