// SPDX-License-Identifier: MIT
#pragma once
// Memory search and watches. Not part of a save state: a watch list is per game, and a freeze
// lasts only until that game is closed.
//
// Search is byte-wide. Start captures a region (WRAM or cartridge SRAM). Each narrow keeps the
// addresses that match, so a second pass can leave a single byte. Relative compares (changed,
// increased, ...) use the previous capture; the others compare against a typed value.
//
// Every watch has an id that is never reused. The running machine reads each watch once per
// frame, at the start of the frame after cheat and freeze writes. A condition such as "changed
// from 3 to 4" is true only on the sample where the previous value was `from` and the new one
// is `to`. Auto-splits read that flag. It does not stay set on the following frames.
//
// A freeze rewrites its bytes every frame and is not saved. A hand edit is a one-shot poke.
// Turning a watch into a cheat uses a Pro Action Replay WRAM code; SRAM has no such code.
// RunMarks records that this loaded game used a freeze, a cheat, or a hand edit. Closing the
// game drops the marks with the machine. Loading a state does not.
#include "snes/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bitforge {

enum class WatchDomain : uint8_t { Wram, Sram };

enum class SearchCompare : uint8_t {
    Equal,
    NotEqual,
    Less,
    Greater,
    Changed,
    Unchanged,
    Increased,
    Decreased,
};

inline auto compare_needs_value(SearchCompare compare) -> bool {
    return compare == SearchCompare::Equal || compare == SearchCompare::NotEqual || compare == SearchCompare::Less ||
           compare == SearchCompare::Greater;
}

// "$7E1FE8" for WRAM (bank $7E, or $7F past 64KB). "SRAM:$10" for cartridge RAM.
auto format_address(WatchDomain domain, uint32_t offset) -> std::string;
// Bus address of a WRAM offset: $7E0000 + offset, so offset $10000 is $7F0000.
auto wram_bus_address(uint32_t offset) -> Address;

struct WatchCondition {
    bool enabled = false;
    uint32_t from = 0;
    uint32_t to = 0;
};

struct Watch {
    uint32_t id = 0;
    WatchDomain domain = WatchDomain::Wram;
    uint32_t offset = 0;
    uint8_t size = 1;  // 1, 2, or 4 bytes, little-endian
    std::string label;
    WatchCondition condition;
    bool frozen = false;  // session only; format() leaves this out
    uint32_t freeze_value = 0;
    uint32_t value = 0;      // last sample
    uint32_t previous = 0;   // the sample before that
    bool sampled = false;
    bool condition_met = false;  // true only on the sample that saw from -> to
};

struct RunMarks {
    bool freeze = false;
    bool cheat = false;
    bool hand_edit = false;
};

struct FreezeByte {
    WatchDomain domain = WatchDomain::Wram;
    uint32_t offset = 0;
    uint8_t value = 0;
};

class RamWatch {
public:
    static constexpr uint32_t kWramBytes = 128 * 1024;

    auto start_search(WatchDomain domain, std::span<const uint8_t> bytes) -> void;
    // False when no search is running, the region size changed, or a value compare has no value.
    // A false return leaves the candidate list alone.
    auto narrow(std::span<const uint8_t> bytes, SearchCompare compare, std::optional<uint8_t> value) -> bool;
    auto searching() const -> bool { return searching_; }
    auto search_domain() const -> WatchDomain { return search_domain_; }
    auto candidate_count() const -> uint32_t { return static_cast<uint32_t>(candidates_.size()); }
    auto first_candidates(uint32_t limit) const -> std::vector<uint32_t>;

    // 0 when the address does not fit (WRAM) or the id space is exhausted. Size becomes 1, 2, or 4.
    auto add_watch(WatchDomain domain, uint32_t offset, uint8_t size, std::string label) -> uint32_t;
    auto remove_watch(uint32_t id) -> bool;
    auto find(uint32_t id) -> Watch* ;
    auto find(uint32_t id) const -> const Watch*;
    auto list() const -> std::span<const Watch> { return watches_; }
    auto next_id() const -> uint32_t { return next_id_; }

    auto set_label(uint32_t id, std::string label) -> bool;
    auto set_size(uint32_t id, uint8_t size) -> bool;
    auto set_condition(uint32_t id, WatchCondition condition) -> bool;
    auto set_frozen(uint32_t id, bool frozen, uint32_t value) -> bool;

    // One pass over the list. `read` returns the byte at that offset, or nullopt if it is not
    // there (an SRAM watch past the end). A watch that cannot be read keeps its last value.
    template <typename Read>
    auto sample(Read&& read) -> void {
        for (Watch& watch : watches_) {
            uint32_t now = 0;
            bool ok = true;
            for (uint8_t i = 0; i < watch.size; ++i) {
                const std::optional<uint8_t> byte = read(watch.domain, watch.offset + i);
                if (!byte) {
                    ok = false;
                    break;
                }
                now |= static_cast<uint32_t>(*byte) << (8 * i);
            }
            if (!ok) {
                watch.condition_met = false;
                continue;
            }
            watch.condition_met = watch.sampled && watch.condition.enabled && watch.value == watch.condition.from && now == watch.condition.to;
            if (watch.sampled) watch.previous = watch.value;
            watch.value = now;
            watch.sampled = true;
        }
    }

    // Drops the last sample so a condition cannot fire across a state load. The watch stays.
    auto forget_samples() -> void;
    auto freeze_bytes() const -> std::vector<FreezeByte>;

    auto note_freeze_applied() -> void { marks_.freeze = true; }
    auto note_cheat() -> void { marks_.cheat = true; }
    auto note_hand_edit() -> void { marks_.hand_edit = true; }
    auto marks() const -> RunMarks { return marks_; }

    // Pro Action Replay bytes (AAAAAAVV, joined with '+') for the frozen value, or the last
    // sample when it is not frozen. nullopt for SRAM or an unknown id.
    auto cheat_code(uint32_t id) const -> std::optional<std::string>;

    // Watches only: id, domain, offset, size, label, condition, and the next id. Not freezes,
    // samples, search results, or run marks.
    auto format() const -> std::string;
    auto parse(std::string_view text) -> void;

private:
    uint32_t next_id_ = 1;
    std::vector<Watch> watches_;
    RunMarks marks_{};
    bool searching_ = false;
    WatchDomain search_domain_ = WatchDomain::Wram;
    std::vector<uint8_t> snapshot_;
    std::vector<uint32_t> candidates_;
};

}  // namespace bitforge
