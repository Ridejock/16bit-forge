// SPDX-License-Identifier: MIT
// RAM search and memory watches (src/debug): value formats, the search filters and undo,
// freezing, the watches file, and cheat conversion. Headless, no ROM.
#include "debug/memory_watch.hpp"
#include "debug/ram_search.hpp"
#include "snes/cheats.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int checks_failed = 0;

#define CHECK(cond)                                                           \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::printf("\n    check failed: %s (line %d)", #cond, __LINE__);       \
      checks_failed++;                                                        \
    }                                                                         \
  } while (0)

using bitforge::debug::Compare;
using bitforge::debug::Format;
using bitforge::debug::RamSearch;
using bitforge::debug::Region;
using bitforge::debug::Test;
using bitforge::debug::ValueSize;
using bitforge::debug::ValueType;
using bitforge::debug::Watch;

auto test_values() -> void {
  const ValueType hex8{ValueSize::Bits8, Format::Hex};
  const ValueType dec8{};
  const ValueType signed8{ValueSize::Bits8, Format::Signed};
  CHECK(bitforge::debug::format_value(4, hex8) == "$04");
  CHECK(bitforge::debug::format_value(3, dec8) == "3");
  CHECK(bitforge::debug::format_value(0xFF, signed8) == "-1");
  CHECK(bitforge::debug::parse_value("$04", hex8) == uint32_t{4});
  CHECK(bitforge::debug::parse_value("-1", signed8) == uint32_t{0xFF});
  CHECK(!bitforge::debug::parse_value("x", dec8));
  CHECK(bitforge::debug::format_address(Region::Wram, 0x1FE8) == "$7E1FE8");
  CHECK(bitforge::debug::parse_address(Region::Wram, "$7E1FE8", bitforge::debug::kWramBytes) == 0x1FE8u);
  CHECK(bitforge::debug::parse_address(Region::Wram, "1FE8", bitforge::debug::kWramBytes) == 0x1FE8u);

  std::vector<uint8_t> mem{0x01, 0x02, 0x00};
  CHECK(bitforge::debug::read_value(mem, 0, ValueSize::Bits16) == 0x0201u);
  CHECK(bitforge::debug::write_value(mem, 0, ValueSize::Bits16, 0x1234));
  CHECK(mem[0] == 0x34 && mem[1] == 0x12);
  CHECK(!bitforge::debug::write_value(mem, 2, ValueSize::Bits16, 1));
}

auto test_watches_and_cheats() -> void {
  const Watch hp{"HP", Region::Wram, 0x1FE8, {}, false, 0, 1};
  CHECK(bitforge::debug::format_watch_line(hp) == "1 wram 001FE8 8 dec HP");
  const auto parsed = bitforge::debug::parse_watch_file(
      "# comment\n1 wram 001FE8 8 dec HP\nbogus\n2 sram 000010 16 hex Battery\n");
  CHECK(parsed.size() == 2);
  CHECK(parsed[0].label == "HP" && parsed[0].offset == 0x1FE8 && parsed[1].id == 2);
  CHECK(parsed[1].type.size == ValueSize::Bits16 && parsed[1].type.format == Format::Hex);

  const auto code = bitforge::debug::watch_to_cheat(hp, 3);
  CHECK(code && code->code == "7E1FE803");
  const auto decoded = code ? bitforge::decode_cheat(code->code) : std::nullopt;
  CHECK(decoded && decoded->size() == 1 && (*decoded)[0].kind == bitforge::Cheat::Kind::RamWrite &&
        (*decoded)[0].address == 0x7E1FE8 && (*decoded)[0].value == 0x03);
  CHECK(!bitforge::debug::watch_to_cheat(parsed[1], 1));

  std::vector<uint8_t> wram(8, 0);
  std::vector<uint8_t> sram(8, 0);
  Watch frozen = hp;
  frozen.offset = 1;
  frozen.frozen = true;
  frozen.value = 3;
  bitforge::debug::RegionMemory memory{};
  memory[static_cast<size_t>(Region::Wram)] = wram;
  memory[static_cast<size_t>(Region::Sram)] = sram;
  CHECK(bitforge::debug::apply_freezes(std::span<const Watch>(&frozen, 1), memory) ==
        (1u << static_cast<unsigned>(Region::Wram)));
  CHECK(wram[1] == 3);
  CHECK(bitforge::debug::apply_freezes(std::span<const Watch>(&frozen, 1), memory) == 0);

  std::vector<Watch> list = {frozen};
  std::vector<bitforge::debug::WatchReading> readings;
  bitforge::debug::sample_watches(list, memory, {}, readings);
  CHECK(readings.size() == 1 && readings[0].valid && readings[0].before == readings[0].now);
  wram[1] = 4;
  bitforge::debug::sample_watches(list, memory, readings, readings);
  const bitforge::debug::Condition to{list[0].id, Test::ChangedTo, 4};
  CHECK(bitforge::debug::evaluate(to, list, readings));
}

auto test_search() -> void {
  std::vector<uint8_t> mem{1, 5, 5, 9};
  RamSearch search;
  CHECK(!search.filter(mem, Compare::Equal, 5));
  search.reset(mem, {});
  CHECK(search.started() && search.count() == 4 && search.steps() == 0);
  CHECK(search.filter(mem, Compare::Equal, 5));
  CHECK(search.count() == 2 && search.candidates()[0] == 1u && search.candidates()[1] == 2u);
  mem[1] = 4;
  CHECK(search.filter(mem, Compare::Decreased, 1));
  CHECK(search.count() == 1 && search.candidates()[0] == 1u && search.previous(1) == 4u);
  CHECK(search.can_undo());
  search.undo();
  CHECK(search.count() == 2 && search.steps() == 1 && search.previous(1) == 5u);
  std::vector<uint8_t> other{1, 2, 3};
  CHECK(!search.filter(other, Compare::Changed, 0));
}

}  // namespace

int main() {
  std::printf("=== Memory watch tests ===\n");
  int failed = 0;
  auto run = [&](const char* name, void (*fn)()) {
    const int before = checks_failed;
    fn();
    if (checks_failed == before) {
      std::printf("  %s... PASSED\n", name);
    } else {
      std::printf("\n  %s... FAILED\n", name);
      failed++;
    }
  };
  run("Value formats and addresses", test_values);
  run("Watch file, cheats, freeze, condition", test_watches_and_cheats);
  run("RAM search filters and undo", test_search);
  std::printf("\n=== Results: %d passed, %d failed ===\n", 3 - failed, failed);
  return failed == 0 ? 0 : 1;
}
