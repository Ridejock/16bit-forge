// SPDX-License-Identifier: MIT
// SDL_GPU video backend. Reuses Ervin Ashley's Emerald engine rendering code (MIT), at commit
// 2952e05, adapted from its wrapper types to plain SDL:
//   - device creation, window claim and present-mode choice: src/Renderer/Renderer.cpp
//     (Renderer::Init, ChoosePresentMode)
//   - texture upload through a transfer buffer and copy pass: src/Renderer/Texture.cpp
//   - render-target creation and the pass-to-pass chain: src/Renderer/PostChain.cpp (here every
//     pass gets its own target, since passes can change the size)
//   - screenshot capture (render offscreen, download, blit to the swapchain, wait on a fence):
//     src/Renderer/Renderer.cpp (BeginFrame/EndFrame/FinishScreenshot)
//   - shaders and pipelines: video/gpu_shader.cpp; shaders/fullscreen.vert.hlsl
#include "video/gpu_backend.hpp"
#include "utils/logging.hpp"
#include "video/gpu_shader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifndef EMERALD_SHADER_FORMATS  // set by emerald_add_shaders() in cmake/Shaders.cmake
#define EMERALD_SHADER_FORMATS (SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL)
#endif

namespace snes::video {

namespace {

constexpr const char* kVertexShader = "fullscreen.vert";
constexpr const char* kFragmentSuffix = ".frag";
// Emulator frames are XRGB8888 in memory order B, G, R, X.
constexpr SDL_GPUTextureFormat kSourceFormat = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
constexpr SDL_GPUTextureFormat kIntermediateFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr SDL_GPUTextureFormat kFloatFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;  // "float" passes
constexpr size_t kMaxPassInputs = 8;  // the previous pass plus extra inputs
constexpr Uint32 kBytesPerPixel = 4;
constexpr Uint32 kMaxTargetSide = 16384;  // SDL_GPU's guaranteed limit on every backend

// Matches FilterUniforms in shaders/filter_common.hlsli (std140-compatible layout).
struct FilterUniforms {
    float source_size[4];
    float output_size[4];
    float original_size[4];
    Uint32 frame_count;
    Uint32 padding[3];
    float parameters[kMaxShaderParams];
};

auto size_vec(Uint32 w, Uint32 h, float (&out)[4]) -> void {
    out[0] = static_cast<float>(w);
    out[1] = static_cast<float>(h);
    out[2] = 1.0f / static_cast<float>(std::max(w, 1u));
    out[3] = 1.0f / static_cast<float>(std::max(h, 1u));
}

// From Emerald's Renderer.cpp: without vsync prefer MAILBOX (no tearing), then IMMEDIATE.
// The app paces frames itself, like the SDL_Renderer path, so vsync is off.
auto choose_present_mode(SDL_GPUDevice* device, SDL_Window* window) -> SDL_GPUPresentMode {
    if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX)) return SDL_GPU_PRESENTMODE_MAILBOX;
    if (SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_IMMEDIATE)) return SDL_GPU_PRESENTMODE_IMMEDIATE;
    return SDL_GPU_PRESENTMODE_VSYNC;
}

auto create_sampler(SDL_GPUDevice* device, SDL_GPUFilter filter) -> SDL_GPUSampler* {
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = filter;
    info.mag_filter = filter;
    info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    return SDL_CreateGPUSampler(device, &info);
}

// From Emerald's PostChain.cpp (CreateColorTarget).
auto create_texture(SDL_GPUDevice* device, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, Uint32 width,
                    Uint32 height) -> SDL_GPUTexture* {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = std::max(width, 1u);
    info.height = std::max(height, 1u);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    return SDL_CreateGPUTexture(device, &info);
}

auto create_transfer(SDL_GPUDevice* device, SDL_GPUTransferBufferUsage usage, Uint32 size) -> SDL_GPUTransferBuffer* {
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = usage;
    info.size = size;
    return SDL_CreateGPUTransferBuffer(device, &info);
}

auto scaled(Uint32 size, float scale) -> Uint32 {
    const float v = std::round(static_cast<float>(size) * scale);
    return std::clamp(static_cast<Uint32>(std::max(v, 1.0f)), 1u, kMaxTargetSide);
}

} // namespace

