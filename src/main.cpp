// SPDX-License-Identifier: MIT
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdlib>
#include <string_view>

#include "frontend/app.hpp"

// Usage: snes_emulator [rom_file] [--exit-after-frames N] [--renderer gpu|sdl] [--filter NAME]
//                      [--screenshot FILE.bmp]   (saved when --exit-after-frames is reached)
//                      [--record-audio FILE.wav] (records FILE_core.wav and FILE_device.wav)
int main(int argc, char* argv[]) {
    snes::frontend::LaunchOptions options;
    for (int i = 1; i < argc; i++) {
        std::string_view arg = argv[i];
        if (arg == "--exit-after-frames" && i + 1 < argc) {
            options.exit_after_frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--renderer" && i + 1 < argc) {
            options.renderer = snes::video::parse_backend_kind(argv[++i]);
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

    snes::frontend::App app;
    if (!app.init()) return 1;
    return app.run(options);
}
