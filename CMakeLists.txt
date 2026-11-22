# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)
# PROJECT_NAME is the executable, package and settings-file name (16bitForge.exe, 16bitForge.ini).
project(16bitForge VERSION 0.1.2
    DESCRIPTION "Forged for the 16-bit era."
    HOMEPAGE_URL "https://github.com/Ridejock/16bit-forge"
    LANGUAGES C CXX)
set(BITFORGE_TITLE "16bit Forge")  # display name: window title and log banner

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CPU/PPU/APU/DMA trace points are switched at runtime from the Log panel (Emulation tab).
# Uncomment to compile them out entirely.
#add_compile_definitions(BITFORGE_NO_TRACE)

# ThreadSanitizer build (GCC/Clang) for the threading tests: cmake -DBITFORGE_TSAN=ON. Covers SDL too.
option(BITFORGE_TSAN "Build everything with -fsanitize=thread" OFF)
if(BITFORGE_TSAN AND NOT MSVC)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

include(FetchContent)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.20  # pinned for reproducible builds; bump deliberately
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)

# Dear ImGui has no CMake build of its own. Pull the sources and compile the SDL3 renderer backend.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.93.0
    GIT_SHALLOW TRUE
)
# v1.93.0 has no CMakeLists.txt. MakeAvailable still downloads it and skips add_subdirectory.
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)
if(MSVC)
    target_compile_options(imgui PRIVATE /W3)
else()
    target_compile_options(imgui PRIVATE -w)
endif()

# The emulator core (no SDL, no ImGui): linked by the app and both test suites.
add_library(bitforge_core STATIC
    src/snes/system.cpp
    src/snes/cpu/cpu65c816.cpp
    src/snes/memory/bus.cpp
    src/snes/memory/dma.cpp
    src/snes/memory/wram.cpp
    src/snes/memory/cartridge.cpp
    src/snes/coprocessor/board.cpp
    src/snes/coprocessor/upd77c25.cpp
    src/snes/coprocessor/necdsp.cpp
    src/snes/coprocessor/gsu.cpp
    src/snes/coprocessor/sa1.cpp
    src/snes/coprocessor/obc1.cpp
    src/snes/coprocessor/cx4.cpp
    src/snes/coprocessor/sdd1.cpp
    src/snes/coprocessor/spc7110.cpp
    src/snes/coprocessor/rtc4513.cpp
    src/snes/ppu/ppu.cpp
    src/snes/ppu/renderer.cpp
    src/snes/apu/apu.cpp
    src/snes/apu/spc700.cpp
    src/snes/apu/dsp.cpp
    src/snes/savestate.cpp
    src/snes/msu1.cpp
    src/snes/cheats.cpp
    src/snes/controllers.cpp
    src/debug/memory_value.cpp
    src/debug/ram_search.cpp
    src/debug/memory_watch.cpp
    src/debug/timing_check.cpp
    src/speedrun/splits.cpp
    src/speedrun/run_timer.cpp
    src/speedrun/xml.cpp
    src/speedrun/lss.cpp
    src/speedrun/autosplit.cpp
    src/speedrun/splits_file.cpp
    src/speedrun/movie.cpp
    src/speedrun/movie_journal.cpp
    src/speedrun/race.cpp
    src/utils/logging.cpp
    src/utils/sha1.cpp
)
target_include_directories(bitforge_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(bitforge_core PUBLIC Threads::Threads)  # the movie journal's writer

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/frontend/app.cpp
    src/frontend/app_movie.cpp
    src/frontend/audio.cpp
    src/frontend/emu_thread.cpp
    src/frontend/emulator.cpp
    src/frontend/rewind.cpp
    src/frontend/memory_panel.cpp
    src/frontend/timer_panel.cpp
    src/frontend/timing_report.cpp
    src/frontend/game_window.cpp
    src/frontend/input.cpp
    src/frontend/settings.cpp
    src/frontend/wav_writer.cpp
    src/video/video_backend.cpp
    src/video/filter_preset.cpp
    src/video/renderer_backend.cpp
    src/video/gpu_backend.cpp
    src/video/gpu_shader.cpp
    src/utils/log_view.cpp
    src/frontend/app_icon.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE src)