auto GpuBackend::create(SDL_Window* window) -> bool {
    destroy();
#ifdef NDEBUG
    constexpr bool debug_mode = false;
#else
    constexpr bool debug_mode = true;  // validation layers / extra checks where available
#endif
    // SDL only picks a backend that can consume one of the shader formats we ship:
    // SPIR-V -> Vulkan, DXIL -> Direct3D 12, MSL -> Metal. SDL_GPU_DRIVER forces one.
    device_ = SDL_CreateGPUDevice(EMERALD_SHADER_FORMATS, debug_mode, nullptr);
    if (!device_) {
        utils::Logger::warning(utils::LogChannel::Video, "SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return false;
    }
    // Claiming the window creates its swapchain.
    if (!SDL_ClaimWindowForGPUDevice(device_, window)) {
        utils::Logger::warning(utils::LogChannel::Video, "SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        destroy();
        return false;
    }
    window_ = window;
    if (!SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                       choose_present_mode(device_, window_))) {
        utils::Logger::warning(utils::LogChannel::Video, "SDL_SetGPUSwapchainParameters failed: %s", SDL_GetError());
    }
    swapchain_format_ = SDL_GetGPUSwapchainTextureFormat(device_, window_);

    vertex_shader_ = load_shader(device_, kVertexShader);
    nearest_ = create_sampler(device_, SDL_GPU_FILTER_NEAREST);
    linear_ = create_sampler(device_, SDL_GPU_FILTER_LINEAR);
    // A device without our shaders is no use: let the caller fall back to SDL_Renderer.
    if (!vertex_shader_ || !nearest_ || !linear_ || !build_passes(default_preset())) {
        utils::Logger::warning(utils::LogChannel::Video, "SDL_GPU backend unusable (shaders missing or invalid)");
        destroy();
        return false;
    }

    const SDL_PropertiesID props = SDL_GetGPUDeviceProperties(device_);
    utils::Logger::info(utils::LogChannel::Video, "Game window renderer: %s, device '%s'", description().c_str(),
                        SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_NAME_STRING, "?"));
    return true;
}

auto GpuBackend::destroy() -> void {
    if (!device_) return;
    // Resources may still be in use by in-flight frames; wait before destroying anything.
    SDL_WaitForGPUIdle(device_);
    release_passes();
    for (SDL_GPUTexture* texture : {source_, capture_}) {
        if (texture) SDL_ReleaseGPUTexture(device_, texture);
    }
    for (SDL_GPUTransferBuffer* buffer : {upload_, capture_download_}) {
        if (buffer) SDL_ReleaseGPUTransferBuffer(device_, buffer);
    }
    for (SDL_GPUSampler* sampler : {nearest_, linear_}) {
        if (sampler) SDL_ReleaseGPUSampler(device_, sampler);
    }
    if (vertex_shader_) SDL_ReleaseGPUShader(device_, vertex_shader_);
    if (window_) SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
    device_ = nullptr;
    window_ = nullptr;
    vertex_shader_ = nullptr;
    nearest_ = linear_ = nullptr;
    source_ = capture_ = nullptr;
    upload_ = capture_download_ = nullptr;
    source_w_ = source_h_ = capture_w_ = capture_h_ = 0;
    upload_pending_ = has_frame_ = false;
}

auto GpuBackend::description() const -> std::string {
    const char* driver = device_ ? SDL_GetGPUDeviceDriver(device_) : nullptr;
    return std::string("SDL_GPU (") + (driver ? driver : "?") + ")";
}

auto GpuBackend::release_passes() -> void {
    for (Pass& pass : passes_) {
        if (pass.pipeline) SDL_ReleaseGPUGraphicsPipeline(device_, pass.pipeline);
        if (pass.target) SDL_ReleaseGPUTexture(device_, pass.target);
    }
    passes_.clear();
}

