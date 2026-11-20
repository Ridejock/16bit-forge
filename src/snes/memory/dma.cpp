// SPDX-License-Identifier: MIT
// General-purpose DMA ($420B) and HDMA ($420C) for the Bus. Transfer modes are a table of
// B-bus offsets; DMA time is added to the bus clock so it is stolen from the CPU.
#include "snes/memory/bus.hpp"
#include "snes/ppu/ppu.hpp"
#include "utils/logging.hpp"
#include <algorithm>
#include <cstdint>
#include <array>

namespace bitforge::memory {

namespace {

// Transfer mode (DMAP bits 2-0): how many bytes make one unit and which $21xx offset each goes to.
struct TransferMode {
    uint8_t length;
    std::array<uint8_t, 4> offsets;
};
constexpr std::array<TransferMode, 8> kTransferModes = {{
    {1, {0, 0, 0, 0}},  // 0: one register
    {2, {0, 1, 0, 0}},  // 1: two registers (VRAM L/H)
    {2, {0, 0, 0, 0}},  // 2: one register, written twice (OAM, CGRAM)
    {4, {0, 0, 1, 1}},  // 3: two registers, each twice (scroll pairs, windows)
    {4, {0, 1, 2, 3}},  // 4: four registers
    {4, {0, 1, 0, 1}},  // 5: two registers, alternating
    {2, {0, 0, 0, 0}},  // 6: same as 2
    {4, {0, 0, 1, 1}},  // 7: same as 3
}};

// Master clocks. GP-DMA: 8 to start, 8 per channel, 8 per byte. HDMA: 18 at the table reload
// and per line with any channel active, 8 per channel, 16 more per indirect address load, 8 per
// byte.
constexpr uint64_t kDmaStartClocks = 8;
constexpr uint64_t kDmaChannelClocks = 8;
constexpr uint64_t kDmaByteClocks = 8;
constexpr uint64_t kHdmaOverheadClocks = 18;
constexpr uint64_t kHdmaIndirectClocks = 16;

constexpr Byte kDirectionBtoA = 0x80;
constexpr Byte kHdmaIndirect = 0x40;
constexpr Byte kStepDecrement = 0x10;
constexpr Byte kStepFixed = 0x08;

constexpr auto long_addr(Byte bank, Word offset) -> Address {
    return (static_cast<Address>(bank) << 16) | offset;
}

} // namespace

auto Bus::write_dma_register(Byte channel, Byte reg, Byte value) -> void {
    auto& ch = dma_channels[channel & 7];
    switch (reg) {
        case 0x0: ch.dmap = value; break;
        case 0x1: ch.bbad = value; break;
        case 0x2: ch.a1t = static_cast<Word>((ch.a1t & 0xFF00) | value); break;
        case 0x3: ch.a1t = static_cast<Word>((ch.a1t & 0x00FF) | (value << 8)); break;
        case 0x4: ch.a1b = value; break;
        case 0x5: ch.das = static_cast<Word>((ch.das & 0xFF00) | value); break;
        case 0x6: ch.das = static_cast<Word>((ch.das & 0x00FF) | (value << 8)); break;
        case 0x7: ch.dasb = value; break;
        case 0x8: ch.a2a = static_cast<Word>((ch.a2a & 0xFF00) | value); break;
        case 0x9: ch.a2a = static_cast<Word>((ch.a2a & 0x00FF) | (value << 8)); break;
        case 0xA: ch.ntrl = value; break;
        case 0xB: case 0xF: ch.unused = value; break;
        default: break;
    }
}

auto Bus::read_dma_register(Byte channel, Byte reg) const -> Byte {
    const auto& ch = dma_channels[channel & 7];
    switch (reg) {
        case 0x0: return ch.dmap;
        case 0x1: return ch.bbad;
        case 0x2: return static_cast<Byte>(ch.a1t);
        case 0x3: return static_cast<Byte>(ch.a1t >> 8);
        case 0x4: return ch.a1b;
        case 0x5: return static_cast<Byte>(ch.das);
        case 0x6: return static_cast<Byte>(ch.das >> 8);
        case 0x7: return ch.dasb;
        case 0x8: return static_cast<Byte>(ch.a2a);
        case 0x9: return static_cast<Byte>(ch.a2a >> 8);
        case 0xA: return ch.ntrl;
        case 0xB: case 0xF: return ch.unused;
        default: return open_bus;
    }
}

// Moves one byte between the A-bus address and B-bus register $21(BBAD + mode offset).
auto Bus::dma_transfer_unit(DMAChannel& ch, Address a_addr, int index) -> void {
    const auto& mode = kTransferModes[ch.dmap & 0x07];
    const Address b_addr = 0x2100 | ((ch.bbad + mode.offsets[static_cast<size_t>(index % mode.length)]) & 0xFF);
    if (ch.dmap & kDirectionBtoA) {
        write(a_addr, read(b_addr));
    } else {
        write(b_addr, read(a_addr));
    }
    pending_clocks += kDmaByteClocks;
    mar = a_addr;  // the A-bus side is what a coprocessor sharing the cartridge bus contends with
}

auto Bus::trigger_dma(Byte mdmaen_value) -> void {
    if (mdmaen_value == 0) return;
    dma_active = true;
    // HDMA cuts into a running GP-DMA, and its table reload (line 0) or line transfer stops the
    // GP-DMA of every channel it uses. The transfer below runs in one go, so work out where
    // those points fall.
    const uint64_t start = pending_clocks;
    const ppu::PPU::HdmaTiming hdma = (hdmaen && ppu) ? ppu->hdma_timing(pending_clocks)
                                                      : ppu::PPU::HdmaTiming{UINT64_MAX, UINT64_MAX};
    pending_clocks += kDmaStartClocks;
    for (int i = 0; i < 8; i++) {
        if (!(mdmaen_value & (1 << i))) continue;
        auto& ch = dma_channels[static_cast<size_t>(i)];
        uint64_t stop_at = UINT64_MAX;
        if (hdmaen & (1 << i)) stop_at = start + std::min(hdma.init, ch.hdma_terminated ? UINT64_MAX : hdma.run);
        DMA_TRACE("DMA ch%d: DMAP %02X  $21%02X %s $%02X:%04X  %u bytes", i, ch.dmap, ch.bbad,
                  (ch.dmap & kDirectionBtoA) ? "->" : "<-", ch.a1b, ch.a1t, ch.das ? ch.das : 0x10000u);
        pending_clocks += kDmaChannelClocks;
        const int step = (ch.dmap & kStepFixed) ? 0 : (ch.dmap & kStepDecrement) ? -1 : 1;
        // DAS counts down to 0 (0 means 65536) and A1T is left pointing past the block.
        int index = 0;
        do {
            if (pending_clocks >= stop_at) break;  // HDMA took the channel over
            dma_transfer_unit(ch, long_addr(ch.a1b, ch.a1t), index++);
            ch.a1t = static_cast<Word>(ch.a1t + step);
            ch.das = static_cast<Word>(ch.das - 1);
        } while (ch.das != 0);
    }
    dma_active = false;
}

// Reads the next table entry: line count (0 ends the channel for this frame), then for
// indirect HDMA the 16-bit data address.
auto Bus::hdma_reload(DMAChannel& ch) -> void {
    ch.ntrl = read(long_addr(ch.a1b, ch.a2a));
    ch.a2a = static_cast<Word>(ch.a2a + 1);
    if (ch.ntrl == 0) {
        ch.hdma_terminated = true;
        return;
    }
    if (ch.dmap & kHdmaIndirect) {
        const Byte low = read(long_addr(ch.a1b, ch.a2a));
        const Byte high = read(long_addr(ch.a1b, static_cast<Word>(ch.a2a + 1)));
        ch.das = static_cast<Word>(low | (high << 8));
        ch.a2a = static_cast<Word>(ch.a2a + 2);
        pending_clocks += kHdmaIndirectClocks;
    }
    ch.hdma_do_transfer = true;
}

auto Bus::hdma_init() -> void {
    dma_active = true;
    if (hdmaen) pending_clocks += kHdmaOverheadClocks;
    for (size_t i = 0; i < dma_channels.size(); i++) {
        auto& ch = dma_channels[i];
        ch.hdma_terminated = !(hdmaen & (1 << i));
        if (ch.hdma_terminated) continue;
        pending_clocks += kDmaChannelClocks;
        ch.a2a = ch.a1t;
        hdma_reload(ch);
    }
    dma_active = false;
}

auto Bus::hdma_run_line() -> void {
    bool any = false;
    dma_active = true;
    for (size_t i = 0; i < dma_channels.size(); i++) {
        auto& ch = dma_channels[i];
        if (!(hdmaen & (1 << i)) || ch.hdma_terminated) continue;
        any = true;
        pending_clocks += kDmaChannelClocks;

        if (ch.hdma_do_transfer) {
            const int length = kTransferModes[ch.dmap & 0x07].length;
            for (int b = 0; b < length; b++) {
                Address source;
                if (ch.dmap & kHdmaIndirect) {
                    source = long_addr(ch.dasb, ch.das);
                    ch.das = static_cast<Word>(ch.das + 1);
                } else {
                    source = long_addr(ch.a1b, ch.a2a);
                    ch.a2a = static_cast<Word>(ch.a2a + 1);
                }
                dma_transfer_unit(ch, source, b);
            }
        }

        // Bits 6-0 count lines down; bit 7 = transfer on every line instead of only the first.
        ch.ntrl = static_cast<Byte>(ch.ntrl - 1);
        ch.hdma_do_transfer = (ch.ntrl & 0x80) != 0;
        if ((ch.ntrl & 0x7F) == 0) hdma_reload(ch);
    }
    if (any) pending_clocks += kHdmaOverheadClocks;
    dma_active = false;
}

} // namespace bitforge::memory