target_link_libraries(${PROJECT_NAME} PRIVATE bitforge_core SDL3::SDL3-static imgui Threads::Threads)
# App icon (assets/icons, made by tools/make_icons.py). Windows: the .ico goes into the exe as a
# resource, with the name and version in its file properties. Elsewhere the 64 px PNG is
# embedded as bytes and set as the window icon at run time (src/frontend/app_icon.cpp).
set(BITFORGE_ICON_PNG ${CMAKE_SOURCE_DIR}/assets/icons/16bitForge_64.png)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${BITFORGE_ICON_PNG})
file(READ ${BITFORGE_ICON_PNG} icon_hex HEX)
string(REGEX REPLACE "(................................................................)" "\\1\n" icon_hex "${icon_hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," icon_bytes "${icon_hex}")
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/app_icon_png.inc CONTENT "${icon_bytes}\n" @ONLY)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_BINARY_DIR}/generated)
if(MSVC)
    set(BITFORGE_ICO ${CMAKE_SOURCE_DIR}/assets/icons/16bitForge.ico)
    configure_file(assets/icons/16bitForge.rc.in ${CMAKE_BINARY_DIR}/generated/16bitForge.rc @ONLY)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_BINARY_DIR}/generated/16bitForge.rc)
endif()
# The app's name, version and settings file come from project() above (src/frontend/app.cpp).
target_compile_definitions(${PROJECT_NAME} PRIVATE
    BITFORGE_TITLE="${BITFORGE_TITLE}"
    BITFORGE_TAGLINE="${PROJECT_DESCRIPTION}"
    BITFORGE_VERSION="${PROJECT_VERSION}"
    BITFORGE_EXE_NAME="${PROJECT_NAME}")

