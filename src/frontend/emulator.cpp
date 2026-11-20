// SPDX-License-Identifier: MIT
#include "frontend/emulator.hpp"
#include "snes/savestate.hpp"
#include "snes/system.hpp"
#include "speedrun/race.hpp"
#include "utils/logging.hpp"
#include "utils/sha1.hpp"

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

// A rewind snapshot: the frame counter, the movie frame (kNoMovieFrame outside a movie), then
// the machine.
constexpr size_t kSnapshotHeader = 2 * sizeof(uint64_t);
constexpr uint64_t kNoMovieFrame = ~0ull;

auto put_u64(std::vector<uint8_t>& out, uint64_t v) -> void {
    for (size_t i = 0; i < sizeof(uint64_t); i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

auto get_u64(std::span<const uint8_t> in, size_t at) -> uint64_t {
    uint64_t v = 0;
    for (size_t i = 0; i < sizeof(uint64_t); i++) v |= static_cast<uint64_t>(in[at + i]) << (8 * i);
    return v;
}

} // namespace

Emulator::Emulator() = default;
Emulator::~Emulator() { unload(); }

auto Emulator::load(const std::string& path) -> bool {
    // Another game, or this one afresh: a movie ends (a recording is written) first.
    if (const std::string why = stop_movie(); !why.empty()) utils::Logger::error(utils::LogChannel::Rom, "Movie: %s", why.c_str());
    return boot(path, true);
}

// Loads and boots `path`. Without `battery` its battery save and clock file are left alone,
// now and until the next boot with one (a movie).
auto Emulator::boot(const std::string& path, bool battery) -> bool {
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

    close_game();  // flushes the previous game's SRAM
    battery_off_ = !battery;
    system_ = std::move(system);
    rom_path_ = path;
    rom_name_ = std::filesystem::path(path).filename().string();
    rom_sha1_ = {};
    if (const auto file = read_file(path)) rom_sha1_ = utils::sha1(*file);
    capture_input();
    if (battery) load_sram();
    else system_->clear_sram_dirty();
    load_cheats();
    load_watches();
    poked_ = false;
    auto [w, h] = system_->get_framebuffer_size();
    width_ = w;
    height_ = h;
    test_pattern_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_), kBlack);
    frames_ = 0;
    jump_ = speedrun::Jump::Reset;
    seen_output_ = false;
    update_test_pattern();
    utils::Logger::info(utils::LogChannel::Rom, "Booted %s", rom_name_.c_str());
    return true;
}

auto Emulator::unload() -> void {
    if (const std::string why = stop_movie(); !why.empty()) utils::Logger::error(utils::LogChannel::Rom, "Movie: %s", why.c_str());
    close_game();
}

auto Emulator::close_game() -> void {
    flush_sram();
    system_.reset();
    rom_path_.clear();
    rom_name_.clear();
    frames_ = 0;
}

auto Emulator::reset() -> void {
    if (!system_) return;
    if (movie_mode_ == MovieMode::Playing) {
        utils::Logger::info(utils::LogChannel::Rom, "Reset ignored: a movie is playing (its own resets are in it)");
        return;
    }
    if (movie_mode_ == MovieMode::Recording) pending_events_ |= speedrun::kEventReset;
    reset_machine();
}

auto Emulator::reset_machine() -> void {
    system_->reset();
    frames_ = 0;
    jump_ = speedrun::Jump::Reset;
    readings_.clear();
}

auto Emulator::power_cycle() -> bool {
    if (!system_) return false;
    const std::string path = rom_path_;  // boot() replaces it
    utils::Logger::info(utils::LogChannel::Rom, "Power cycle");
    if (movie_mode_ == MovieMode::Recording) pending_events_ |= speedrun::kEventPower;
    return boot(path, !battery_off_);
}

auto Emulator::set_race(bool on) -> void {
    if (on == race_) return;
    race_ = on;
    utils::Logger::info(utils::LogChannel::Rom, "Race mode %s", on ? "on" : "off");
    if (!system_) return;
    if (on) {
        // A race starts from power-on with nothing before it, so a movie ends here.
        if (const std::string why = stop_movie(); !why.empty()) utils::Logger::error(utils::LogChannel::Rom, "Movie: %s", why.c_str());
        power_cycle();  // also drops freezes and the hand-edit mark; load_cheats() holds the cheats back
    } else {
        apply_cheats();
    }
}

