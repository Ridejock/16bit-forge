# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)
project(snes_emulator VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CPU/PPU/APU/DMA trace points are switched at runtime from the Log panel (Emulation tab).
# Uncomment to compile them out entirely.
#add_compile_definitions(SNES_NO_TRACE)

# ThreadSanitizer build (GCC/Clang) for the threading tests: cmake -DSNES_TSAN=ON. Covers SDL too.
option(SNES_TSAN "Build everything with -fsanitize=thread" OFF)
if(SNES_TSAN AND NOT MSVC)
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
add_library(snes_core STATIC
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
    src/utils/logging.cpp
)
target_include_directories(snes_core PUBLIC src)

add_executable(snes_emulator
    src/main.cpp
    src/frontend/app.cpp
    src/frontend/audio.cpp
    src/frontend/emu_thread.cpp
    src/frontend/emulator.cpp
    src/frontend/rewind.cpp
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
)

target_include_directories(snes_emulator PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(snes_emulator PRIVATE snes_core SDL3::SDL3-static imgui Threads::Threads)

# Filter shaders: every shaders/*.hlsl is compiled by SDL_shadercross (cmake/Shaders.cmake, from
# Emerald) to SPIR-V, DXIL and MSL in a shaders/ folder next to the executable, and presets.ini
# is copied beside them. A new .hlsl is picked up on the next build (CONFIGURE_DEPENDS).
include(cmake/Shaders.cmake)
file(GLOB SNES_SHADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/*.hlsl)
emerald_add_shaders(snes_emulator ${SNES_SHADERS})
add_custom_command(TARGET snes_emulator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/shaders/presets.ini
            $<TARGET_FILE_DIR:snes_emulator>/shaders/presets.ini
    COMMENT "Copying shaders/presets.ini next to snes_emulator"
)

# The ROM library scans a roms/ folder next to the executable; put your own ROMs there.

# Optional folder of extra free test ROMs (.sfc/.spc) that apu_tests and state_tests also run.
# The suites pass without it (they build their own test ROM). Can be overridden at run time
# with the SNES_TEST_ROMS_DIR environment variable.
set(SNES_TEST_ROMS_DIR "" CACHE PATH "Folder of extra test ROMs for the test suites (optional)")

# Tests: CPU/bus suite, the PPU, timing/IRQ and DMA/HDMA suite, and the APU suite (SPC700, timers,
# DSP). apu_tests also runs SNES_TEST_ROMS_DIR/*.spc and any .spc paths on its command line.
add_executable(cpu_tests tests/cpu_tests.cpp)
target_link_libraries(cpu_tests PRIVATE snes_core)
add_executable(ppu_tests tests/ppu_tests.cpp)
target_link_libraries(ppu_tests PRIVATE snes_core)
add_executable(apu_tests tests/apu_tests.cpp)
target_link_libraries(apu_tests PRIVATE snes_core)
target_compile_definitions(apu_tests PRIVATE SNES_TEST_ROMS_DIR="${SNES_TEST_ROMS_DIR}")

# Save states (archive, header checks, save/run/load/run determinism over the built-in test ROMs,
# SNES_TEST_ROMS_DIR/*.sfc and any ROMs given on the command line), battery RAM and the $4016/$4017/$4218-$421F controller ports.
add_executable(state_tests tests/state_tests.cpp src/frontend/emulator.cpp src/frontend/rewind.cpp)
target_include_directories(state_tests PRIVATE src)
target_link_libraries(state_tests PRIVATE snes_core)
target_compile_definitions(state_tests PRIVATE SNES_TEST_ROMS_DIR="${SNES_TEST_ROMS_DIR}")

# Enhancement chips: uPD77C25 instructions, Super FX ops, SA-1 arithmetic/DMA/bitmap conversion,
# cartridge detection, and save/run/load/run determinism with coprocessor state. Pass free test
# ROMs (Super FX / SA-1 demos and test carts) on the command line to include them.
add_executable(chip_tests tests/chip_tests.cpp)
target_link_libraries(chip_tests PRIVATE snes_core)

# Audio output tests: the frontend's FIFO, rate control and fades against a simulated sound
# card, through a real SDL_AudioStream (no device needed).
add_executable(audio_sync_tests tests/audio_sync_tests.cpp src/frontend/audio.cpp src/frontend/wav_writer.cpp)
target_include_directories(audio_sync_tests PRIVATE src)
target_link_libraries(audio_sync_tests PRIVATE snes_core SDL3::SDL3-static)

# Controller input: the mapping layer, InputManager on SDL virtual gamepads (hotplug, ports,
# press-to-bind) and the [padN]/[log] ini round trip. Headless.
add_executable(input_tests tests/input_tests.cpp src/frontend/input.cpp src/frontend/settings.cpp
    src/video/video_backend.cpp src/video/gpu_backend.cpp src/video/gpu_shader.cpp src/video/renderer_backend.cpp
    src/video/filter_preset.cpp)
target_include_directories(input_tests PRIVATE src)
target_link_libraries(input_tests PRIVATE snes_core SDL3::SDL3-static)

# Frame pacing (header-only logic, virtual time).
add_executable(pacer_tests tests/pacer_tests.cpp)
target_include_directories(pacer_tests PRIVATE src)

# The emulation thread: the frame mailbox, the command queue, and a real-time harness (the real
# EmuThread, core and audio output against a simulated sound card thread, with the UI thread
# stalling). Build with -DSNES_TSAN=ON to run it under ThreadSanitizer.
add_executable(thread_tests tests/thread_tests.cpp src/frontend/emu_thread.cpp src/frontend/emulator.cpp src/frontend/rewind.cpp
    src/frontend/audio.cpp src/frontend/wav_writer.cpp src/frontend/input.cpp)
target_include_directories(thread_tests PRIVATE src)
target_link_libraries(thread_tests PRIVATE snes_core SDL3::SDL3-static Threads::Threads)

# Headless audio render to WAV (no SDL): audio_dump <rom.sfc|song.spc> <out.wav> [seconds]
add_executable(audio_dump tools/audio_dump.cpp)
target_link_libraries(audio_dump PRIVATE snes_core)

foreach(target snes_emulator snes_core cpu_tests ppu_tests apu_tests state_tests chip_tests input_tests audio_sync_tests pacer_tests thread_tests audio_dump)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    endif()
endforeach()

# ---- tests: ctest runs every suite headless (no window, no audio device, no ROM files) ----
enable_testing()
foreach(suite cpu_tests ppu_tests apu_tests state_tests chip_tests audio_sync_tests input_tests pacer_tests thread_tests)
    add_test(NAME ${suite} COMMAND ${suite})
    set_tests_properties(${suite} PROPERTIES TIMEOUT 900 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endforeach()

# ---- release package: cmake --install, or cpack (ZIP) ----
# A portable folder: the executable, compiled shaders, default settings and the docs. SDL3 and
# Dear ImGui are linked statically, so there are no DLLs to ship. Users add roms/ and firmware/.
install(TARGETS snes_emulator RUNTIME DESTINATION .)
install(DIRECTORY $<TARGET_FILE_DIR:snes_emulator>/shaders/ DESTINATION shaders)
install(FILES config/snes_emulator.ini README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
install(FILES dist/roms/README.txt DESTINATION roms)
install(FILES dist/firmware/README.txt DESTINATION firmware)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME snes_emulator)
set(CPACK_PACKAGE_VENDOR "Ervin Ashley")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "SNES emulator")
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
