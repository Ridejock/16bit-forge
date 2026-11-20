// SPDX-License-Identifier: MIT
// RAM search and memory watches. No window, no audio device, no ROM files.
#include "frontend/emulator.hpp"
#include "snes/cheats.hpp"
#include "snes/ramwatch.hpp"
#include "snes/system.hpp"
#include "test_rom.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

int checks_failed = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("\n    check failed: %s (line %d)", #cond, __LINE__); \
            checks_failed++;                                                  \
        }                                                                     \
    } while (0)

using bitforge::RamWatch;
using bitforge::RunMarks;
using bitforge::SearchCompare;
using bitforge::WatchDomain;

auto test_search_narrows() -> void {
    std::vector<uint8_t> mem(RamWatch::kWramBytes, 0);
    mem[0x10] = 3;
    mem[0x1FE8] = 3;
    mem[0x10000] = 3;

    RamWatch ram;
    CHECK(!ram.narrow(mem, SearchCompare::Equal, uint8_t{3}));
    ram.start_search(WatchDomain::Wram, mem);
    CHECK(ram.searching() && ram.candidate_count() == RamWatch::kWramBytes);
    CHECK(ram.narrow(mem, SearchCompare::Equal, uint8_t{3}));
    CHECK(ram.candidate_count() == 3);
    const uint32_t before = ram.candidate_count();
    CHECK(!ram.narrow(mem, SearchCompare::Equal, std::nullopt));
    CHECK(ram.candidate_count() == before);

    mem[0x1FE8] = 2;
    CHECK(ram.narrow(mem, SearchCompare::Equal, uint8_t{2}));
    const auto hits = ram.first_candidates(8);
    CHECK(hits.size() == 1 && hits[0] == 0x1FE8);
    CHECK(bitforge::format_address(WatchDomain::Wram, 0x1FE8) == "$7E1FE8");
    CHECK(bitforge::format_address(WatchDomain::Wram, 0x10000) == "$7F0000");
    CHECK(bitforge::format_address(WatchDomain::Sram, 0x10) == "SRAM:$10");

    // The same byte, found by "it went down" instead of a second known value.
    RamWatch again;
    mem[0x1FE8] = 3;
    again.start_search(WatchDomain::Wram, mem);
    CHECK(again.narrow(mem, SearchCompare::Equal, uint8_t{3}));
    mem[0x1FE8] = 2;
    CHECK(again.narrow(mem, SearchCompare::Decreased, std::nullopt));
    CHECK(again.candidate_count() == 1 && again.first_candidates(1)[0] == 0x1FE8);

    // A capture compared with itself has not changed.
    RamWatch fresh;
    fresh.start_search(WatchDomain::Wram, mem);
    CHECK(fresh.narrow(mem, SearchCompare::Changed, std::nullopt));
    CHECK(fresh.candidate_count() == 0);
}

auto test_search_compares() -> void {
    const std::vector<uint8_t> start{1, 5, 5, 9};
    std::vector<uint8_t> now = start;
    RamWatch ram;
    ram.start_search(WatchDomain::Wram, start);
    CHECK(ram.narrow(now, SearchCompare::Greater, uint8_t{5}));
    CHECK(ram.candidate_count() == 1 && ram.first_candidates(4)[0] == 3);

    ram.start_search(WatchDomain::Wram, start);
    CHECK(ram.narrow(now, SearchCompare::Less, uint8_t{5}));
    CHECK(ram.candidate_count() == 1 && ram.first_candidates(4)[0] == 0);

    ram.start_search(WatchDomain::Wram, start);
    CHECK(ram.narrow(now, SearchCompare::NotEqual, uint8_t{5}));
    CHECK(ram.candidate_count() == 2);

    now[0] = 2;
    now[1] = 4;
    ram.start_search(WatchDomain::Wram, start);
    CHECK(ram.narrow(now, SearchCompare::Increased, std::nullopt));
    CHECK(ram.candidate_count() == 1 && ram.first_candidates(4)[0] == 0);
    ram.start_search(WatchDomain::Wram, start);
    CHECK(ram.narrow(now, SearchCompare::Unchanged, std::nullopt));
    const auto same = ram.first_candidates(4);
    CHECK(same.size() == 2 && same[0] == 2 && same[1] == 3);
}

