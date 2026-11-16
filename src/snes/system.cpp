// SPDX-License-Identifier: MIT
#include "snes/system.hpp"
#include "snes/ppu/ppu.hpp"
#include "utils/logging.hpp"
#include <cstdio>

namespace snes {

System::System() 
    : cpu(bus) {
    
    bus.connect_cpu(&cpu);
    bus.connect_ppu(&ppu);
    bus.connect_apu(&apu);
    bus.connect_cheats(&cheats_);
    ppu.connect_bus(&bus);
    
    reset();
}

System::~System() = default;

auto System::load_rom(const std::string& path) -> bool {
    if (!bus.load_rom(path)) return false;
    msu1_ = Msu1::detect(path);
    bus.connect_msu1(msu1_.get());
    msu1_samples_.clear();
    
    char title[22];
    for (int i = 0; i < 21; i++) {
        unsigned char ch = bus.read(0x00FFC0 + i);
        title[i] = (ch >= 32 && ch < 127) ? static_cast<char>(ch) : ' ';
    }
    title[21] = '\0';
    utils::Logger::info(utils::LogChannel::Rom, "ROM header title: %.21s", title);
    utils::Logger::info(utils::LogChannel::Rom, "ROM type: 0x%02X  size: 0x%02X  SRAM: 0x%02X",
                        bus.read(0x00FFD6), bus.read(0x00FFD7), bus.read(0x00FFD8));

    auto log_bytes = [this](const char* label, Address base, int count) {
        char line[128];
        int used = 0;
        line[0] = '\0';
        for (int i = 0; i < count; i++) {
            if (used < 0 || static_cast<size_t>(used) >= sizeof(line)) break;
            int wrote = snprintf(line + used, sizeof(line) - static_cast<size_t>(used),
                                 "%02X%s", bus.read(base + i), (i + 1 == count) ? "" : " ");
            if (wrote < 0) break;
            used += wrote;
        }
        utils::Logger::info(utils::LogChannel::Rom, "%s %s", label, line);
    };
    log_bytes("Bytes at $008000:", 0x008000, 16);
    log_bytes("Bytes at $008080:", 0x008080, 32);
    log_bytes("Bytes at $008090:", 0x008090, 32);

    return true;
}

auto System::reset() -> void {
    utils::Logger::info("Reset");

    bus.reset();
    cpu.reset();
    ppu.reset();
    apu.reset();
    if (msu1_) msu1_->reset();
    total_clocks = 0;
}

auto System::run_frame() -> void {
    frame_count++;

    if (!vram_dumped && frame_count > 60) {
        ppu.dump_vram();  // This should already dump $4000 if we changed it
        vram_dumped = true;
    }
    
    apu.clear_samples();
    bus.apply_ram_cheats();  // the frame starts in vblank, where a Pro Action Replay writes
    while (true) {
        // The bus times each instruction in master clocks from the speed of every address it
        // touched (FastROM, MEMSEL), plus any DMA it started; HDMA time lands in the next step.
        bus.begin_cpu_step();
        const int cycles = cpu.step();
        const auto clocks = static_cast<uint32_t>(bus.end_cpu_step(cycles));
        total_clocks += clocks;

        bool frame_ready = ppu.step(clocks);
        apu.step(clocks);  // the SMP catches up to the S-CPU after every instruction
        bus.cart().step(clocks, bus.last_address());  // and so does a cartridge coprocessor
        
        if (frame_ready) {
            if (msu1_) {
                msu1_samples_.resize(apu.samples().size());
                msu1_->render(msu1_samples_, apu::APU::kSampleRate);
            }
            // Every 60 frames (1 second), dump CPU state
            if (frame_count % 60 == 0) {
                cpu.print_state();
            }
            break;
        }
    }
}

auto System::get_framebuffer() const -> const uint32_t* {
    return ppu.get_framebuffer();
}

auto System::get_framebuffer_size() const -> std::pair<int, int> {
    return {ppu.frame_width(), ppu.frame_height()};
}

auto System::set_joypad_state(int port, uint16_t state) -> void {
    bus.set_joypad(port, state);
}

auto System::serialize(Archive& ar) -> void {
    ar(total_clocks, frame_count, vram_dumped);
    bus.serialize(ar);
    cpu.serialize(ar);
    ppu.serialize(ar);
    apu.serialize(ar);
    if (msu1_) msu1_->serialize(ar);  // only with an MSU-1: other games' states are unchanged
}

auto System::save_state(uint32_t version) -> std::vector<uint8_t> {
    Archive ar = Archive::saver(version);
    serialize(ar);
    return ar.data();
}

auto System::load_state(std::span<const uint8_t> data, uint32_t version) -> bool {
    std::vector<uint8_t> backup = save_state();
    Archive ar = Archive::loader(data, version);
    serialize(ar);
    if (ar.ok() && ar.remaining() == 0) return true;
    Archive restore = Archive::loader(backup);
    serialize(restore);
    return false;
}

auto System::rom_crc32() const -> uint32_t { return bus.cart().rom_crc32(); }
auto System::sram() -> std::span<uint8_t> { return bus.cart().sram_data(); }
auto System::sram_dirty() const -> bool { return bus.cart().sram_dirty(); }
auto System::clear_sram_dirty() -> void { bus.cart().clear_sram_dirty(); }

auto System::rtc_save(int64_t now) -> std::vector<uint8_t> {
    auto* chip = bus.cart().coprocessor();
    return chip ? chip->rtc_save(now) : std::vector<uint8_t>{};
}

auto System::rtc_load(std::span<const uint8_t> data, int64_t now) -> bool {
    auto* chip = bus.cart().coprocessor();
    return chip && chip->rtc_load(data, now);
}

} // namespace snes