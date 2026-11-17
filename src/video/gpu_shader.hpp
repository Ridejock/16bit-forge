// SPDX-License-Identifier: MIT
#pragma once
// Shader and pipeline helpers for GpuBackend, ported from Ervin Ashley's Emerald engine (MIT):
// LoadShader from src/Renderer/Shader.cpp and CreateGraphicsPipeline from
// src/Renderer/Pipeline.cpp, at Emerald commit 2952e05. Emerald's wrapper types (GpuDevice,
// GpuShader, ...) are replaced by the SDL types and its logger by utils::Logger; the loading
// rules, file layout and reflection handling are unchanged.
#include "video/filter_preset.hpp"

#include <SDL3/SDL.h>
#include <string_view>
#include <vector>

namespace bitforge::video {

// Loads shaders/<name>.<spv|dxil|msl> next to the executable (compiled at build time by
// SDL_shadercross, see cmake/Shaders.cmake), picking the format the device's backend takes.
// `name` ends in ".vert" or ".frag", which sets the stage. Resource counts come from the
// shadercross reflection file shaders/<name>.json. Null on failure (logged).
[[nodiscard]] auto load_shader(SDL_GPUDevice* device, std::string_view name) -> SDL_GPUShader*;

// How many uniform buffers the reflection file of shaders/<name> lists (0 if unknown).
[[nodiscard]] auto shader_uniform_buffers(std::string_view name) -> unsigned;
// How many samplers (texture inputs) the reflection file of shaders/<name> lists (0 if unknown).
[[nodiscard]] auto shader_samplers(std::string_view name) -> unsigned;

// The parameters in shaders/<shader>.params (see parse_shader_params); empty if there is none.
[[nodiscard]] auto load_shader_params(std::string_view shader) -> std::vector<FilterParam>;

// A pipeline for one fullscreen-triangle pass: no vertex buffers, no blending, no culling,
// one color target of `format`. Null on failure (logged).
[[nodiscard]] auto create_fullscreen_pipeline(SDL_GPUDevice* device, SDL_GPUShader* vertex, SDL_GPUShader* fragment,
                                              SDL_GPUTextureFormat format) -> SDL_GPUGraphicsPipeline*;

} // namespace bitforge::video
