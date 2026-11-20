// SPDX-License-Identifier: MIT
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

#include "frontend/app.hpp"
#include "frontend/timing_report.hpp"

// Usage: 16bitForge [rom_file] [--exit-after-frames N] [--renderer gpu|sdl] [--filter NAME]
//                   [--screenshot FILE.bmp]   (saved when --exit-after-frames is reached)
//                   [--record-audio FILE.wav] (records FILE_core.wav and FILE_device.wav)
//        16bitForge --timing-report FILE.txt [rom_file] [--timing-frames N]
//                   writes the timing report (frontend/timing_report.hpp) without opening a window
int main(int argc, char* argv[]) {
    bitforge::frontend::LaunchOptions options;
    std::string timing_report;
    uint64_t timing_frames = 3600;
    for (int i = 1; i < argc; i++) {
        std::string_view arg = argv[i];
        if (arg == "--timing-report" && i + 1 < argc) {
            timing_report = argv[++i];
        } else if (arg == "--timing-frames" && i + 1 < argc) {
            timing_frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--exit-after-frames" && i + 1 < argc) {
            options.exit_after_frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--renderer" && i + 1 < argc) {
            options.renderer = bitforge::video::parse_backend_kind(argv[++i]);
        } else if (arg == "--filter" && i + 1 < argc) {
            options.filter = argv[++i];
        } else if (arg == "--screenshot" && i + 1 < argc) {
            options.screenshot = argv[++i];
        } else if (arg == "--record-audio" && i + 1 < argc) {
            options.record_audio = argv[++i];
        } else {
            options.rom = argv[i];
        }
    }

    if (!timing_report.empty()) {
        const auto report = bitforge::frontend::make_timing_report(
            {std::string(BITFORGE_TITLE) + " " + BITFORGE_VERSION, options.rom, timing_frames, {}});
        std::ofstream(timing_report, std::ios::binary | std::ios::trunc) << report.text;
        std::fputs(report.text.c_str(), stdout);
        return report.ok ? 0 : 1;
    }

    bitforge::frontend::App app;
    if (!app.init()) return 1;
    return app.run(options);
}