auto test_ids_file_and_condition() -> void {
    RamWatch ram;
    const uint32_t first = ram.add_watch(WatchDomain::Wram, 0x1FE8, 1, "HP");
    const uint32_t second = ram.add_watch(WatchDomain::Wram, 0x10, 1, "other");
    CHECK(first == 1 && second == 2);
    CHECK(ram.remove_watch(first));
    CHECK(ram.add_watch(WatchDomain::Wram, 0x20, 1, "third") == 3);
    CHECK(ram.add_watch(WatchDomain::Wram, 0x1FFFF, 2, "nope") == 0);
    CHECK(ram.add_watch(WatchDomain::Wram, 0x1FFFF, 1, "edge") != 0);

    CHECK(ram.set_label(second, "say \"hi\""));
    CHECK(ram.set_condition(second, {true, 3, 4}));
    int reads = 0;
    uint8_t byte = 3;
    RamWatch cond;
    const uint32_t hp = cond.add_watch(WatchDomain::Wram, 0x1FE8, 1, "HP");
    CHECK(cond.set_condition(hp, {true, 3, 4}));
    auto read = [&](WatchDomain, uint32_t) -> std::optional<uint8_t> {
        ++reads;
        return byte;
    };
    cond.sample(read);
    CHECK(reads == 1);
    CHECK(cond.find(hp)->value == 3 && !cond.find(hp)->condition_met);
    const auto at_three = cond.cheat_code(hp);
    CHECK(at_three && *at_three == "7E1FE803");
    byte = 4;
    reads = 0;
    cond.sample(read);
    CHECK(reads == 1);
    CHECK(cond.find(hp)->condition_met && cond.find(hp)->previous == 3 && cond.find(hp)->value == 4);
    cond.sample(read);
    CHECK(!cond.find(hp)->condition_met && cond.find(hp)->previous == 4);
    byte = 5;
    cond.sample(read);
    CHECK(!cond.find(hp)->condition_met);

    // Two watches, one of them two bytes: one sample reads each byte once.
    RamWatch wide;
    wide.add_watch(WatchDomain::Wram, 0, 2, "word");
    wide.add_watch(WatchDomain::Wram, 4, 1, "byte");
    reads = 0;
    wide.sample([&](WatchDomain, uint32_t) -> std::optional<uint8_t> {
        ++reads;
        return uint8_t{0};
    });
    CHECK(reads == 3);
    reads = 0;
    (void)wide.list();
    CHECK(reads == 0);

    const std::string saved = ram.format();
    CHECK(saved.find("frozen") == std::string::npos);
    CHECK(saved.find("say \\\"hi\\\"") != std::string::npos);
    RamWatch loaded;
    loaded.parse(saved);
    const bitforge::Watch* found = loaded.find(second);
    CHECK(found && found->label == "say \"hi\"" && found->condition.enabled && found->condition.from == 3 && found->condition.to == 4);
    CHECK(!found->frozen && !found->sampled);
    CHECK(loaded.add_watch(WatchDomain::Wram, 1, 1, "next") == ram.next_id());

    RamWatch parsed;
    parsed.parse("# comment\r\nnope\r\n2 wram ZZ 1 \"bad\"\r\n4 wram 20 1 \"Gold\" 3 4\r\nnext 10\r\n4 wram 30 1 \"dup\"\r\n");
    CHECK(parsed.list().size() == 1 && parsed.find(4) && parsed.find(4)->label == "Gold" && parsed.find(4)->condition.to == 4);
    CHECK(parsed.add_watch(WatchDomain::Wram, 1, 1, "later") == 10);

    if (at_three) {
        const auto decoded = bitforge::decode_cheat(*at_three);
        CHECK(decoded && decoded->size() == 1 && (*decoded)[0].kind == bitforge::Cheat::Kind::RamWrite);
        CHECK((*decoded)[0].address == 0x7E1FE8 && (*decoded)[0].value == 3);
    } else {
        CHECK(false);
    }

    RamWatch word;
    const uint32_t id = word.add_watch(WatchDomain::Wram, 0x1000, 2, "pair");
    CHECK(word.set_frozen(id, true, 0x0201));
    const auto pair = word.cheat_code(id);
    CHECK(pair && *pair == "7E100001+7E100102");
    const auto parts = bitforge::decode_cheat(*pair);
    CHECK(parts && parts->size() == 2 && (*parts)[0].address == 0x7E1000 && (*parts)[0].value == 0x01);
    CHECK((*parts)[1].address == 0x7E1001 && (*parts)[1].value == 0x02 && (*parts)[1].kind == bitforge::Cheat::Kind::RamWrite);

    RamWatch sram;
    const uint32_t cell = sram.add_watch(WatchDomain::Sram, 4, 1, "battery");
    CHECK(!sram.cheat_code(cell));
    CHECK(!sram.cheat_code(999));
}

