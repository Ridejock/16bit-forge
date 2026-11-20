// SPDX-License-Identifier: MIT
#pragma once
// Debug > Memory watch: the RAM search and the watch list, drawn into the Memory tab. The search
// and the watches themselves live in src/debug (no ImGui); this is only their UI. Memory is read
// from the image the emulation thread publishes once per frame while the tab is showing, and
// every write (a value edit, a freeze, the watch list) goes through the command queue. In race
// mode, freezes, edits and the Cheat button are greyed out (the core refuses them anyway).
#include "debug/memory_watch.hpp"
#include "debug/ram_search.hpp"
#include "frontend/emu_thread.hpp"
#include "snes/cheats.hpp"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace bitforge::frontend {

class MemoryPanel {
public:
    using AddCheat = std::function<void(CheatEntry)>;
    using Notify = std::function<void(std::string)>;

    explicit MemoryPanel(EmuThread& emu) : emu_(emu) {}

    // The tab's contents for the running game. `add_cheat` appends a code to the Cheats tab's
    // list; `notify` reports what happened (status bar and game window).
    auto draw(const EmuStatus& status, const AddCheat& add_cheat, const Notify& notify) -> void;

private:
    auto sync_game(const EmuStatus& status) -> void;
    auto draw_search(const MemoryImage& memory) -> void;
    auto draw_watches(const MemoryImage& memory, const AddCheat& add_cheat, const Notify& notify) -> void;
    auto draw_add_watch(const MemoryImage& memory) -> void;
    auto run_search(const MemoryImage& memory) -> void;
    auto add_watch(debug::Region region, uint32_t offset, debug::ValueType type, std::string label) -> void;
    auto commit() -> void;  // the watch list to the emulation thread (and its file)

    EmuThread& emu_;
    std::string rom_;  // the game the watches and the search belong to
    std::vector<debug::Watch> watches_;
    uint64_t revision_ = 0;  // the emulator's watches_revision() this copy is from

    debug::RamSearch search_;
    debug::Region search_region_ = debug::Region::Wram;
    debug::Region region_ = debug::Region::Wram;  // the combos' choice; a change starts a new search
    debug::ValueType type_;
    debug::Compare compare_ = debug::Compare::Equal;
    std::array<char, 32> operand_{};
    std::string search_error_;

    debug::Region add_region_ = debug::Region::Wram;
    debug::ValueType add_type_;
    std::array<char, 32> add_address_{};
    std::array<char, 64> add_label_{};
    std::string watch_error_;

    bool status_race_ = false;  // race mode: no freezes, edits or cheats from here
    int edit_row_ = -1;  // the watch whose value field is being typed in
    std::array<char, 32> edit_value_{};
};

} // namespace bitforge::frontend
