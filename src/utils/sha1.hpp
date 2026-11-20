// SPDX-License-Identifier: MIT
#pragma once
// SHA-1 (FIPS 180-4), for identifying a ROM file in a race verification summary. Not for
// security: it only has to match the hashes leaderboards and ROM databases list.
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace bitforge::utils {

auto sha1(std::span<const uint8_t> data) -> std::array<uint8_t, 20>;
// Lowercase hex, 40 characters.
auto sha1_hex(std::span<const uint8_t> data) -> std::string;
// The hash of a file's contents; nullopt if it can't be read.
auto sha1_file(const std::string& path) -> std::optional<std::string>;

}  // namespace bitforge::utils