auto GpuBackend::build_passes(const FilterPreset& preset) -> bool {
    SDL_WaitForGPUIdle(device_);
    release_passes();
    bool ok = !preset.passes.empty();
    std::vector<std::string> declared;  // every parameter the preset's shaders take
    for (size_t i = 0; ok && i < preset.passes.size(); i++) {
        Pass pass;
        pass.desc = preset.passes[i];
        const std::string name = pass.desc.shader + kFragmentSuffix;
        // The shader must read exactly the inputs the preset gives it: the previous pass + extras.
        const size_t inputs = 1 + pass.desc.inputs.size();
        if (inputs > kMaxPassInputs || shader_samplers(name) != inputs) {
            utils::Logger::error(utils::LogChannel::Video, "Filter '%s': shader %s does not take %zu input%s",
                                 preset.name.c_str(), pass.desc.shader.c_str(), inputs, inputs == 1 ? "" : "s");
            ok = false;
            continue;  // ends the loop
        }
        // Parameters: the shader's defaults, overridden by the preset's values.
        const auto params = load_shader_params(pass.desc.shader);
        for (size_t p = 0; p < params.size(); p++) {
            declared.push_back(params[p].name);
            pass.params[p] = params[p].value;
            for (const FilterParam& set : preset.params) {
                if (set.name == params[p].name) pass.params[p] = set.value;
            }
        }
        SDL_GPUShader* fragment = load_shader(device_, name);
        if (fragment) {
            // The last pass draws into the swapchain (or the same-format capture texture).
            const bool last = i + 1 == preset.passes.size();
            const SDL_GPUTextureFormat format = last ? swapchain_format_
                                                     : (pass.desc.float_target ? kFloatFormat : kIntermediateFormat);
            pass.pipeline = create_fullscreen_pipeline(device_, vertex_shader_, fragment, format);
            pass.has_uniforms = shader_uniform_buffers(name) > 0;
            SDL_ReleaseGPUShader(device_, fragment);  // the pipeline keeps what it needs
        }
        ok = pass.pipeline != nullptr;
        passes_.push_back(pass);
    }
    if (!ok) {
        utils::Logger::error(utils::LogChannel::Video, "Filter '%s' could not be built", preset.name.c_str());
        release_passes();
    }
    for (const FilterParam& set : preset.params) {  // most likely a typo in presets.ini
        if (ok && std::find(declared.begin(), declared.end(), set.name) == declared.end()) {
            utils::Logger::warning(utils::LogChannel::Video, "Filter '%s': no pass takes parameter '%s'", preset.name.c_str(),
                                   set.name.c_str());
        }
    }
    return ok;
}

auto GpuBackend::set_filter(const FilterPreset& preset) -> bool {
    if (!device_) return false;
    if (build_passes(preset)) {
        utils::Logger::info(utils::LogChannel::Video, "Filter: %s (%zu pass%s)", preset.name.c_str(), passes_.size(),
                            passes_.size() == 1 ? "" : "es");
        return true;
    }
    build_passes(default_preset());
    return false;
}

auto GpuBackend::ensure_source(Uint32 width, Uint32 height) -> bool {
    if (source_ && width == source_w_ && height == source_h_) return true;
    SDL_WaitForGPUIdle(device_);
    if (source_) SDL_ReleaseGPUTexture(device_, source_);
    if (upload_) SDL_ReleaseGPUTransferBuffer(device_, upload_);
    source_ = create_texture(device_, kSourceFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER, width, height);
    upload_ = create_transfer(device_, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, width * height * kBytesPerPixel);
    if (!source_ || !upload_) {
        utils::Logger::error(utils::LogChannel::Video, "GpuBackend: could not create the %ux%u frame texture: %s", width, height, SDL_GetError());
        return false;
    }
    source_w_ = width;
    source_h_ = height;
    return true;
}

auto GpuBackend::upload_frame(const uint32_t* pixels, int width, int height, int pitch) -> void {
    has_frame_ = false;
    if (!device_ || !pixels || width <= 0 || height <= 0) return;
    const auto w = static_cast<Uint32>(width);
    const auto h = static_cast<Uint32>(height);
    if (!ensure_source(w, h)) return;

    // Cycle: the previous frame's buffer may still be in flight.
    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, upload_, true));
    if (!mapped) return;
    const size_t row_bytes = static_cast<size_t>(w) * kBytesPerPixel;
    const auto* src = reinterpret_cast<const uint8_t*>(pixels);
    for (Uint32 y = 0; y < h; y++) {
        std::memcpy(mapped + y * row_bytes, src + static_cast<size_t>(y) * static_cast<size_t>(pitch), row_bytes);
    }
    SDL_UnmapGPUTransferBuffer(device_, upload_);
    upload_pending_ = true;
    has_frame_ = true;
}