auto test_system_frame() -> void {
    const auto rom = test_rom::make_test_rom();
    {
        bitforge::System sys;
        sys.load_test_rom(rom);
        const auto before = sys.save_state();
        const uint32_t id = sys.watches().add_watch(WatchDomain::Wram, 0x1FE8, 1, "HP");
        CHECK(sys.watches().set_condition(id, {true, 3, 4}));
        CHECK(sys.save_state() == before);

        CHECK(sys.write_memory(WatchDomain::Wram, 0x1FE8, 3));
        sys.run_frame();
        CHECK(sys.watches().find(id)->value == 3 && !sys.watches().find(id)->condition_met);
        const auto state = sys.save_state();
        CHECK(sys.write_memory(WatchDomain::Wram, 0x1FE8, 4));
        sys.run_frame();
        CHECK(sys.watches().find(id)->condition_met && sys.watches().find(id)->previous == 3);

        CHECK(sys.load_state(state));
        CHECK(sys.watches().find(id) && !sys.watches().find(id)->condition_met && !sys.watches().find(id)->sampled);
        CHECK(sys.write_memory(WatchDomain::Wram, 0x1FE8, 4));
        sys.run_frame();
        CHECK(!sys.watches().find(id)->condition_met && sys.watches().find(id)->value == 4);
        CHECK(sys.write_memory(WatchDomain::Wram, 0x1FE8, 3));
        sys.run_frame();
        CHECK(!sys.watches().find(id)->condition_met);
        CHECK(sys.write_memory(WatchDomain::Wram, 0x1FE8, 4));
        sys.run_frame();
        CHECK(sys.watches().find(id)->condition_met);
    }
    {
        bitforge::System sys;
        sys.load_test_rom(rom);
        const bitforge::CheatEntry cheat{true, "7E1FE801", "one"};
        sys.set_cheats(std::span<const bitforge::CheatEntry>(&cheat, 1));
        sys.run_frame();
        CHECK(sys.peek_wram(0x1FE8) == 1);
        CHECK(sys.watches().marks().cheat && !sys.watches().marks().freeze);

        const uint32_t id = sys.watches().add_watch(WatchDomain::Wram, 0x1FE8, 1, "HP");
        CHECK(sys.watches().set_frozen(id, true, 3));
        sys.run_frame();
        CHECK(sys.peek_wram(0x1FE8) == 3);
        CHECK(sys.watches().marks().freeze && sys.watches().marks().cheat);
        const auto marked = sys.save_state();
        CHECK(sys.load_state(marked));
        CHECK(sys.watches().marks().cheat && sys.watches().marks().freeze);
        CHECK(sys.watches().find(id)->frozen);

        CHECK(sys.write_memory(WatchDomain::Wram, 0x1FE8, 9));
        sys.run_frame();
        CHECK(sys.peek_wram(0x1FE8) == 3);
        sys.reset();
        CHECK(sys.watches().find(id)->frozen);
        CHECK(sys.write_memory(WatchDomain::Wram, 0x1FE8, 9));
        sys.run_frame();
        CHECK(sys.peek_wram(0x1FE8) == 3);

        CHECK(sys.watches().set_frozen(id, false, 0));
        CHECK(sys.write_memory(WatchDomain::Wram, 0x1FE8, 0x55));
        sys.run_frame();
        CHECK(sys.peek_wram(0x1FE8) == 1);
        CHECK(sys.watches().marks().freeze);

        sys.set_cheats({});
        CHECK(sys.watches().marks().cheat);
    }
    {
        bitforge::System sys;
        sys.load_test_rom(rom);
        CHECK(sys.write_memory(WatchDomain::Wram, 0x1FE8, 7));
        sys.watches().note_hand_edit();
        CHECK(sys.peek_wram(0x1FE8) == 7);
        sys.run_frame();
        CHECK(sys.peek_wram(0x1FE8) == 7);
        const RunMarks marks = sys.watches().marks();
        CHECK(marks.hand_edit && !marks.freeze && !marks.cheat);
    }
    {
        bitforge::System sys;
        sys.load_test_rom(rom, bitforge::memory::MapMode::LoROM, 32);
        const uint32_t id = sys.watches().add_watch(WatchDomain::Sram, 4, 1, "battery");
        CHECK(!sys.watches().cheat_code(id));
        CHECK(sys.watches().set_frozen(id, true, 9));
        sys.run_frame();
        CHECK(sys.sram()[4] == 9 && sys.sram_dirty());
        CHECK(sys.watches().marks().freeze);
        CHECK(sys.watches().set_frozen(id, false, 0));
        CHECK(sys.write_memory(WatchDomain::Sram, 4, 1));
        sys.run_frame();
        CHECK(sys.sram()[4] == 1);
    }
}

