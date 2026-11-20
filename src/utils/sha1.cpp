// SPDX-License-Identifier: MIT
#include "utils/sha1.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>

namespace bitforge::utils {

namespace {

constexpr size_t kBlock = 64;
constexpr std::array<uint32_t, 5> kInit = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
constexpr std::array<uint32_t, 4> kRound = {0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu, 0xCA62C1D6u};

constexpr auto rotl(uint32_t v, int n) -> uint32_t { return (v << n) | (v >> (32 - n)); }

auto compress(std::array<uint32_t, 5>& h, const uint8_t* block) -> void {
    std::array<uint32_t, 80> w{};
    for (size_t i = 0; i < 16; i++) {
        w[i] = static_cast<uint32_t>(block[i * 4]) << 24 | static_cast<uint32_t>(block[i * 4 + 1]) << 16 |
               static_cast<uint32_t>(block[i * 4 + 2]) << 8 | static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (size_t i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (size_t i = 0; i < 80; i++) {
        uint32_t f = 0;
        if (i < 20) f = (b & c) | (~b & d);
        else if (i < 40 || i >= 60) f = b ^ c ^ d;
        else f = (b & c) | (b & d) | (c & d);
        const uint32_t t = rotl(a, 5) + f + e + kRound[i / 20] + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

}  // namespace

auto sha1(std::span<const uint8_t> data) -> std::array<uint8_t, 20> {
    std::array<uint32_t, 5> h = kInit;
    size_t i = 0;
    for (; i + kBlock <= data.size(); i += kBlock) compress(h, data.data() + i);
    // The tail, a 1 bit, zeros, then the length in bits (big-endian) to fill the last block(s).
    std::array<uint8_t, kBlock * 2> tail{};
    const size_t rest = data.size() - i;
    std::copy(data.begin() + static_cast<std::ptrdiff_t>(i), data.end(), tail.begin());
    tail[rest] = 0x80;
    const size_t blocks = rest + 1 + 8 > kBlock ? 2 : 1;
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    for (size_t b = 0; b < 8; b++) tail[blocks * kBlock - 1 - b] = static_cast<uint8_t>(bits >> (8 * b));
    for (size_t b = 0; b < blocks; b++) compress(h, tail.data() + b * kBlock);
    std::array<uint8_t, 20> out{};
    for (size_t w = 0; w < h.size(); w++) {
        for (size_t b = 0; b < 4; b++) out[w * 4 + b] = static_cast<uint8_t>(h[w] >> (24 - 8 * b));
    }
    return out;
}

auto sha1_hex(std::span<const uint8_t> data) -> std::string {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const uint8_t b : sha1(data)) {
        out += kHex[b >> 4];
        out += kHex[b & 0xF];
    }
    return out;
}

auto sha1_file(const std::string& path) -> std::optional<std::string> {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return sha1_hex(data);
}

}  // namespace bitforge::utils