auto GpuBackend::ensure_target(Pass& pass, Uint32 width, Uint32 height) -> bool {
    if (pass.target && pass.target_w == width && pass.target_h == height) return true;
    if (pass.target) SDL_ReleaseGPUTexture(device_, pass.target);  // deferred by SDL until unused
    pass.target = create_texture(device_, pass.desc.float_target ? kFloatFormat : kIntermediateFormat,
                                 SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, width, height);
    pass.target_w = width;
    pass.target_h = height;
    return pass.target != nullptr;
}

// An extra pass input: the emulator frame, or the output of the earlier pass with that name.
auto GpuBackend::input_texture(const std::string& input) const -> SDL_GPUTexture* {
    if (input == kOriginalInput) return source_;
    for (const Pass& pass : passes_) {
        if (pass.desc.name == input) return pass.target;
    }
    return nullptr;  // not reached: parse_presets only accepts earlier passes' names
}

auto GpuBackend::ensure_capture(Uint32 width, Uint32 height) -> bool {
    if (capture_ && capture_w_ == width && capture_h_ == height) return true;
    if (capture_) SDL_ReleaseGPUTexture(device_, capture_);
    if (capture_download_) SDL_ReleaseGPUTransferBuffer(device_, capture_download_);
    // Same format as the swapchain, so the last pass's pipeline can draw into it.
    capture_ = create_texture(device_, swapchain_format_,
                              SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, width, height);
    capture_download_ = create_transfer(device_, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, width * height * kBytesPerPixel);
    capture_w_ = width;
    capture_h_ = height;
    return capture_ && capture_download_;
}

auto GpuBackend::run_passes(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* output, const Viewport& viewport) -> void {
    const bool draw = has_frame_ && !passes_.empty() && viewport.w > 0 && viewport.h > 0;
    SDL_GPUTexture* input = source_;
    Uint32 in_w = source_w_;
    Uint32 in_h = source_h_;
    const auto view_w = static_cast<Uint32>(std::max(viewport.w, 1));
    const auto view_h = static_cast<Uint32>(std::max(viewport.h, 1));

    for (size_t i = 0; draw && i < passes_.size(); i++) {
        Pass& pass = passes_[i];
        const bool last = i + 1 == passes_.size();
        Uint32 out_w = view_w;
        Uint32 out_h = view_h;
        if (!last) {
            const bool by_source = pass.desc.scale_type == ScaleType::Source;
            out_w = scaled(by_source ? in_w : view_w, pass.desc.scale_x);
            out_h = scaled(by_source ? in_h : view_h, pass.desc.scale_y);
            if (!ensure_target(pass, out_w, out_h)) {
                utils::Logger::error(utils::LogChannel::Video, "GpuBackend: could not create a %ux%u pass target", out_w, out_h);
                return;
            }
        }

        SDL_GPUColorTargetInfo color{};
        color.texture = last ? output : pass.target;
        color.load_op = last ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_DONT_CARE;  // last: black borders
        color.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* render = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(render, pass.pipeline);
        if (last) {
            const SDL_GPUViewport vp{static_cast<float>(viewport.x), static_cast<float>(viewport.y),
                                     static_cast<float>(viewport.w), static_cast<float>(viewport.h), 0.0f, 1.0f};
            SDL_SetGPUViewport(render, &vp);
        }
        // Slot 0: the previous pass (or the frame); then the extra inputs, all with the pass's filter.
        SDL_GPUSampler* sampler = pass.desc.filter == FilterMode::Linear ? linear_ : nearest_;
        SDL_GPUTextureSamplerBinding bindings[kMaxPassInputs]{{input, sampler}};
        const size_t inputs = 1 + pass.desc.inputs.size();
        for (size_t k = 1; k < inputs; k++) bindings[k] = {input_texture(pass.desc.inputs[k - 1]), sampler};
        SDL_BindGPUFragmentSamplers(render, 0, bindings, static_cast<Uint32>(inputs));
        if (pass.has_uniforms) {
            FilterUniforms uniforms{};
            size_vec(in_w, in_h, uniforms.source_size);
            size_vec(out_w, out_h, uniforms.output_size);
            size_vec(source_w_, source_h_, uniforms.original_size);
            uniforms.frame_count = frame_count_;
            std::copy(pass.params.begin(), pass.params.end(), uniforms.parameters);
            SDL_PushGPUFragmentUniformData(cmd, 0, &uniforms, static_cast<Uint32>(sizeof(uniforms)));
        }
        SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
        SDL_EndGPURenderPass(render);

        input = pass.target;
        in_w = out_w;
        in_h = out_h;
    }

    if (!draw) {  // no picture: just clear to black
        SDL_GPUColorTargetInfo color{};
        color.texture = output;
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &color, 1, nullptr));
    }
}