auto Emulator::run_frame() -> void {
    if (!system_) return;
    const speedrun::FrameInput in = next_input();  // a movie's power cycle reboots the game here
    if (!system_) return;
    apply_input(in);
    apply_freezes();
    system_->run_frame();
    frames_++;
    if (journal_.is_open() && ++unflushed_ >= flush_frames_) flush_movie();
    sample_watches();
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

// ---- Input: what the frontend sets is collected in input_ and goes to the ports at the start
// of the next frame, the one way in for live input and a movie alike.

namespace {
auto port_valid(int port) -> bool { return port >= 0 && port < static_cast<int>(speedrun::kMoviePorts); }
}  // namespace

auto Emulator::set_joypad(int port, uint16_t state) -> void { set_pad(port, 0, state); }

auto Emulator::set_port_connected(int port, bool connected) -> void {
    if (!port_valid(port)) return;
    speedrun::PortInput& p = input_.ports[static_cast<size_t>(port)];
    if (!connected) p = {bitforge::Device::None, {}};
    else if (p.device == bitforge::Device::None) p.device = bitforge::Device::Gamepad;
}

auto Emulator::set_port_device(int port, bitforge::Device device) -> void {
    if (port_valid(port)) input_.ports[static_cast<size_t>(port)].device = device;
}

auto Emulator::set_pad(int port, int slot, uint16_t state) -> void {
    if (port_valid(port) && slot >= 0 && slot < bitforge::kMultitapSlots) {
        input_.ports[static_cast<size_t>(port)].words[static_cast<size_t>(slot)] = state;
    }
}

auto Emulator::add_mouse(int port, int dx, int dy, bool left, bool right) -> void {
    if (!port_valid(port)) return;
    speedrun::PortInput& p = input_.ports[static_cast<size_t>(port)];
    if (p.device != bitforge::Device::Mouse) return;
    p = speedrun::mouse_input(static_cast<int16_t>(p.words[0]) + dx, static_cast<int16_t>(p.words[1]) + dy, left, right);
}

auto Emulator::set_scope(int port, const bitforge::ScopeInput& input) -> void {
    if (port_valid(port) && input_.ports[static_cast<size_t>(port)].device == bitforge::Device::SuperScope) {
        input_.ports[static_cast<size_t>(port)] = speedrun::scope_input(input);
    }
}

auto Emulator::capture_input() -> void {
    input_ = {};
    for (size_t p = 0; p < speedrun::kMoviePorts; p++) {
        const bitforge::ControllerPort& port = system_->controller(static_cast<int>(p));
        speedrun::PortInput& in = input_.ports[p];
        in.device = port.device();
        for (int s = 0; s < bitforge::kMultitapSlots; s++) in.words[static_cast<size_t>(s)] = port.pad(s);
        if (in.device == bitforge::Device::Mouse) in = speedrun::mouse_input(0, 0, false, false);
        if (in.device == bitforge::Device::SuperScope) in = speedrun::scope_input(port.scope());
    }
}

// The movie's next frame (its reset or power cycle done first), else the frontend's input with
// the reset or power cycle since the last frame, recorded if a movie is recording.
auto Emulator::next_input() -> speedrun::FrameInput {
    if (movie_at_end()) {
        utils::Logger::info(utils::LogChannel::Rom, "Movie finished (%zu frames): live input from here", movie_.frames.size());
        stop_movie();
    }
    if (movie_mode_ == MovieMode::Playing) {
        const speedrun::FrameInput in = movie_.frames[movie_cursor_++];
        if ((in.events & speedrun::kEventPower) && !boot(std::string(rom_path_), false)) return in;
        if (in.events & speedrun::kEventReset) reset_machine();
        return in;
    }
    speedrun::FrameInput in = input_;
    in.events = std::exchange(pending_events_, uint8_t{0});
    for (speedrun::PortInput& p : in.ports) {
        std::fill(p.words.begin() + static_cast<std::ptrdiff_t>(speedrun::device_words(p.device)), p.words.end(), uint16_t{0});
    }
    for (speedrun::PortInput& p : input_.ports) {
        if (p.device == bitforge::Device::Mouse) p.words[0] = p.words[1] = 0;  // the motion went with this frame
    }
    if (movie_mode_ == MovieMode::Recording) {
        cut_movie(movie_cursor_);
        movie_.frames.push_back(in);
        movie_cursor_++;
        if (!race_) movie_.race = false;
    }
    return in;
}

auto Emulator::apply_input(const speedrun::FrameInput& in) -> void {
    for (size_t i = 0; i < in.ports.size(); i++) {
        const int port = static_cast<int>(i);
        const speedrun::PortInput& p = in.ports[i];
        system_->set_port_device(port, p.device);
        switch (p.device) {
        case bitforge::Device::None:
            system_->set_pad(port, 0, 0);
            break;
        case bitforge::Device::Gamepad:
        case bitforge::Device::Multitap:
            for (size_t s = 0; s < speedrun::device_words(p.device); s++) system_->set_pad(port, static_cast<int>(s), p.words[s]);
            break;
        case bitforge::Device::Mouse:
            system_->add_mouse(port, static_cast<int16_t>(p.words[0]), static_cast<int16_t>(p.words[1]), p.words[2] & 1, p.words[2] & 2);
            break;
        case bitforge::Device::SuperScope:
            system_->set_scope(port, speedrun::scope_from(p));
            break;
        }
    }
}

// ---- Movies ------------------------------------------------------------------------------------

auto Emulator::record_movie(speedrun::MovieStart start, const std::string& path, const MovieMeta& meta) -> std::string {
    if (!system_) return "no game is running";
    if (start == speedrun::MovieStart::SaveState && !speedrun::race_allows(race_, speedrun::Guard::SaveState)) {
        return speedrun::blocked_message(speedrun::Guard::SaveState);
    }
    std::string why = stop_movie();
    speedrun::Movie m;
    m.id = speedrun::new_movie_id();
    m.start = start;
    m.race = race_;
    m.standard = video_standard();
    m.rom_crc32 = rom_crc32();
    m.rom_sha1 = rom_sha1_;
    m.recorded = unix_now();
    m.emulator = meta.emulator;
    m.rom_file = rom_name_;
    m.author = meta.author;
    flush_sram();  // the real battery save as it is, before the movie's blank one
    if (start == speedrun::MovieStart::PowerOn) {
        if (!boot(std::string(rom_path_), false)) return "could not restart " + rom_name_;
    } else {
        battery_off_ = true;  // the state has the save; the file stays as it is
        m.state = savestate::encode_from_state(system_->rom_crc32(), frames_, unix_now(), nullptr, 0, 0, system_->save_state());
    }
    movie_ = std::move(m);
    movie_mode_ = MovieMode::Recording;
    movie_cursor_ = 0;
    movie_path_ = path;
    pending_events_ = 0;
    start_journal();
    utils::Logger::info(utils::LogChannel::Rom, "Movie: recording %s to %s", std::string(speedrun::info(start).label).c_str(), path.c_str());
    return why;
}

auto Emulator::play_movie(speedrun::Movie movie, const std::string& path) -> std::string {
    if (!system_) return "no game is running";
    if (movie.start == speedrun::MovieStart::SaveState && !speedrun::race_allows(race_, speedrun::Guard::LoadState)) {
        return speedrun::blocked_message(speedrun::Guard::LoadState);
    }
    if (const std::string why = stop_movie(); !why.empty()) utils::Logger::error(utils::LogChannel::Rom, "Movie: %s", why.c_str());
    flush_sram();
    if (movie.start == speedrun::MovieStart::PowerOn) {
        if (!boot(std::string(rom_path_), false)) return "could not restart " + rom_name_;
    } else {
        std::string why;
        if (!restore(movie.state, why)) return "its start state: " + why;
        battery_off_ = true;
    }
    movie_ = std::move(movie);
    movie_mode_ = MovieMode::Playing;
    movie_cursor_ = 0;
    movie_path_ = path;
    pending_events_ = 0;
    utils::Logger::info(utils::LogChannel::Rom, "Movie: playing %s (%zu frames, %u rerecords)", path.c_str(), movie_.frames.size(),
                        movie_.rerecords);
    return {};
}

auto Emulator::stop_movie() -> std::string {
    if (movie_mode_ == MovieMode::Off) return {};
    std::string why;
    // What was recorded since it opened is written (temp file + rename), then the journal goes.
    // If the movie can't be written the journal stays, to be recovered.
    if ((movie_mode_ == MovieMode::Recording || journal_.is_open()) && !movie_path_.empty()) {
        if (speedrun::write_movie(movie_path_, movie_)) {
            utils::Logger::info(utils::LogChannel::Rom, "Movie: %zu frames, %u rerecords written to %s", movie_.frames.size(),
                                movie_.rerecords, movie_path_.c_str());
            journal_.close(true);
        } else {
            why = "could not write " + movie_path_;
            if (journal_.is_open()) {
                flush_movie();
                why += " (" + fs::path(journal_.path()).filename().string() + " is kept, to recover it)";
            }
        }
    }
    journal_.close(false);
    movie_ = {};
    movie_mode_ = MovieMode::Off;
    movie_cursor_ = 0;
    movie_path_.clear();
    pending_events_ = 0;
    return why;
}

auto Emulator::movie_status() const -> MovieStatus {
    MovieStatus s;
    s.mode = movie_mode_;
    s.frame = movie_cursor_;
    s.frames = movie_.frames.size();
    s.rerecords = movie_.rerecords;
    s.read_only = movie_read_only_;
    s.race = movie_.race;
    s.start = movie_.start;
    s.journal = journal_.is_open();
    s.path = movie_path_;
    return s;
}

auto Emulator::add_rerecord() -> void {
    if (movie_mode_ == MovieMode::Recording) movie_.rerecords++;
}

auto Emulator::rewind_movie(uint64_t frame) -> void {
    movie_cursor_ = static_cast<size_t>(frame);
    if (movie_mode_ == MovieMode::Recording) cut_movie(movie_cursor_);
}

auto Emulator::cut_movie(size_t frame) -> void {
    if (frame < movie_.frames.size()) movie_.frames.resize(frame);
    journal_frames_ = std::min(journal_frames_, frame);
}

auto Emulator::start_journal() -> void {
    if (journal_.is_open() || movie_path_.empty()) return;
    journal_.open(movie_path_, movie_);
    journal_frames_ = movie_.frames.size();
    journal_rerecords_ = movie_.rerecords;
    journal_race_ = movie_.race;
    unflushed_ = 0;
}

auto Emulator::flush_movie(bool wait) -> void {
    if (!journal_.is_open()) return;
    unflushed_ = 0;
    if (journal_frames_ != movie_.frames.size() || journal_rerecords_ != movie_.rerecords || journal_race_ != movie_.race) {
        journal_.append(speedrun::encode_journal_input(movie_, journal_frames_));
        journal_frames_ = movie_.frames.size();
        journal_rerecords_ = movie_.rerecords;
        journal_race_ = movie_.race;
    }
    if (wait && !journal_.sync()) utils::Logger::error(utils::LogChannel::Rom, "Movie: could not write %s", journal_.path().c_str());
}

auto Emulator::state_ref(size_t frame) -> speedrun::StateRef {
    flush_movie();  // the journal has this take, should a rerecord cut it
    return speedrun::make_state_ref(movie_, frame, journal_.id(), journal_.size());
}

auto Emulator::take_of(const speedrun::StateRef& ref, std::vector<speedrun::FrameInput>& frames) -> bool {
    if (ref.frames.size() == ref.frame) {  // an old sidecar: the take itself
        frames = ref.frames;
        return true;
    }
    if (!journal_.is_open() || ref.journal_id != journal_.id() || ref.journal_size > journal_.size() || !journal_.sync()) return false;
    speedrun::Journal journal;
    if (!speedrun::read_journal(journal_.path(), journal, ref.journal_size).empty() || !speedrun::on_take(ref, journal.movie.frames)) {
        return false;
    }
    frames.assign(journal.movie.frames.begin(), journal.movie.frames.begin() + ref.frame);
    return true;
}

auto Emulator::rom_sha1_hex() const -> std::string {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const uint8_t b : rom_sha1_) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

auto Emulator::rom_stem() const -> std::string { return fs::path(rom_path_).stem().string(); }

auto Emulator::rom_crc32() const -> uint32_t { return system_ ? system_->rom_crc32() : 0; }

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
    if (!speedrun::race_allows(race_, speedrun::Guard::SaveState)) return speedrun::blocked_message(speedrun::Guard::SaveState);
    const auto now = std::chrono::system_clock::now();
    const int64_t timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const std::vector<uint8_t> data = savestate::encode(*system_, frames_, timestamp, system_->get_framebuffer(),
                                                        width_, height_);
    const std::string path = state_path(slot);
    const bool movie = movie_mode_ != MovieMode::Off;
    const speedrun::StateRef ref = movie ? state_ref(movie_cursor_) : speedrun::StateRef{};
    if (!write_file(path, data) || !speedrun::write_state_ref(path, movie ? &ref : nullptr)) {
        utils::Logger::error(utils::LogChannel::State, "Could not write %s", path.c_str());
        return "could not write " + path;
    }
    utils::Logger::info(utils::LogChannel::State, "Saved state %d (%zu KB)%s: %s", slot, data.size() / 1024, movie ? " with the movie" : "",
                        path.c_str());
    return {};
}

