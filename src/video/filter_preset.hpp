// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace snes::video {

// How a pass's output size is chosen (see shaders/presets.ini).
enum class ScaleType { Source, Viewport };
// How a pass samples its inputs.
enum class FilterMode { Nearest, Linear };

// The name a pass uses to read the emulator frame itself as an extra input.
inline constexpr std::string_view kOriginalInput = "original";

struct FilterPass {
    std::string shader;  // fragment shader base name: "xbr_lv2" -> shaders/xbr_lv2.frag.*
    ScaleType scale_type = ScaleType::Viewport;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    FilterMode filter = FilterMode::Nearest;
    bool float_target = false;        // 16-bit float output instead of 8-bit (signals, metrics)
    std::string name;                 // lets later passes read this pass's output ("" = none)
    std::vector<std::string> inputs;  // extra inputs, bound from slot 1: "original" or a pass name
};

// One "param = name, value" line: sets that parameter in every pass whose shader declares it.
struct FilterParam {
    std::string name;
    float value = 0.0f;
};

struct FilterPreset {
    std::string name;
    std::vector<FilterPass> passes;  // never empty once loaded
    std::vector<FilterParam> params;
};

// Parses the presets file. Bad lines and empty presets are skipped with a log line. Returns
// the built-in nearest preset alone if the file is missing or has no valid preset.
auto load_presets(const std::string& path) -> std::vector<FilterPreset>;
// The same, from text already in memory (`origin` names it in log lines).
auto parse_presets(std::string_view text, const std::string& origin) -> std::vector<FilterPreset>;

// One stock pass with nearest sampling: what the GPU backend shows when a preset fails.
auto default_preset() -> FilterPreset;

// The preset called `name`, else the first one. `presets` must not be empty.
auto find_preset(const std::vector<FilterPreset>& presets, const std::string& name) -> const FilterPreset&;

// A shader's tunable parameters, in uniform slot order, with their defaults. CMake writes them
// to shaders/<shader>.params from the shader's "// @param <name> <default>" lines.
inline constexpr size_t kMaxShaderParams = 16;  // FilterUniforms.Parameters in filter_common.hlsli
auto parse_shader_params(std::string_view text) -> std::vector<FilterParam>;

} // namespace snes::video
