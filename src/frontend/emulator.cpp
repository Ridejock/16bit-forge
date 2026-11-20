// SPDX-License-Identifier: MIT
#include "frontend/emulator.hpp"
#include "snes/savestate.hpp"
#include "snes/system.hpp"
#include "utils/logging.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace bitforge::frontend {

namespace {

constexpr uint32_t kBlack = 0xFF000000;

// SMPTE-style bars so an idle PPU still shows that the window and texture path work.
constexpr std::array<uint32_t, 7> kBars = {
    0xFFC0C0C0, 0xFFC0C000, 0xFF00C0C0, 0xFF00C000, 0xFFC000C0, 0xFFC00000, 0xFF0000C0,
};
constexpr uint32_t kSweepColor = 0xFFFFFFFF;
constexpr const char* kStatesFolder = "states";
constexpr const char* kSavesFolder = "saves";
constexpr const char* kCheatsFolder = "cheats";
constexpr const char* kWatchesFolder = "watches";
constexpr const char* kFirmwareFolder = "firmware";  // user-supplied coprocessor firmware

auto read_file(const std::string& path) -> std::optional<std::vector<uint8_t>> {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

auto unix_now() -> int64_t {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Writes to <path>.tmp and renames, so a crash mid-write never leaves a half file behind.
auto write_file(const std::string& path, std::span<const uint8_t> data) -> bool {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}
constexpr int kSweepWidth = 4;
constexpr int kBarsHeightPercent = 75;

} // namespace

Emulator::Emulator() = default;
Emulator::~Emulator() { unload(); }

auto Emulator::load(const std::string& path) -> bool {
    auto system = std::make_unique<System>();
    system->set_firmware_dir((fs::path(data_dir_) / kFirmwareFolder).string());
    last_error_.clear();
    last_warning_.clear();
    if (!system->load_rom(path)) {
        last_error_ = system->load_error().empty() ? "Could not load " + std::filesystem::path(path).filename().string()
                                                   : system->load_error();
        utils::Logger::error(utils::LogChannel::Rom, "Could not load ROM: %s", path.c_str());
        return false;
    }
    last_warning_ = system->load_warning();
    system->reset();

    unload();  // flushes the previous game's SRAM
    system_ = std::move(system);
    rom_path_ = path;
    rom_name_ = std::filesystem::path(path).filename().string();
    load_sram();
    load_cheats();
    load_watches();
    auto [w, h] = system_->get_framebuffer_size();
    width_ = w;
    height_ = h;
    test_pattern_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_), kBlack);
    frames_ = 0;
    seen_output_ = false;
    update_test_pattern();
    utils::Logger::info(utils::LogChannel::Rom, "Booted %s", rom_name_.c_str());
    return true;
}

auto Emulator::unload() -> void {
    flush_sram();
    system_.reset();
    rom_path_.clear();
    rom_name_.clear();
    frames_ = 0;
}

auto Emulator::reset() -> void {
    if (!system_) return;
    system_->reset();
    frames_ = 0;
}

auto Emulator::run_frame() -> void {
    if (!system_) return;
    system_->run_frame();
    frames_++;
    if (frames_ % kSramFlushFrames == 0) flush_sram();

    // The core picks its picture size per frame (hi-res, interlace, overscan); the test
    // pattern keeps the size it was made at.
    const auto [w, h] = system_->get_framebuffer_size();
    if (!seen_output_) {
        const uint32_t* fb = system_->get_framebuffer();
        const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
        seen_output_ = std::any_of(fb, fb + count, [](uint32_t px) { return px != kBlack; });
        if (!seen_output_) update_test_pattern();
    }
    if (seen_output_) {
        width_ = w;
        height_ = h;
    }
}

auto Emulator::framebuffer() const -> const uint32_t* {
    if (!system_) return nullptr;
    return seen_output_ ? system_->get_framebuffer() : test_pattern_.data();
}

auto Emulator::set_joypad(int port, uint16_t state) -> void {
    if (system_) system_->set_joypad_state(port, state);
}

auto Emulator::set_port_connected(int port, bool connected) -> void {
    if (system_) system_->set_port_connected(port, connected);
}