auto Emulator::load_state(int slot) -> std::string {
    if (!system_) return "no game is running";
    if (!speedrun::race_allows(race_, speedrun::Guard::LoadState)) return speedrun::blocked_message(speedrun::Guard::LoadState);
    const std::string path = state_path(slot);
    const auto data = read_file(path);
    if (!data) {
        utils::Logger::info(utils::LogChannel::State, "State slot %d is empty", slot);
        return "slot " + std::to_string(slot) + " is empty";
    }
    // During a movie a state goes back to its frame of the movie: it must have been saved in
    // this movie, and in read-only mode on the take playing. Read+write, a take a rerecord cut
    // comes back from the journal.
    speedrun::StateRef ref;
    std::vector<speedrun::FrameInput> take;
    bool same_take = true;
    const bool movie = movie_mode_ != MovieMode::Off;
    if (movie) {
        const std::string slot_name = "state " + std::to_string(slot);
        if (!speedrun::read_state_ref(path, ref) || ref.movie_id != movie_.id) return slot_name + " wasn't saved during this movie";
        same_take = speedrun::on_take(ref, movie_.frames);
        if (!same_take && movie_read_only_) return slot_name + " is from another take of this movie (turn read-only off to record from it)";
        if (!same_take && !take_of(ref, take)) {
            return slot_name + " is from another take of this movie that is no longer kept (takes are kept while the movie stays open)";
        }
    }
    std::string why;
    if (!restore(*data, why)) {
        why = "Can't load state " + std::to_string(slot) + ": " + why;
        utils::Logger::warning(utils::LogChannel::State, "%s [%s]", why.c_str(), path.c_str());
        return why;
    }
    if (movie) {
        movie_cursor_ = ref.frame;
        if (movie_read_only_) {
            movie_mode_ = MovieMode::Playing;
        } else {
            if (same_take) {
                cut_movie(ref.frame);
            } else {
                const auto common = std::mismatch(take.begin(), take.end(), movie_.frames.begin(), movie_.frames.end()).first;
                cut_movie(static_cast<size_t>(common - take.begin()));  // the journal keeps what both takes share
                movie_.frames = std::move(take);
            }
            movie_.rerecords++;
            movie_mode_ = MovieMode::Recording;
            start_journal();
        }
        utils::Logger::info(utils::LogChannel::Rom, "Movie: %s from frame %zu (%u rerecords)", movie_read_only_ ? "playing" : "recording",
                            movie_cursor_, movie_.rerecords);
    }
    utils::Logger::info(utils::LogChannel::State, "Loaded state %d: %s", slot, path.c_str());
    return {};
}

