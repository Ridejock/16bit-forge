// SPDX-License-Identifier: MIT
// Filter presets: the presets.ini parser (pass lines, options, params, bad lines, the old
// four-field format) and the shipped shaders/presets.ini against the shaders it names: every
// shader exists, declares as many textures as its pass gets, and every preset parameter is
// declared by one of the preset's shaders. Headless: no GPU needed.
#include "utils/logging.hpp"
#include "video/filter_preset.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace snes::video;

namespace {

int checks_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("\n    check failed: %s (line %d)", #cond, __LINE__);    \
            checks_failed++;                                                     \
        }                                                                        \
    } while (0)

auto read_file(const std::string& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

auto count(const std::string& text, const std::string& what) -> size_t {
    size_t n = 0;
    for (size_t pos = text.find(what); pos != std::string::npos; pos = text.find(what, pos + 1)) n++;
    return n;
}

auto test_parser() -> void {
    const auto presets = parse_presets(
        "; comment\n"
        "[old]\r\n"
        "pass = xbr_lv2, source, 2, nearest\r\n"
        "pass = sharp_bilinear, viewport, 1, linear\n"
        "[new]\n"
        "pass = a, source, 4x1, nearest, float, name sig\n"
        "pass = b, source, 1.5, linear, input original, input sig\n"
        "param = gain, -0.25\n"
        "pass = c, viewport, 1, nearest\n"
        "[bad]\n"
        "pass = a, source, 1, nearest, input later\n"   // not an earlier pass
        "pass = a, source, 0, nearest\n"                // scale must be > 0
        "pass = a, source, 2x, nearest\n"
        "pass = a, window, 1, nearest\n"
        "pass = a, source, 1, cubic\n"
        "pass = a, source, 1, nearest, sparkle\n"
        "pass = a, source, 1, nearest, name original\n"
        "param = gain\n"
        "param = gain, loud\n",
        "test");
    CHECK(presets.size() == 2);  // [bad] has no valid pass
    if (presets.size() != 2) return;

    const FilterPreset& old = presets[0];
    CHECK(old.name == "old" && old.passes.size() == 2 && old.params.empty());
    CHECK(old.passes[0].shader == "xbr_lv2" && old.passes[0].scale_type == ScaleType::Source);
    CHECK(old.passes[0].scale_x == 2.0f && old.passes[0].scale_y == 2.0f);
    CHECK(!old.passes[0].float_target && old.passes[0].name.empty() && old.passes[0].inputs.empty());
    CHECK(old.passes[1].filter == FilterMode::Linear && old.passes[1].scale_type == ScaleType::Viewport);

    const FilterPreset& now = presets[1];
    CHECK(now.passes.size() == 3);
    if (now.passes.size() != 3) return;
    CHECK(now.passes[0].scale_x == 4.0f && now.passes[0].scale_y == 1.0f);
    CHECK(now.passes[0].float_target && now.passes[0].name == "sig");
    CHECK(now.passes[1].scale_x == 1.5f && now.passes[1].filter == FilterMode::Linear);
    CHECK((now.passes[1].inputs == std::vector<std::string>{"original", "sig"}));
    CHECK(now.params.size() == 1 && now.params[0].name == "gain" && now.params[0].value == -0.25f);

    CHECK(parse_presets("", "empty").front().name == default_preset().name);
    CHECK(find_preset(presets, "new").name == "new" && find_preset(presets, "gone").name == "old");
}

auto test_shader_params() -> void {
    const auto params = parse_shader_params("curvature 0.04\r\nmask -1\n\nbroken\nalso broken x\n");
    CHECK(params.size() == 2);
    if (params.size() != 2) return;
    CHECK(params[0].name == "curvature" && params[0].value == 0.04f);
    CHECK(params[1].name == "mask" && params[1].value == -1.0f);
}

// The "// @param <name> <default>" lines of a shader source, like CMakeLists.txt extracts them.
auto declared_params(const std::string& source) -> std::vector<std::string> {
    std::vector<std::string> names;
    std::istringstream lines(source);
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind("// @param ", 0) != 0) continue;
        std::istringstream words(line.substr(10));
        std::string name;
        words >> name;
        names.push_back(name);
    }
    return names;
}

auto test_shipped_presets() -> void {
    const std::string dir = std::string(SNES_SOURCE_DIR) + "/shaders/";
    const std::string ini = read_file(dir + "presets.ini");
    CHECK(!ini.empty());
    const auto presets = parse_presets(ini, "presets.ini");
    CHECK(count(ini, "\n[") == presets.size());  // every preset parsed
    for (const FilterPreset& preset : presets) {
        std::vector<std::string> declared;
        for (const FilterPass& pass : preset.passes) {
            const std::string source = read_file(dir + pass.shader + ".frag.hlsl");
            if (source.empty()) std::printf("\n    %s: no shader %s", preset.name.c_str(), pass.shader.c_str());
            CHECK(!source.empty());
            // Each texture input is one "Texture2D" declaration (Source comes from filter_common).
            const size_t textures = 1 + count(source, "Texture2D");
            if (textures != 1 + pass.inputs.size()) std::printf("\n    %s: %s inputs", preset.name.c_str(), pass.shader.c_str());
            CHECK(textures == 1 + pass.inputs.size());
            const auto names = declared_params(source);
            CHECK(names.size() <= kMaxShaderParams);
            declared.insert(declared.end(), names.begin(), names.end());
        }
        for (const FilterParam& param : preset.params) {
            const bool known = std::find(declared.begin(), declared.end(), param.name) != declared.end();
            if (!known) std::printf("\n    %s: unknown param %s", preset.name.c_str(), param.name.c_str());
            CHECK(known);
        }
    }
}

} // namespace

int main() {
    snes::utils::Logger::set_level(snes::utils::LogLevel::Error);
    std::printf("=== Filter preset tests ===\n\n");
    int passed = 0;
    int failed = 0;
    auto run_test = [&](const char* name, auto&& fn) {
        std::printf("  %s... ", name);
        const int before = checks_failed;
        fn();
        if (checks_failed == before) {
            std::printf("PASSED\n");
            passed++;
        } else {
            std::printf("\n  FAILED\n");
            failed++;
        }
    };
    run_test("presets.ini parser: passes, options, params, bad lines", test_parser);
    run_test("Shader parameter lists", test_shader_params);
    run_test("Shipped presets match their shaders", test_shipped_presets);
    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