auto Emulator::set_port_device(int port, bitforge::Device device) -> void {
    if (system_) system_->set_port_device(port, device);
}

auto Emulator::set_pad(int port, int slot, uint16_t state) -> void {
    if (system_) system_->set_pad(port, slot, state);
}

auto Emulator::add_mouse(int port, int dx, int dy, bool left, bool right) -> void {
    if (system_) system_->add_mouse(port, dx, dy, left, right);
}

auto Emulator::set_scope(int port, const bitforge::ScopeInput& input) -> void {
    if (system_) system_->set_scope(port, input);
}

auto Emulator::rom_stem() const -> std::string { return fs::path(rom_path_).stem().string(); }

auto Emulator::state_path(int slot) const -> std::string { return state_path_for(data_dir_, rom_path_, slot); }

auto Emulator::state_path_for(const std::string& data_dir, const std::string& rom_path, int slot) -> std::string {
    const std::string stem = fs::path(rom_path).stem().string();
    return (fs::path(data_dir) / kStatesFolder / (stem + "." + std::to_string(slot) + ".sst")).string();
}

auto Emulator::sram_path() const -> std::string {
    return (fs::path(data_dir_) / kSavesFolder / (rom_stem() + ".srm")).string();
}

auto Emulator::save_state(int slot) -> std::string {
    if (!system_) return "no game is running";
    const auto now = std::chrono::system_clock::now();
    const int64_t timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const std::vector<uint8_t> data = savestate::encode(*system_, frames_, timestamp, system_->get_framebuffer(),
                                                        width_, height_);
    const std::string path = state_path(slot);
    if (!write_file(path, data)) {
        utils::Logger::error(utils::LogChannel::State, "Could not write %s", path.c_str());
        return "could not write " + path;
    }
    utils::Logger::info(utils::LogChannel::State, "Saved state %d (%zu KB): %s", slot, data.size() / 1024, path.c_str());
    return {};
}

auto Emulator::load_state(int slot) -> std::string {
    if (!system_) return "no game is running";
    const std::string path = state_path(slot);
    const auto data = read_file(path);
    if (!data) {
        utils::Logger::info(utils::LogChannel::State, "State slot %d is empty", slot);
        return "slot " + std::to_string(slot) + " is empty";
    }
    savestate::Info info;
    const savestate::Result result = savestate::decode(*system_, *data, info);
    if (result != savestate::Result::Ok) {
        std::string why = "Can't load state " + std::to_string(slot) + ": " + savestate::describe(result);
        if (result == savestate::Result::WrongVersion) {
            why += " (file v" + std::to_string(info.version) + ", this build v" + std::to_string(savestate::kVersion) + ")";
        }
        utils::Logger::warning(utils::LogChannel::State, "%s [%s]", why.c_str(), path.c_str());
        return why;
    }
    frames_ = info.frame;
    const auto [w, h] = system_->get_framebuffer_size();
    width_ = w;
    height_ = h;
    seen_output_ = true;
    utils::Logger::info(utils::LogChannel::State, "Loaded state %d: %s", slot, path.c_str());
    return {};
}

auto Emulator::save_snapshot() -> std::vector<uint8_t> {
    if (!system_) return {};
    std::vector<uint8_t> data(sizeof(uint64_t));
    for (size_t i = 0; i < sizeof(uint64_t); i++) data[i] = static_cast<uint8_t>(frames_ >> (8 * i));
    const std::vector<uint8_t> state = system_->save_state();
    data.insert(data.end(), state.begin(), state.end());
    return data;
}

auto Emulator::load_snapshot(std::span<const uint8_t> data) -> bool {
    if (!system_ || data.size() <= sizeof(uint64_t)) return false;
    if (!system_->load_state(data.subspan(sizeof(uint64_t)))) return false;
    frames_ = 0;
    for (size_t i = 0; i < sizeof(uint64_t); i++) frames_ |= static_cast<uint64_t>(data[i]) << (8 * i);
    const auto [w, h] = system_->get_framebuffer_size();
    width_ = w;
    height_ = h;
    seen_output_ = true;
    return true;
}