auto test_emulator_file() -> void {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "bitforge_ramwatch_tests";
    fs::remove_all(dir);
    fs::create_directories(dir / "cheats");
    fs::create_directories(dir / "watches");
    const fs::path rom_path = dir / "Watch.sfc";
    {
        const auto rom = test_rom::make_test_rom(2);
        std::ofstream out(rom_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(rom.data()), static_cast<std::streamsize>(rom.size()));
    }
    {
        std::ofstream(dir / "cheats" / "Watch.cht") << "on 7E1FE001  marker\n";
        std::ofstream(dir / "watches" / "Watch.wch") << "# test\nnext 8\nbogus\n2 wram 1FE8 1 \"HP\" 3 4\n";
    }

    {
        bitforge::frontend::Emulator emu;
        emu.set_data_dir(dir.string());
        if (!emu.load(rom_path.string())) {
            CHECK(false);
            fs::remove_all(dir);
            return;
        }
        const bitforge::Watch* watch = emu.watches().find(2);
        CHECK(watch && watch->label == "HP" && watch->condition.enabled && watch->condition.from == 3 && !watch->frozen);
        CHECK(emu.run_marks().cheat && !emu.run_marks().freeze);
        CHECK(emu.watches().next_id() == 8);

        CHECK(emu.freeze_watch(2, true, 3));
        for (int i = 0; i < 5; ++i) emu.run_frame();
        CHECK(emu.peek_wram(0x1FE8) == 3);
        CHECK(emu.peek_wram(0x1FE0) == 1);
        CHECK(emu.run_marks().freeze && emu.run_marks().cheat);
        CHECK(emu.edit_watch(2, 9));
        CHECK(emu.peek_wram(0x1FE8) == 9 && emu.run_marks().hand_edit);
        emu.run_frame();
        CHECK(emu.peek_wram(0x1FE8) == 3);

        CHECK(emu.save_watches());
        const auto text = [&] {
            std::ifstream in(dir / "watches" / "Watch.wch");
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }();
        CHECK(text.find("frozen") == std::string::npos);
        CHECK(text.find("next 8") != std::string::npos);
        emu.unload();
    }
    {
        bitforge::frontend::Emulator emu;
        emu.set_data_dir(dir.string());
        CHECK(emu.load(rom_path.string()));
        const bitforge::Watch* watch = emu.watches().find(2);
        CHECK(watch && !watch->frozen && watch->condition.to == 4);
        CHECK(!emu.run_marks().freeze);
        CHECK(emu.watches().remove_watch(2));
        CHECK(emu.watches().add_watch(WatchDomain::Wram, 0x20, 1, "later") == 8);
        CHECK(emu.save_watches());
        emu.unload();
    }
    {
        bitforge::frontend::Emulator emu;
        emu.set_data_dir(dir.string());
        CHECK(emu.load(rom_path.string()));
        CHECK(emu.watches().find(2) == nullptr && emu.watches().find(8) && emu.watches().next_id() == 9);
        emu.unload();
    }
    fs::remove_all(dir);
}

}  // namespace

int main() {
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"search: two passes leave $7E1FE8", test_search_narrows},
        {"search: equal, relative, and a missed value", test_search_compares},
        {"watches: ids, condition, file, cheat code", test_ids_file_and_condition},
        {"system: freeze, cheat, hand edit, state load", test_system_frame},
        {"file: watches survive reload, freezes do not", test_emulator_file},
    };
    std::printf("=== RAM search / memory watch tests ===\n");
    int passed = 0;
    int failed = 0;
    for (const auto& test : tests) {
        const int before = checks_failed;
        std::printf("  %-56s", test.name);
        test.fn();
        if (checks_failed == before) {
            std::printf(" PASS\n");
            ++passed;
        } else {
            std::printf("\n  FAIL\n");
            ++failed;
        }
    }
    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
