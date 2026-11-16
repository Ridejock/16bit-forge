// SPDX-License-Identifier: MIT
#include "video/filter_preset.hpp"
#include "utils/logging.hpp"

#include <fstream>
#include <string_view>

namespace snes::video {

namespace {

constexpr std::string_view kPassKey = "pass";
constexpr size_t kPassFields = 4;  // shader, scale type, scale, filter

auto trim(std::string_view s) -> std::string_view {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r");
    return s.substr(first, last - first + 1);
}

auto split_fields(std::string_view value) -> std::vector<std::string_view> {
    std::vector<std::string_view> fields;
    while (true) {
        const auto comma = value.find(',');
        fields.push_back(trim(value.substr(0, comma)));
        if (comma == std::string_view::npos) return fields;
        value.remove_prefix(comma + 1);
    }
}

auto parse_pass(std::string_view value, FilterPass& pass) -> bool {
    const auto fields = split_fields(value);
    if (fields.size() != kPassFields || fields[0].empty()) return false;

    pass.shader = std::string(fields[0]);

    if (fields[1] == "source") {
        pass.scale_type = ScaleType::Source;
    } else if (fields[1] == "viewport") {
        pass.scale_type = ScaleType::Viewport;
    } else {
        return false;
    }

    const std::string scale(fields[2]);
    try {
        pass.scale = std::stof(scale);
    } catch (...) {
        return false;
    }
    if (!(pass.scale > 0.0f)) return false;

    if (fields[3] == "nearest") {
        pass.filter = FilterMode::Nearest;
    } else if (fields[3] == "linear") {
        pass.filter = FilterMode::Linear;
    } else {
        return false;
    }
    return true;
}

} // namespace

auto default_preset() -> FilterPreset {
    return {"nearest", {{"stock", ScaleType::Viewport, 1.0f, FilterMode::Nearest}}};
}

auto load_presets(const std::string& path) -> std::vector<FilterPreset> {
    std::vector<FilterPreset> presets;
    std::ifstream in(path);
    if (!in) utils::Logger::warning(utils::LogChannel::Video, "No filter presets at %s; only 'nearest' is available", path.c_str());

    std::string raw;
    int line_no = 0;
    while (std::getline(in, raw)) {
        line_no++;
        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            presets.push_back({std::string(trim(line.substr(1, line.size() - 2))), {}});
            continue;
        }
        const auto eq = line.find('=');
        FilterPass pass;
        if (presets.empty() || eq == std::string_view::npos || trim(line.substr(0, eq)) != kPassKey ||
            !parse_pass(line.substr(eq + 1), pass)) {
            utils::Logger::warning(utils::LogChannel::Video, "%s:%d: ignored '%.*s'", path.c_str(), line_no, static_cast<int>(line.size()),
                                   line.data());
            continue;
        }
        presets.back().passes.push_back(std::move(pass));
    }

    std::erase_if(presets, [&](const FilterPreset& p) {
        if (!p.passes.empty() && !p.name.empty()) return false;
        utils::Logger::warning(utils::LogChannel::Video, "%s: preset '%s' has no passes, skipped", path.c_str(), p.name.c_str());
        return true;
    });
    if (presets.empty()) presets.push_back(default_preset());
    return presets;
}

auto find_preset(const std::vector<FilterPreset>& presets, const std::string& name) -> const FilterPreset& {
    for (const auto& preset : presets) {
        if (preset.name == name) return preset;
    }
    return presets.front();
}

} // namespace snes::video
