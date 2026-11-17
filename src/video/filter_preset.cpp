// SPDX-License-Identifier: MIT
#include "video/filter_preset.hpp"
#include "utils/logging.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace bitforge::video {

namespace {

constexpr std::string_view kPassKey = "pass";
constexpr std::string_view kParamKey = "param";
constexpr size_t kPassFields = 4;  // shader, scale type, scale, filter; then options

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

// The whole field must be a number.
auto parse_float(std::string_view s, float& out) -> bool {
    const std::string text(trim(s));
    char* end = nullptr;
    out = std::strtof(text.c_str(), &end);
    return !text.empty() && end == text.c_str() + text.size();
}

// "2" (both axes) or "4x1" (x, y).
auto parse_scale(std::string_view s, FilterPass& pass) -> bool {
    const auto x = s.find('x');
    if (x == std::string_view::npos) {
        if (!parse_float(s, pass.scale_x)) return false;
        pass.scale_y = pass.scale_x;
    } else if (!parse_float(s.substr(0, x), pass.scale_x) || !parse_float(s.substr(x + 1), pass.scale_y)) {
        return false;
    }
    return pass.scale_x > 0.0f && pass.scale_y > 0.0f;
}

// "<keyword> <word>", e.g. "name metric"; the word after the keyword, or "" if it doesn't match.
auto keyword_arg(std::string_view option, std::string_view keyword) -> std::string_view {
    if (option.size() <= keyword.size() || option.substr(0, keyword.size()) != keyword) return {};
    const std::string_view rest = option.substr(keyword.size());
    if (rest.front() != ' ' && rest.front() != '\t') return {};
    const std::string_view arg = trim(rest);
    return arg.find_first_of(" \t") == std::string_view::npos ? arg : std::string_view{};
}

// `earlier` are the passes before this one in the preset, which named inputs must refer to.
auto parse_pass(std::string_view value, const std::vector<FilterPass>& earlier, FilterPass& pass) -> bool {
    const auto fields = split_fields(value);
    if (fields.size() < kPassFields || fields[0].empty()) return false;

    pass.shader = std::string(fields[0]);

    if (fields[1] == "source") {
        pass.scale_type = ScaleType::Source;
    } else if (fields[1] == "viewport") {
        pass.scale_type = ScaleType::Viewport;
    } else {
        return false;
    }

    if (!parse_scale(fields[2], pass)) return false;

    if (fields[3] == "nearest") {
        pass.filter = FilterMode::Nearest;
    } else if (fields[3] == "linear") {
        pass.filter = FilterMode::Linear;
    } else {
        return false;
    }

    for (size_t i = kPassFields; i < fields.size(); i++) {
        const std::string_view option = fields[i];
        if (option == "float") {
            pass.float_target = true;
        } else if (const auto name = keyword_arg(option, "name"); !name.empty() && name != kOriginalInput) {
            pass.name = std::string(name);
        } else if (const auto input = keyword_arg(option, "input"); !input.empty()) {
            const bool known = input == kOriginalInput ||
                               std::any_of(earlier.begin(), earlier.end(), [&](const FilterPass& p) { return p.name == input; });
            if (!known) return false;
            pass.inputs.emplace_back(input);
        } else {
            return false;
        }
    }
    return true;
}

auto parse_param(std::string_view value, FilterParam& param) -> bool {
    const auto fields = split_fields(value);
    if (fields.size() != 2 || fields[0].empty() || !parse_float(fields[1], param.value)) return false;
    param.name = std::string(fields[0]);
    return true;
}

} // namespace

auto default_preset() -> FilterPreset {
    FilterPass pass;
    pass.shader = "stock";
    return {"nearest", {pass}, {}};
}

auto load_presets(const std::string& path) -> std::vector<FilterPreset> {
    std::ifstream in(path, std::ios::binary);
    if (!in) utils::Logger::warning(utils::LogChannel::Video, "No filter presets at %s; only 'nearest' is available", path.c_str());
    std::ostringstream text;
    text << in.rdbuf();
    return parse_presets(text.str(), path);
}

auto parse_presets(std::string_view text, const std::string& origin) -> std::vector<FilterPreset> {
    std::vector<FilterPreset> presets;
    int line_no = 0;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const std::string_view line = trim(text.substr(0, newline));
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        line_no++;
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            presets.push_back({std::string(trim(line.substr(1, line.size() - 2))), {}, {}});
            continue;
        }
        const auto eq = line.find('=');
        const std::string_view key = eq == std::string_view::npos ? std::string_view{} : trim(line.substr(0, eq));
        bool ok = false;
        if (!presets.empty() && key == kPassKey) {
            FilterPass pass;
            ok = parse_pass(line.substr(eq + 1), presets.back().passes, pass);
            if (ok) presets.back().passes.push_back(std::move(pass));
        } else if (!presets.empty() && key == kParamKey) {
            FilterParam param;
            ok = parse_param(line.substr(eq + 1), param);
            if (ok) presets.back().params.push_back(std::move(param));
        }
        if (!ok) {
            utils::Logger::warning(utils::LogChannel::Video, "%s:%d: ignored '%.*s'", origin.c_str(), line_no,
                                   static_cast<int>(line.size()), line.data());
        }
    }

    std::erase_if(presets, [&](const FilterPreset& p) {
        if (!p.passes.empty() && !p.name.empty()) return false;
        utils::Logger::warning(utils::LogChannel::Video, "%s: preset '%s' has no passes, skipped", origin.c_str(), p.name.c_str());
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

auto parse_shader_params(std::string_view text) -> std::vector<FilterParam> {
    std::vector<FilterParam> params;
    while (!text.empty() && params.size() < kMaxShaderParams) {
        const auto newline = text.find('\n');
        const std::string_view line = trim(text.substr(0, newline));
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        const auto space = line.find_first_of(" \t");
        FilterParam param;
        if (space == std::string_view::npos || !parse_float(line.substr(space), param.value)) continue;
        param.name = std::string(line.substr(0, space));
        params.push_back(std::move(param));
    }
    return params;
}

} // namespace bitforge::video