// A save-state file into the machine: false (and `why`) if it doesn't fit this game or build.
auto Emulator::restore(std::span<const uint8_t> file, std::string& why) -> bool {
    savestate::Info info;
    const savestate::Result result = savestate::decode(*system_, file, info);
    if (result != savestate::Result::Ok) {
        why = savestate::describe(result);
        if (result == savestate::Result::WrongVersion) {
            why += " (file v" + std::to_string(info.version) + ", this build v" + std::to_string(savestate::kVersion) + ")";
        }
        return false;
    }
    frames_ = info.frame;
    jump_ = speedrun::Jump::Load;
    readings_.clear();
    const auto [w, h] = system_->get_framebuffer_size();
    width_ = w;
    height_ = h;
    seen_output_ = true;
    return true;
}

auto Emulator::save_snapshot() -> std::vector<uint8_t> {
    if (!system_) return {};
    std::vector<uint8_t> data;
    put_u64(data, frames_);
    put_u64(data, movie_mode_ != MovieMode::Off ? movie_cursor_ : kNoMovieFrame);
    const std::vector<uint8_t> state = system_->save_state();
    data.insert(data.end(), state.begin(), state.end());
    return data;
}

auto Emulator::load_snapshot(std::span<const uint8_t> data) -> bool {
    if (!system_ || data.size() <= kSnapshotHeader) return false;
    if (!speedrun::race_allows(race_, speedrun::Guard::Rewind)) return false;  // rewind and the timeline
    const uint64_t movie_frame = get_u64(data, sizeof(uint64_t));
    const bool movie = movie_mode_ != MovieMode::Off;
    if (movie && (movie_frame == kNoMovieFrame || movie_frame > movie_.frames.size())) return false;  // from before the movie
    if (!system_->load_state(data.subspan(kSnapshotHeader))) return false;
    frames_ = get_u64(data, 0);
    if (movie) rewind_movie(movie_frame);
    jump_ = speedrun::Jump::Rewind;
    readings_.clear();
    const auto [w, h] = system_->get_framebuffer_size();
    width_ = w;
    height_ = h;
    seen_output_ = true;
    return true;
}