auto Emulator::save_snapshot_to_slot(int slot, std::span<const uint8_t> snapshot, const uint32_t* thumb, int thumb_w,
                                     int thumb_h) -> std::string {
    if (!system_) return "no game is running";
    if (slot < 1 || slot > kStateSlots) return "that slot does not exist";
    if (snapshot.size() <= sizeof(uint64_t)) return "that moment is no longer in the timeline";
    uint64_t frame = 0;
    for (size_t i = 0; i < sizeof(uint64_t); i++) frame |= static_cast<uint64_t>(snapshot[i]) << (8 * i);
    const bool have_thumb = thumb && thumb_w > 0 && thumb_h > 0;
    const std::vector<uint8_t> data = savestate::encode_from_state(
        system_->rom_crc32(), frame, unix_now(), have_thumb ? thumb : nullptr, have_thumb ? thumb_w : 0,
        have_thumb ? thumb_h : 0, snapshot.subspan(sizeof(uint64_t)));
    const std::string path = state_path(slot);
    if (!write_file(path, data)) {
        utils::Logger::error(utils::LogChannel::State, "Could not write %s", path.c_str());
        return "could not write " + path;
    }
    utils::Logger::info(utils::LogChannel::State, "Saved timeline moment to state %d (%zu KB): %s", slot, data.size() / 1024,
                        path.c_str());
    return {};
}

auto Emulator::slot_info(int slot) const -> std::optional<SlotInfo> {
    if (!system_) return std::nullopt;
    return read_slot_info(state_path(slot));
}

auto Emulator::read_slot_info(const std::string& path) -> std::optional<SlotInfo> {
    // Only the header and thumbnail are needed; they sit at the start of the file. (States are
    // written to a .tmp file and renamed, so a reader never sees half a file.)
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    constexpr size_t kHeaderBytes = 36;
    constexpr size_t kMaxThumbBytes = 512 * 512 * 4;  // the format's limit (thumbnails are ~128x112)
    std::vector<uint8_t> head(kHeaderBytes + kMaxThumbBytes);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(in.gcount()));
    savestate::Info info;
    if (savestate::read_info(head, info) != savestate::Result::Ok) return std::nullopt;
    return SlotInfo{info.timestamp, info.frame, info.version, info.thumb_w, info.thumb_h, std::move(info.thumbnail)};
}

auto Emulator::age_text(int64_t saved, int64_t now) -> std::string {
    const int64_t s = now - saved;
    if (s < 0) return "in the future";
    if (s < 60) return "just now";
    if (s < 3600) return std::to_string(s / 60) + " min ago";
    if (s < 86400) return std::to_string(s / 3600) + " h ago";
    const int64_t days = s / 86400;
    return days == 1 ? "yesterday" : std::to_string(days) + " days ago";
}

auto Emulator::play_time_text(uint64_t frames) -> std::string {
    const auto total = static_cast<uint64_t>(static_cast<double>(frames) / kNtscFrameRate);
    char text[32];
    if (total >= 3600) {
        std::snprintf(text, sizeof(text), "%llu:%02llu:%02llu", static_cast<unsigned long long>(total / 3600),
                      static_cast<unsigned long long>(total / 60 % 60), static_cast<unsigned long long>(total % 60));
    } else {
        std::snprintf(text, sizeof(text), "%llu:%02llu", static_cast<unsigned long long>(total / 60),
                      static_cast<unsigned long long>(total % 60));
    }
    return text;
}

auto Emulator::cheats_path() const -> std::string {
    return (fs::path(data_dir_) / kCheatsFolder / (rom_stem() + ".cht")).string();
}

auto Emulator::load_cheats() -> void {
    cheats_.clear();
    if (const auto data = read_file(cheats_path())) {
        cheats_ = parse_cheat_file(std::string_view(reinterpret_cast<const char*>(data->data()), data->size()));
        const auto on = std::count_if(cheats_.begin(), cheats_.end(), [](const CheatEntry& e) { return e.enabled; });
        utils::Logger::info(utils::LogChannel::Rom, "Cheats: %zu loaded, %td on (%s)", cheats_.size(), on, cheats_path().c_str());
    }
    system_->set_cheats(cheats_);
}

