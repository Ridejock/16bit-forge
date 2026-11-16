// SPDX-License-Identifier: MIT
// Ported from Emerald (MIT, Ervin Ashley): src/Renderer/Shader.cpp and src/Renderer/Pipeline.cpp.
#include "video/gpu_shader.hpp"
#include "utils/logging.hpp"

#include <array>
#include <charconv>
#include <string>
#include <system_error>

namespace snes::video {

namespace {

// One compiled variant of a shader, as produced by cmake/Shaders.cmake.
struct ShaderVariant {
    SDL_GPUShaderFormat format;
    const char* extension;
    const char* entry_point;
};

// Tried in order; the first one the device accepts and that exists on disk wins.
// SPIRV-Cross renames `main` to `main0` when generating MSL (`main` is reserved in Metal).
constexpr std::array<ShaderVariant, 3> kVariants{{
    {SDL_GPU_SHADERFORMAT_SPIRV, "spv", "main"},
    {SDL_GPU_SHADERFORMAT_DXIL, "dxil", "main"},
    {SDL_GPU_SHADERFORMAT_MSL, "msl", "main0"},
}};

struct ResourceCounts {
    Uint32 samplers = 0;
    Uint32 storage_textures = 0;
    Uint32 storage_buffers = 0;
    Uint32 uniform_buffers = 0;
};

auto base_path(std::string_view name) -> std::string {
    const char* exe_dir = SDL_GetBasePath();  // null if the platform can't tell
    return std::string(exe_dir ? exe_dir : "") + "shaders/" + std::string(name);
}

// Tiny helper to read `"key": 123` from the shadercross reflection JSON. The file is a single
// flat object written by shadercross, so a full JSON parser is not needed here.
auto read_json_count(std::string_view json, std::string_view key, Uint32& out) -> bool {
    const std::string quoted = "\"" + std::string(key) + "\"";
    size_t pos = json.find(quoted);
    if (pos == std::string_view::npos) return false;
    pos = json.find(':', pos + quoted.size());
    if (pos == std::string_view::npos) return false;
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string_view::npos) return false;
    return std::from_chars(json.data() + pos, json.data() + json.size(), out).ec == std::errc{};
}

auto read_reflection(std::string_view name, ResourceCounts& counts) -> bool {
    const std::string path = base_path(name) + ".json";
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data) {
        utils::Logger::warning(utils::LogChannel::Video, "load_shader: no reflection file '%s'", path.c_str());
        return false;
    }
    const std::string_view json(static_cast<const char*>(data), size);
    read_json_count(json, "samplers", counts.samplers);
    read_json_count(json, "storage_textures", counts.storage_textures);
    read_json_count(json, "storage_buffers", counts.storage_buffers);
    read_json_count(json, "uniform_buffers", counts.uniform_buffers);
    SDL_free(data);
    return true;
}

} // namespace

auto shader_uniform_buffers(std::string_view name) -> unsigned {
    ResourceCounts counts;
    read_reflection(name, counts);
    return counts.uniform_buffers;
}

auto load_shader(SDL_GPUDevice* device, std::string_view name) -> SDL_GPUShader* {
    SDL_GPUShaderStage stage;
    if (name.ends_with(".vert")) {
        stage = SDL_GPU_SHADERSTAGE_VERTEX;
    } else if (name.ends_with(".frag")) {
        stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    } else {
        utils::Logger::error(utils::LogChannel::Video, "load_shader: cannot infer stage of '%.*s'", static_cast<int>(name.size()), name.data());
        return nullptr;
    }

    const std::string path_base = base_path(name);
    ResourceCounts counts;
    read_reflection(name, counts);

    // Which bytecode formats does this device's backend understand?
    const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(device);
    for (const ShaderVariant& variant : kVariants) {
        if (!(supported & variant.format)) continue;

        const std::string path = path_base + "." + variant.extension;
        size_t code_size = 0;
        void* code = SDL_LoadFile(path.c_str(), &code_size);
        if (!code) {
            utils::Logger::warning(utils::LogChannel::Video, "load_shader: %s not found, trying other formats", path.c_str());
            continue;
        }

        SDL_GPUShaderCreateInfo info{};
        info.code = static_cast<const Uint8*>(code);
        info.code_size = code_size;
        info.entrypoint = variant.entry_point;
        info.format = variant.format;
        info.stage = stage;
        info.num_samplers = counts.samplers;
        info.num_storage_textures = counts.storage_textures;
        info.num_storage_buffers = counts.storage_buffers;
        info.num_uniform_buffers = counts.uniform_buffers;

        SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
        SDL_free(code);
        if (!shader) utils::Logger::error(utils::LogChannel::Video, "SDL_CreateGPUShader(%s) failed: %s", path.c_str(), SDL_GetError());
        return shader;
    }

    utils::Logger::error(utils::LogChannel::Video, "load_shader: no usable variant of '%s' for this GPU backend", path_base.c_str());
    return nullptr;
}

auto create_fullscreen_pipeline(SDL_GPUDevice* device, SDL_GPUShader* vertex, SDL_GPUShader* fragment,
                                SDL_GPUTextureFormat format) -> SDL_GPUGraphicsPipeline* {
    SDL_GPUColorTargetDescription color_target{};
    color_target.format = format;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // draw both windings
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets = 1;
    info.target_info.has_depth_stencil_target = false;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    if (!pipeline) utils::Logger::error(utils::LogChannel::Video, "SDL_CreateGPUGraphicsPipeline failed: %s", SDL_GetError());
    return pipeline;
}

} // namespace snes::video
