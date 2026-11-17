// SPDX-License-Identifier: MIT
#pragma once
#include "video/filter_preset.hpp"

#include <SDL3/SDL.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace bitforge::video {

// Where the picture goes in the window, in pixels. The rest of the window is black.
struct Viewport {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

enum class BackendKind { Gpu, Renderer };

// "gpu" / "sdl": the names used in the ini file and on the command line.
auto backend_kind_name(BackendKind kind) -> const char*;
auto parse_backend_kind(std::string_view name) -> std::optional<BackendKind>;

// Draws emulator frames into one window, PCSX2-style: the game window owns exactly one backend
// and can swap it at runtime (View > Renderer). Frames are XRGB8888.
class VideoBackend {
public:
    VideoBackend() = default;
    VideoBackend(const VideoBackend&) = delete;
    auto operator=(const VideoBackend&) -> VideoBackend& = delete;
    virtual ~VideoBackend() = default;

    // Takes over `window` for drawing. False (logged) if the backend can't run here.
    virtual auto create(SDL_Window* window) -> bool = 0;
    // Releases everything create() made; the window stays. Safe to call twice.
    virtual auto destroy() -> void = 0;

    // Copies the frame for the next present(). `pitch` is in bytes. Null clears the picture.
    virtual auto upload_frame(const uint32_t* pixels, int width, int height, int pitch) -> void = 0;
    // Draws the last frame into `viewport` (letterboxed in black) and shows it.
    virtual auto present(const Viewport& viewport) -> void = 0;
    // Applies a filter preset. False if it could not (the backend then shows plain nearest).
    virtual auto set_filter(const FilterPreset& preset) -> bool = 0;

    virtual auto kind() const -> BackendKind = 0;
    // For the status bar, e.g. "SDL_GPU (vulkan)".
    virtual auto description() const -> std::string = 0;

    // The next present() also writes what it shows (the whole window) to `path` as a BMP.
    auto request_screenshot(std::string path) -> void { screenshot_path_ = std::move(path); }

protected:
    auto take_screenshot_request() -> std::optional<std::string> { return std::exchange(screenshot_path_, std::nullopt); }

private:
    std::optional<std::string> screenshot_path_;
};

auto make_backend(BackendKind kind) -> std::unique_ptr<VideoBackend>;

// Writes XRGB8888 pixels (tightly packed) to a BMP. Logs the result.
auto save_bmp(const std::string& path, const uint32_t* pixels, int width, int height) -> bool;

} // namespace bitforge::video