auto Emulator::save_snapshot_to_slot(int slot, std::span<const uint8_t> snapshot, const uint32_t* thumb, int thumb_w,
                                     int thumb_h) -> std::string {
    if (!system_) return "no game is running";
    if (!speedrun::race_allows(race_, speedrun::Guard::Timeline)) return speedrun::blocked_message(speedrun::Guard::Timeline);
    if (slot < 1 || slot > kStateSlots) return "that slot does not exist";
    if (snapshot.size() <= kSnapshotHeader) return "that moment is no longer in the timeline";
    const uint64_t frame = get_u64(snapshot, 0);
    const uint64_t movie_frame = get_u64(snapshot, sizeof(uint64_t));
    const bool movie = movie_mode_ != MovieMode::Off && movie_frame != kNoMovieFrame && movie_frame <= movie_.frames.size();
    const speedrun::StateRef ref = movie ? state_ref(static_cast<size_t>(movie_frame)) : speedrun::StateRef{};
    const bool have_thumb = thumb && thumb_w > 0 && thumb_h > 0;
    const std::vector<uint8_t> data = savestate::encode_from_state(
        system_->rom_crc32(), frame, unix_now(), have_thumb ? thumb : nullptr, have_thumb ? thumb_w : 0,
        have_thumb ? thumb_h : 0, snapshot.subspan(kSnapshotHeader));
    const std::string path = state_path(slot);
    if (!write_file(path, data) || !speedrun::write_state_ref(path, movie ? &ref : nullptr)) {
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
    apply_cheats();
}

auto Emulator::apply_cheats() -> void {
    if (!system_) return;
    if (speedrun::race_allows(race_, speedrun::Guard::Cheats)) system_->set_cheats(cheats_);
    else system_->set_cheats({});
}

auto Emulator::set_cheats(std::vector<CheatEntry> entries) -> bool {
    if (!system_) return false;
    cheats_ = std::move(entries);
    apply_cheats();
    const std::string path = cheats_path();
    std::error_code ec;
    if (cheats_.empty() && !fs::exists(path, ec)) return true;
    const std::string text = format_cheat_file(cheats_);
    return write_file(path, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

auto Emulator::watches_path() const -> std::string {
    return (fs::path(data_dir_) / kWatchesFolder / (rom_stem() + ".wch")).string();
}

auto Emulator::load_watches() -> void {
    watches_.clear();
    readings_.clear();
    watches_revision_++;
    if (const auto data = read_file(watches_path())) {
        watches_ = debug::parse_watch_file(std::string_view(reinterpret_cast<const char*>(data->data()), data->size()));
        utils::Logger::info(utils::LogChannel::Rom, "Memory watches: %zu loaded (%s)", watches_.size(), watches_path().c_str());
    }
}

auto Emulator::set_watches(std::vector<debug::Watch> entries) -> bool {
    if (!system_) return false;
    watches_ = std::move(entries);
    if (!speedrun::race_allows(race_, speedrun::Guard::Freeze)) {
        for (debug::Watch& w : watches_) w.frozen = false;
    }
    debug::assign_watch_ids(watches_);
    watches_revision_++;
    apply_freezes();
    const std::string path = watches_path();
    std::error_code ec;
    if (watches_.empty() && !fs::exists(path, ec)) return true;
    const std::string text = debug::format_watch_file(watches_);
    return write_file(path, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

auto Emulator::memory(debug::Region region) -> std::span<uint8_t> {
    if (!system_) return {};
    return region == debug::Region::Wram ? system_->wram() : system_->sram();
}

auto Emulator::write_memory(debug::Region region, uint32_t offset, debug::ValueSize size, uint32_t value) -> bool {
    if (!speedrun::race_allows(race_, speedrun::Guard::MemoryEdit)) return false;
    if (!debug::write_value(memory(region), offset, size, value)) return false;
    if (region == debug::Region::Sram) system_->mark_sram_dirty();  // so the battery file follows
    poked_ = true;
    return true;
}

auto Emulator::evaluate(const debug::Condition& condition) const -> bool {
    return debug::evaluate(condition, watches_, readings_);
}

auto Emulator::memory_tampered() const -> uint32_t {
    uint32_t bits = 0;
    if (debug::any_frozen(watches_)) bits |= debug::kTamperFreeze;
    const bool cheats_apply = speedrun::race_allows(race_, speedrun::Guard::Cheats);
    if (cheats_apply && std::any_of(cheats_.begin(), cheats_.end(), [](const CheatEntry& c) { return c.enabled; })) {
        bits |= debug::kTamperCheat;
    }
    if (poked_) bits |= debug::kTamperPoke;
    return bits;
}

auto Emulator::step_timer(speedrun::RunTimer& timer, const speedrun::Clock& now) -> speedrun::Event {
    const speedrun::FrameInfo frame{frames_, take_jump(), memory_tampered(), now, race_, movie_mode_ == MovieMode::Playing};
    return timer.on_frame(frame, [this](const debug::Condition& c) { return evaluate(c); });
}

auto Emulator::region_memory() -> debug::RegionMemory {
    debug::RegionMemory regions;
    for (const auto& r : debug::kRegions) regions[static_cast<size_t>(r.id)] = memory(r.id);
    return regions;
}

auto Emulator::sample_watches() -> void {
    if (watches_.empty() && readings_.empty()) return;
    std::swap(readings_, last_readings_);
    debug::sample_watches(watches_, region_memory(), last_readings_, readings_);
}

auto Emulator::apply_freezes() -> void {
    if (watches_.empty() || !speedrun::race_allows(race_, speedrun::Guard::Freeze)) return;
    const uint32_t changed = debug::apply_freezes(watches_, region_memory());
    if (changed & (1u << static_cast<uint32_t>(debug::Region::Sram))) system_->mark_sram_dirty();
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
    if (!system_ || battery_off_) return;
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
