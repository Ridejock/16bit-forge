// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <vector>

namespace snes::video {

// How a pass's output size is chosen (see shaders/presets.ini).
enum class ScaleType { Source, Viewport };
// How a pass samples its input.
enum class FilterMode { Nearest, Linear };

struct FilterPass {
    std::string shader;  // fragment shader base name: "xbr_lv2" -> shaders/xbr_lv2.frag.*
    ScaleType scale_type = ScaleType::Viewport;
    float scale = 1.0f;
    FilterMode filter = FilterMode::Nearest;
};

struct FilterPreset {
    std::string name;
    std::vector<FilterPass> passes;  // never empty once loaded
};

// Parses the presets file. Bad lines and empty presets are skipped with a log line. Returns
// the built-in nearest preset alone if the file is missing or has no valid preset.
auto load_presets(const std::string& path) -> std::vector<FilterPreset>;

// One stock pass with nearest sampling: what the GPU backend shows when a preset fails.
auto default_preset() -> FilterPreset;

// The preset called `name`, else the first one. `presets` must not be empty.
auto find_preset(const std::vector<FilterPreset>& presets, const std::string& name) -> const FilterPreset&;

} // namespace snes::video