auto GpuBackend::present(const Viewport& viewport) -> void {
    if (!device_) return;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        utils::Logger::error(utils::LogChannel::Video, "SDL_AcquireGPUCommandBuffer failed: %s", SDL_GetError());
        return;
    }

    if (upload_pending_) {
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = upload_;
        src.pixels_per_row = source_w_;  // tightly packed rows
        src.rows_per_layer = source_h_;
        SDL_GPUTextureRegion dst{};
        dst.texture = source_;
        dst.w = source_w_;
        dst.h = source_h_;
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
        SDL_EndGPUCopyPass(copy);
        upload_pending_ = false;
    }

    SDL_GPUTexture* swapchain = nullptr;
    Uint32 sw_w = 0;
    Uint32 sw_h = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swapchain, &sw_w, &sw_h)) {
        utils::Logger::error(utils::LogChannel::Video, "SDL_WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }
    // A null texture is not an error: the window is minimized/occluded. Every acquired
    // command buffer must still be submitted (the upload above goes through).
    if (!swapchain) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }

    auto screenshot = take_screenshot_request();
    if (screenshot && !ensure_capture(sw_w, sw_h)) {
        utils::Logger::error(utils::LogChannel::Video, "Screenshot: could not create capture resources: %s", SDL_GetError());
        screenshot.reset();
    }

    run_passes(cmd, screenshot ? capture_ : swapchain, viewport);
    frame_count_++;

    if (!screenshot) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }
    // Screenshot frame: copy the offscreen image to the CPU and to the swapchain.
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region{};
    region.texture = capture_;
    region.w = sw_w;
    region.h = sw_h;
    region.d = 1;
    SDL_GPUTextureTransferInfo download{};
    download.transfer_buffer = capture_download_;
    SDL_DownloadFromGPUTexture(copy, &region, &download);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUBlitInfo blit{};
    blit.source.texture = capture_;
    blit.source.w = sw_w;
    blit.source.h = sw_h;
    blit.destination.texture = swapchain;
    blit.destination.w = sw_w;
    blit.destination.h = sw_h;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(cmd, &blit);
    // A fence lets the CPU wait until the GPU has actually finished the download.
    finish_screenshot(SDL_SubmitGPUCommandBufferAndAcquireFence(cmd), *screenshot);
}

auto GpuBackend::finish_screenshot(SDL_GPUFence* fence, const std::string& path) -> void {
    if (!fence) {
        utils::Logger::error(utils::LogChannel::Video, "Screenshot submit failed: %s", SDL_GetError());
        return;
    }
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);

    const bool bgra = swapchain_format_ == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
                      swapchain_format_ == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    const bool rgba = swapchain_format_ == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM ||
                      swapchain_format_ == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    if (!bgra && !rgba) {
        utils::Logger::error(utils::LogChannel::Video, "Screenshot: unsupported swapchain format %d", static_cast<int>(swapchain_format_));
        return;
    }
    const auto* src = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(device_, capture_download_, false));
    if (!src) return;
    std::vector<uint32_t> pixels(static_cast<size_t>(capture_w_) * capture_h_);
    for (size_t i = 0; i < pixels.size(); i++) {
        const uint8_t* p = src + i * kBytesPerPixel;
        const uint32_t r = p[bgra ? 2 : 0];
        const uint32_t b = p[bgra ? 0 : 2];
        pixels[i] = (r << 16) | (static_cast<uint32_t>(p[1]) << 8) | b;
    }
    SDL_UnmapGPUTransferBuffer(device_, capture_download_);
    save_bmp(path, pixels.data(), static_cast<int>(capture_w_), static_cast<int>(capture_h_));
}

} // namespace snes::video