auto Emulator::set_cheats(std::vector<CheatEntry> entries) -> bool {
    if (!system_) return false;
    cheats_ = std::move(entries);
    system_->set_cheats(cheats_);
    const std::string path = cheats_path();
    std::error_code ec;
    if (cheats_.empty() && !fs::exists(path, ec)) return true;
    const std::string text = format_cheat_file(cheats_);
    return write_file(path, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

auto Emulator::watches_path() const -> std::string {
    return (fs::path(data_dir_) / kWatchesFolder / (rom_stem() + ".wch")).string();
}

auto Emulator::watches() -> RamWatch& { return system_->watches(); }

auto Emulator::watches() const -> const RamWatch& { return system_->watches(); }

auto Emulator::load_watches() -> void {
    if (const auto data = read_file(watches_path())) {
        system_->watches().parse(std::string_view(reinterpret_cast<const char*>(data->data()), data->size()));
        utils::Logger::info(utils::LogChannel::Rom, "Watches: %zu loaded (%s)", system_->watches().list().size(),
                            watches_path().c_str());
    }
}

auto Emulator::save_watches() -> bool {
    if (!system_) return false;
    const std::string path = watches_path();
    std::error_code ec;
    if (system_->watches().list().empty() && !fs::exists(path, ec)) return true;
    const std::string text = system_->watches().format();
    return write_file(path, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

auto Emulator::region_bytes(WatchDomain domain) -> std::vector<uint8_t> {
    if (!system_) return {};
    return system_->copy_memory(domain);
}

auto Emulator::memory_view(uint32_t hit_limit) -> MemorySearchView {
    MemorySearchView view;
    if (!system_) return view;
    const RamWatch& ram = system_->watches();
    view.searching = ram.searching();
    view.domain = ram.search_domain();
    view.candidates = ram.candidate_count();
    view.sram_bytes = system_->sram().size();
    view.watches.assign(ram.list().begin(), ram.list().end());
    view.marks = ram.marks();
    if (view.searching && view.candidates > 0 && view.candidates <= hit_limit) {
        for (const uint32_t offset : ram.first_candidates(hit_limit)) {
            const std::optional<uint8_t> byte = system_->read_memory(view.domain, offset);
            if (byte) view.hits.push_back({offset, *byte});
        }
    }
    return view;
}

auto Emulator::run_marks() const -> RunMarks {
    return system_ ? system_->watches().marks() : RunMarks{};
}

auto Emulator::peek_wram(uint32_t offset) const -> uint8_t {
    return system_ ? system_->peek_wram(offset) : 0;
}

auto Emulator::edit_watch(uint32_t id, uint32_t value) -> bool {
    if (!system_) return false;
    const Watch* watch = system_->watches().find(id);
    if (!watch) return false;
    const WatchDomain domain = watch->domain;
    const uint32_t offset = watch->offset;
    const uint8_t size = watch->size;
    bool wrote = false;
    for (uint8_t i = 0; i < size; ++i) {
        wrote = system_->write_memory(domain, offset + i, static_cast<uint8_t>((value >> (8 * i)) & 0xFF)) || wrote;
    }
    if (wrote) system_->watches().note_hand_edit();
    return wrote;
}

auto Emulator::freeze_watch(uint32_t id, bool frozen, uint32_t value) -> bool {
    if (!system_) return false;
    const Watch* watch = system_->watches().find(id);
    if (!watch) return false;
    const WatchDomain domain = watch->domain;
    const uint32_t offset = watch->offset;
    const uint8_t size = watch->size;
    if (!frozen) return system_->watches().set_frozen(id, false, value);
    bool wrote = false;
    for (uint8_t i = 0; i < size; ++i) {
        wrote = system_->write_memory(domain, offset + i, static_cast<uint8_t>((value >> (8 * i)) & 0xFF)) || wrote;
    }
    if (!wrote) return false;
    system_->watches().set_frozen(id, true, value);
    system_->watches().note_freeze_applied();
    return true;
}

auto Emulator::set_watch_size(uint32_t id, uint8_t size) -> bool {
    if (!system_) return false;
    const Watch* watch = system_->watches().find(id);
    if (!watch) return false;
    if (watch->domain == WatchDomain::Sram && static_cast<std::size_t>(watch->offset) + size > system_->sram().size()) return false;
    if (!system_->watches().set_size(id, size)) return false;
    return save_watches();
}

auto Emulator::rtc_path() const -> std::string {
    return (fs::path(data_dir_) / kSavesFolder / (rom_stem() + ".rtc")).string();
}

auto Emulator::load_sram() -> void {
    if (!system_->rtc_save(0).empty()) {  // the cartridge has a clock
        const std::string path = rtc_path();
        if (const auto data = read_file(path)) {
            if (system_->rtc_load(*data, unix_now())) {
                utils::Logger::info(utils::LogChannel::Rom, "Clock loaded (advanced by the time since it was saved): %s", path.c_str());
            } else {
                utils::Logger::warning(utils::LogChannel::Rom, "Ignoring %s: %zu bytes is not a clock file", path.c_str(), data->size());
            }
        }
    }
    const std::span<uint8_t> sram = system_->sram();
    if (sram.empty()) return;
    const std::string path = sram_path();
    if (const auto data = read_file(path)) {
        if (data->size() == sram.size()) {
            std::copy(data->begin(), data->end(), sram.begin());
            utils::Logger::info(utils::LogChannel::Rom, "Battery save loaded: %s", path.c_str());
        } else {
            utils::Logger::warning(utils::LogChannel::Rom, "Ignoring %s: %zu bytes, the cartridge has %zu", path.c_str(),
                                   data->size(), sram.size());
        }
    }
    system_->clear_sram_dirty();
}

auto Emulator::flush_sram() -> void {
    if (!system_) return;
    if (const auto clock = system_->rtc_save(unix_now()); !clock.empty() && !write_file(rtc_path(), clock)) {
        utils::Logger::error(utils::LogChannel::Rom, "Could not write clock file %s", rtc_path().c_str());
    }
    if (!system_->sram_dirty() || system_->sram().empty()) return;
    const std::string path = sram_path();
    if (write_file(path, system_->sram())) {
        system_->clear_sram_dirty();
        utils::Logger::debug(utils::LogChannel::Rom, "Battery save written: %s", path.c_str());
    } else {
        utils::Logger::error(utils::LogChannel::Rom, "Could not write battery save %s", path.c_str());
    }
}

auto Emulator::msu1_samples() const -> std::span<const int16_t> {
    if (!system_) return {};
    return system_->msu1_samples();
}

auto Emulator::has_msu1() const -> bool { return system_ && system_->msu1(); }

auto Emulator::audio_samples() const -> std::span<const int16_t> {
    if (!system_) return {};
    return system_->audio_samples();
}

auto Emulator::audio_sample_rate() -> int { return apu::APU::kSampleRate; }

auto Emulator::update_test_pattern() -> void {
    const int bars_height = height_ * kBarsHeightPercent / 100;
    const int bar_width = (width_ + static_cast<int>(kBars.size()) - 1) / static_cast<int>(kBars.size());
    const int sweep_x = static_cast<int>(frames_ % static_cast<uint64_t>(width_));

    for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            uint32_t color;
            if (y < bars_height) {
                color = kBars[static_cast<size_t>(std::min(x / bar_width, static_cast<int>(kBars.size()) - 1))];
            } else {
                // Lower band: a gray ramp, so scaling and filtering are easy to judge.
                uint32_t level = static_cast<uint32_t>(x * 255 / std::max(1, width_ - 1));
                color = kBlack | (level << 16) | (level << 8) | level;
            }
            // A moving sweep line proves frames are advancing.
            if (x >= sweep_x && x < sweep_x + kSweepWidth) color = kSweepColor;
            test_pattern_[static_cast<size_t>(y * width_ + x)] = color;
        }
    }
}

} // namespace bitforge::frontend
