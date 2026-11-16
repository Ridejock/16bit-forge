// SPDX-License-Identifier: MIT
#pragma once
// S-DD1 (Star Ocean, Street Fighter Alpha 2): a LoROM mapper with a 4 x 1 MB bank switcher for
// $C0-$FF and a streaming decompressor that sits on DMA reads from those banks.
//
//   $4800  DMA channels with decompression enabled (bit n = channel n)
//   $4801  channels armed for their next transfer; a channel's bit clears when it finishes
//   $4802-$4803, $4808-$480F  not decoded: read the ROM byte at that offset
//   $4804-$4807  1 MB ROM bank for $C0-$CF, $D0-$DF, $E0-$EF, $F0-$FF (bits 0-3). Bit 7 of
//          $4805/$4807 also maps $20-$3F/$A0-$BF:8000-FFFF onto the first 2 MB.
// Save RAM (Star Ocean's SHVC-LN3B board): $00-$3F|$80-$BF:6000-7FFF in
// 8 KB pages and $70-$73:0000-FFFF. Star Ocean's sound driver keeps variables there.
//
// The chip snoops the DMA address and size registers ($43n2-$43n6). A read of an armed
// channel's start address returns the next decompressed byte instead of ROM (the transfer uses
// a fixed A-bus address).
//
// Decompression is implemented from the algorithm description in Martin Korth's fullsnes
// ("SNES Cart S-DD1 Decompression Algorithm"), which documents Andreas Naive's and neviksti's
// reverse engineering of the chip: a per-bitplane context model, a 33-state adaptive
// probability estimator, and eight run-length bit counters (one per code size) that pull
// variable-length codewords from a single input bit stream.
#include "snes/coprocessor/chip_log.hpp"
#include "snes/coprocessor/coprocessor.hpp"

#include <array>
#include <functional>
#include <vector>

namespace snes::coprocessor {

class SDD1Decompressor {
public:
  using Reader = std::function<Byte(uint32_t)>;  // 24-bit address in $C0-$FF (MMC-mapped)
  explicit SDD1Decompressor(Reader reader) : read_(std::move(reader)) {}

  auto init(uint32_t address) -> void;
  auto read() -> Byte;
  auto serialize(Archive& ar) -> void;

  // Hardware tables (fullsnes: EvolutionCodeSize / EvolutionMpsNext / EvolutionLpsNext /
  // RunTable). Public so the test encoder can model the same estimator.
  static const std::array<Byte, 33> kCodeSize;
  static const std::array<Byte, 33> kMpsNext;
  static const std::array<Byte, 33> kLpsNext;
  // Counter value loaded for a "1" codeword: the low (7 - code size) index bits are ones.
  // The counter counts down; reaching 0 yields the less probable symbol.
  static const std::array<Byte, 128> kRunTable;

private:
  Reader read_;
  // Input: absolute bit address of the next unread bit (byte = >> 3, MSB first).
  uint32_t bit_address_ = 0;
  // Header: number of bitplanes (2, 4, 8, or 0 for the 8-bits-per-byte mode) and the masks
  // that select context bits from a plane's history.
  Byte planes_ = 2;
  uint16_t high_context_mask_ = 0;
  uint16_t low_context_mask_ = 0;
  // Per code size: run counter (0/0x80: needs a codeword, bit 7 set: MPS-only run).
  std::array<Byte, 8> counter_{};
  // Per context: estimator state and the currently more probable symbol.
  std::array<Byte, 32> state_{};
  std::array<Byte, 32> mps_{};
  // Per bitplane: the most recent decoded bits (newest in bit 0).
  std::array<uint16_t, 8> history_{};
  // Output walk: current plane (odd = its byte is decoded and waiting) and row within a tile.
  Byte plane_ = 0;
  Byte row_ = 0;

  auto input_bit() -> unsigned;
  auto load_counter(Byte code_size) -> Byte;
  auto decode_symbol(Byte context) -> unsigned;
  auto decode_plane_bit(Byte plane) -> unsigned;
};

class SDD1 final : public Coprocessor {
public:
  SDD1(const std::vector<Byte>& rom, std::vector<Byte>& ram);

  auto chip() const -> Chip override { return Chip::SDD1; }
  auto reset() -> void override;
  auto read(Address address) -> std::optional<Byte> override;
  auto write(Address address, Byte value) -> bool override;
  auto dma_register_write(Byte channel, Byte reg, Byte value) -> void override;
  auto step(uint32_t) -> void override {}
  auto serialize(Archive& ar) -> void override;

  auto mmc_read(uint32_t address) const -> Byte;  // $C0-$FF through $4804-$4807

private:
  const std::vector<Byte>& rom_;
  std::vector<Byte>& ram_;
  std::array<Byte, 8> regs_{};  // $4800-$4807
  struct Dma { uint32_t address = 0; uint16_t size = 0; auto serialize(Archive& ar) -> void { ar(address, size); } };
  std::array<Dma, 8> dma_{};
  bool dma_ready_ = false;
  SDD1Decompressor decompressor_;
  LogLimiter<32> log_;

  auto ram_offset(Address address) -> std::optional<size_t>;

  auto rom_at(uint32_t offset) const -> Byte { return rom_.empty() ? Byte{0} : rom_[offset % rom_.size()]; }
};

} // namespace snes::coprocessor
