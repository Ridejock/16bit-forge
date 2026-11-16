// SPDX-License-Identifier: MIT
#pragma once
// Deterministic S-DSP stress script shared by apu_tests (golden hash) and the reference
// comparison harness: random ARAM (BRR blocks with every shift/filter/end/loop combination), the
// echo unit pushed to its limits (EDL 15, FIR and EFB near +/-127, EDL changes mid-buffer, FLG
// echo-write toggles), KON/KOFF, pitch modulation, noise, and ENVX/OUTX/ENDX polled like a game.
// The hash covers every output sample, every polled register and the final ARAM (echo writes).
#include <cstdint>

namespace dsp_stress {

struct Lcg {  // tiny portable PRNG so the script is identical on every compiler
    uint32_t s;
    auto next() -> uint32_t { s = s * 1664525u + 1013904223u; return s >> 8; }
};

// Ram: 64 KB byte array. Write(addr, value), Read(addr) -> int, Run() -> runs one sample and
// returns (left << 16) | (right & 0xFFFF).
template <class Ram, class Write, class Read, class Run>
auto run(uint32_t seed, long samples, Ram& ram, Write write, Read read, Run run_one) -> uint64_t {
    Lcg rng{seed};
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](uint32_t v) { h = (h ^ v) * 1099511628211ull; };
    for (int i = 0; i < 0x10000; i++) ram[i] = static_cast<uint8_t>(rng.next());
    const auto near_limit = [&rng]() { const int v = static_cast<int>(rng.next() % 16); return (rng.next() & 1) ? 127 - v : -128 + v; };
    const auto w = [&](int a, int v) { write(a & 0x7F, v & 0xFF); };
    w(0x6C, 0x00); w(0x0C, 127); w(0x1C, -128); w(0x2C, near_limit()); w(0x3C, near_limit());
    w(0x5D, static_cast<int>(rng.next())); w(0x6D, static_cast<int>(rng.next())); w(0x7D, 15); w(0x0D, near_limit()); w(0x4D, 0xFF);
    for (int i = 0; i < 8; i++) w(i * 0x10 + 0x0F, near_limit());
    for (int v = 0; v < 8; v++) {
        w(v * 16 + 0, near_limit()); w(v * 16 + 1, near_limit());
        w(v * 16 + 2, static_cast<int>(rng.next())); w(v * 16 + 3, static_cast<int>(rng.next() & 0x3F));
        w(v * 16 + 4, static_cast<int>(rng.next())); w(v * 16 + 5, static_cast<int>(0x80 | (rng.next() & 0x7F)));
        w(v * 16 + 6, static_cast<int>(rng.next())); w(v * 16 + 7, static_cast<int>(rng.next()));
    }
    w(0x4C, 0xFF);
    for (long n = 0; n < samples; n++) {
        const int r = static_cast<int>(rng.next() % 1000);
        if (r < 6) w(static_cast<int>(rng.next()), static_cast<int>(rng.next()));
        else if (r < 9) w(0x4C, static_cast<int>(rng.next()));                      // KON
        else if (r < 11) w(0x5C, static_cast<int>(rng.next() & rng.next()));        // KOFF
        else if (r < 12) w(0x7D, static_cast<int>(rng.next() & 0x0F));              // EDL mid-buffer
        else if (r < 13) w(0x6C, static_cast<int>(rng.next() & 0x3F));              // FLG, unmuted
        else if (r < 14) w(0x0D, near_limit());                                     // EFB
        else if (r < 15) w(static_cast<int>((rng.next() & 7) * 0x10 + 0x0F), near_limit());  // FIR
        else if (r < 16) { w(0x2D, static_cast<int>(rng.next())); w(0x3D, static_cast<int>(rng.next() & rng.next())); }
        else if (r < 17) w(0x6D, static_cast<int>(rng.next()));                     // ESA
        if (n % 7 == 0) {
            for (int a = 0; a < 128; a++) mix(static_cast<uint32_t>(read(a)));
        }
        mix(static_cast<uint32_t>(run_one()));
    }
    for (int i = 0; i < 0x10000; i++) mix(ram[i]);
    return h;
}

}  // namespace dsp_stress