# Filter shaders: every shaders/*.hlsl is compiled by SDL_shadercross (cmake/Shaders.cmake, from
# Emerald) to SPIR-V, DXIL and MSL in a shaders/ folder next to the executable, and presets.ini
# is copied beside them. A new .hlsl is picked up on the next build (CONFIGURE_DEPENDS).
include(cmake/Shaders.cmake)
file(GLOB BITFORGE_SHADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/*.hlsl)
emerald_add_shaders(${PROJECT_NAME} ${BITFORGE_SHADERS})
# A shader's tunables are its "// @param <name> <default>" lines; they go to shaders/<name>.params
# (name and default, in uniform slot order) for presets.ini's "param =" lines. Editing a shader
# re-runs CMake so the list stays current.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${BITFORGE_SHADERS})
foreach(shader ${BITFORGE_SHADERS})
    file(STRINGS ${shader} param_lines REGEX "^// @param ")
    if(param_lines)
        list(TRANSFORM param_lines REPLACE "^// @param +([A-Za-z0-9_]+) +([-+.0-9eE]+).*$" "\\1 \\2")
        list(JOIN param_lines "\n" params)
        get_filename_component(name ${shader} NAME_WE)
        file(GENERATE OUTPUT $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders/${name}.params CONTENT "${params}\n")
    endif()
endforeach()
# presets.ini is copied the same way, so editing it alone is enough to update the build.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/presets.ini)
file(GENERATE OUTPUT $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders/presets.ini INPUT ${CMAKE_SOURCE_DIR}/shaders/presets.ini)

# The ROM library scans a roms/ folder next to the executable; put your own ROMs there.

# Optional folder of extra free test ROMs (.sfc/.spc) that apu_tests and state_tests also run.
# The suites pass without it (they build their own test ROM). Can be overridden at run time
# with the BITFORGE_TEST_ROMS_DIR environment variable.
set(BITFORGE_TEST_ROMS_DIR "" CACHE PATH "Folder of extra test ROMs for the test suites (optional)")

# Tests: CPU/bus suite, the PPU, timing/IRQ and DMA/HDMA suite, and the APU suite (SPC700, timers,
# DSP). apu_tests also runs BITFORGE_TEST_ROMS_DIR/*.spc and any .spc paths on its command line.
add_executable(cpu_tests tests/cpu_tests.cpp)
target_link_libraries(cpu_tests PRIVATE bitforge_core)
add_executable(ppu_tests tests/ppu_tests.cpp)
target_link_libraries(ppu_tests PRIVATE bitforge_core)
add_executable(apu_tests tests/apu_tests.cpp)
target_link_libraries(apu_tests PRIVATE bitforge_core)
target_compile_definitions(apu_tests PRIVATE BITFORGE_TEST_ROMS_DIR="${BITFORGE_TEST_ROMS_DIR}")

# Save states (archive, header checks, save/run/load/run determinism over the built-in test ROMs,
# BITFORGE_TEST_ROMS_DIR/*.sfc and any ROMs given on the command line), battery RAM and the $4016/$4017/$4218-$421F controller ports.
add_executable(state_tests tests/state_tests.cpp src/frontend/emulator.cpp src/frontend/rewind.cpp)
target_include_directories(state_tests PRIVATE src)
target_link_libraries(state_tests PRIVATE bitforge_core)
target_compile_definitions(state_tests PRIVATE BITFORGE_TEST_ROMS_DIR="${BITFORGE_TEST_ROMS_DIR}")

# Enhancement chips: uPD77C25 instructions, Super FX ops, SA-1 arithmetic/DMA/bitmap conversion,
# cartridge detection, and save/run/load/run determinism with coprocessor state. Pass free test
# ROMs (Super FX / SA-1 demos and test carts) on the command line to include them.
add_executable(chip_tests tests/chip_tests.cpp)
target_link_libraries(chip_tests PRIVATE bitforge_core)

# Audio output tests: the frontend's FIFO, rate control and fades against a simulated sound
# card, through a real SDL_AudioStream (no device needed).
add_executable(audio_sync_tests tests/audio_sync_tests.cpp src/frontend/audio.cpp src/frontend/wav_writer.cpp)
target_include_directories(audio_sync_tests PRIVATE src)
target_link_libraries(audio_sync_tests PRIVATE bitforge_core SDL3::SDL3-static)

# Controller input: the mapping layer, InputManager on SDL virtual gamepads (hotplug, ports,
# press-to-bind) and the [padN]/[log] ini round trip. Headless.
add_executable(input_tests tests/input_tests.cpp src/frontend/input.cpp src/frontend/settings.cpp
    src/video/video_backend.cpp src/video/gpu_backend.cpp src/video/gpu_shader.cpp src/video/renderer_backend.cpp
    src/video/filter_preset.cpp)
target_include_directories(input_tests PRIVATE src)
target_link_libraries(input_tests PRIVATE bitforge_core SDL3::SDL3-static)

# Filter presets: the presets.ini parser and the shipped presets against their shaders. Headless.
add_executable(filter_tests tests/filter_tests.cpp src/video/filter_preset.cpp)
target_include_directories(filter_tests PRIVATE src)
target_link_libraries(filter_tests PRIVATE bitforge_core)
target_compile_definitions(filter_tests PRIVATE BITFORGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# RAM search and memory watches (src/debug): the search filters and undo, value formats,
# freezing, the watches file and cheat conversion. Headless, no ROM.
add_executable(memory_tests tests/memory_tests.cpp)
target_include_directories(memory_tests PRIVATE src)
target_link_libraries(memory_tests PRIVATE bitforge_core)

# Run timer and splits (src/speedrun): timing in both methods through pause, fast-forward and
# rewind, split/undo/skip/reset, golds and PB, the .lss reader/writer and the auto-split sidecar;
# then auto-splits against the real core on a built-in test ROM (and on any ROM given with
# --rom <path> --watch <hex offset>, e.g. a homebrew game's HP). Headless.
add_executable(timer_tests tests/timer_tests.cpp src/frontend/emulator.cpp src/frontend/rewind.cpp)
target_include_directories(timer_tests PRIVATE src)
target_link_libraries(timer_tests PRIVATE bitforge_core)

# Race mode (src/speedrun/race.*): the guards, race verification in the run timer, the .race
# sidecar beside the .lss, the verification summary, SHA-1, and the core refusing cheats,
# freezes, edits, states and snapshots on a built-in test ROM. Headless.
add_executable(race_tests tests/race_tests.cpp src/frontend/emulator.cpp src/frontend/rewind.cpp)
target_include_directories(race_tests PRIVATE src)
target_link_libraries(race_tests PRIVATE bitforge_core)

# Movies (src/speedrun/movie.*): the .bfm format, recording and playback in the core on a
# built-in test ROM (replays match frame for frame), save states and rewind cutting a recording
# (rerecords), read-only playback, the battery save left alone, the recording journal (crash
# recovery) and save-state references. Headless.
add_executable(movie_tests tests/movie_tests.cpp src/frontend/emulator.cpp src/frontend/rewind.cpp)
target_include_directories(movie_tests PRIVATE src)
target_link_libraries(movie_tests PRIVATE bitforge_core)

# Timing (src/debug/timing_check.*, src/snes/timing.hpp): the core against documented NTSC hardware
# timing (line and frame lengths, the short line, the frame rate, NMI, IRQ, auto-joypad, refresh,
# DMA/HDMA), game time against the exact frame period, the lag frame counter and the timing report.
# Headless, on built-in programs. Optional: timing_tests --rom <file.sfc> [--frames <n>] adds a
# scripted run of a real game, replayed through rewind, a save state and a movie.
add_executable(timing_tests tests/timing_tests.cpp src/frontend/timing_report.cpp src/frontend/emulator.cpp src/frontend/rewind.cpp)
target_include_directories(timing_tests PRIVATE src)
target_link_libraries(timing_tests PRIVATE bitforge_core SDL3::SDL3-static)

# Frame pacing (header-only logic, virtual time).
add_executable(pacer_tests tests/pacer_tests.cpp)
target_include_directories(pacer_tests PRIVATE src)

# Game window placement beside the main window, kept on screen (header-only logic, no display).
add_executable(window_tests tests/window_tests.cpp)
target_include_directories(window_tests PRIVATE src)

# The emulation thread: the frame mailbox, the command queue, and a real-time harness (the real
# EmuThread, core and audio output against a simulated sound card thread, with the UI thread
# stalling). Build with -DBITFORGE_TSAN=ON to run it under ThreadSanitizer.
add_executable(thread_tests tests/thread_tests.cpp src/frontend/emu_thread.cpp src/frontend/emulator.cpp src/frontend/rewind.cpp
    src/frontend/audio.cpp src/frontend/wav_writer.cpp src/frontend/input.cpp)
target_include_directories(thread_tests PRIVATE src)
target_link_libraries(thread_tests PRIVATE bitforge_core SDL3::SDL3-static Threads::Threads)

# Headless audio render to WAV (no SDL): audio_dump <rom.sfc|song.spc> <out.wav> [seconds]
add_executable(audio_dump tools/audio_dump.cpp)
target_link_libraries(audio_dump PRIVATE bitforge_core)

foreach(target ${PROJECT_NAME} bitforge_core cpu_tests ppu_tests apu_tests state_tests chip_tests input_tests filter_tests audio_sync_tests memory_tests timer_tests race_tests movie_tests timing_tests pacer_tests window_tests thread_tests audio_dump)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)  # fopen/getenv are fine here
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    endif()
endforeach()

# ---- tests: ctest runs every suite headless (no window, no audio device, no ROM files) ----
enable_testing()
foreach(suite cpu_tests ppu_tests apu_tests state_tests chip_tests audio_sync_tests input_tests filter_tests memory_tests timer_tests race_tests movie_tests timing_tests pacer_tests window_tests thread_tests)
    add_test(NAME ${suite} COMMAND ${suite})
    set_tests_properties(${suite} PROPERTIES TIMEOUT 900 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endforeach()

# ---- release package: cmake --install, or cpack (ZIP) ----
# A portable folder: the executable, compiled shaders, default settings and the docs. SDL3 and
# Dear ImGui are linked statically, so there are no DLLs to ship. Users add roms/ and firmware/.
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION .)
install(DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders/ DESTINATION shaders)
install(FILES config/${PROJECT_NAME}.ini README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
install(FILES dist/roms/README.txt DESTINATION roms)
install(FILES dist/firmware/README.txt DESTINATION firmware)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_VENDOR "Ervin Ashley")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${BITFORGE_TITLE}: ${PROJECT_DESCRIPTION}")
if(WIN32)
    set(CPACK_SYSTEM_NAME windows-x64)
elseif(APPLE)
    set(CPACK_SYSTEM_NAME macos)
else()
    set(CPACK_SYSTEM_NAME linux-${CMAKE_SYSTEM_PROCESSOR})
endif()
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CPACK_SYSTEM_NAME}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
include(CPack)
