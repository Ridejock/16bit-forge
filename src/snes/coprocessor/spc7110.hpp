// SPDX-License-Identifier: MIT
#pragma once
// SPC7110 (Hudson: Tengai Makyou Zero / Far East of Eden Zero, Momotarou Dentetsu Happy, Super
// Power League 4). A HiROM mapper with a separate data ROM, a graphics decompressor, a data
// port, a multiply/divide unit, and, on the Tengai Makyou Zero board, an RTC-4513.
//
// The ROM file is the 1 MB program ROM followed by the data ROM.
//   $00-$3F|$80-$BF:8000-FFFF, $C0-$FF:0000-FFFF  four 1 MB windows ($x0/$x1/$x2/$x3 banks):
//          the first is program ROM, the others the data ROM MB chosen by $4831/$4832/$4833
//   $40-$4F:0000-FFFF   program ROM
//   $00-$3F|$80-$BF:6000-7FFF  SRAM, enabled by $4830 bit 7 (reads 0 when disabled)
//   $4800-$480C  decompressor: $4801-$4803 table base, $4804 index (fetches the table entry:
//                mode, 24-bit stream address), $4806 start, $4800 data
//                ($50:xxxx mirrors $4800), $4805/$4806 initial skip, $4807 row skip, $480B mode,
//                $480C status
//   $4810-$481A  data port: offset $4811-$4813, adjust $4814-$4815, stride $4816-$4817,
//                control $4818, data $4810 ($481A advances)
//   $4820-$482F  ALU: 16x16 multiply ($4825 starts), 32/16 divide ($4827 starts), $482E signed
//   $4830-$4834  SRAM enable, data ROM banks, data ROM size
//   $4840-$4842  RTC-4513 (when present)
// Register behaviour is from fullsnes ("SNES Cart SPC7110") and the hardware analysis by Dark
// Force, neviksti, Andreas Naive and others it documents. Operations finish instantly.
#include "snes/coprocessor/chip_log.hpp"
#include "snes/coprocessor/coprocessor.hpp"
#include "snes/coprocessor/rtc4513.hpp"

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace snes::coprocessor {

// The graphics decompressor: an adaptive binary arithmetic decoder with per-context
// probability states and a move-to-front pixel predictor, for 1, 2 and 4 bits per pixel
// (modes 0, 1, 2). Implemented from fullsnes ("SNES Cart SPC7110 Decompression Algorithm").
// Each decode_row() call produces one tile row: 8 pixels, returned as bit planes.
class SPC7110Decompressor {
public:
  using Reader = std::function<Byte(uint32_t)>;  // data ROM byte
  explicit SPC7110Decompressor(Reader reader) : read_(std::move(reader)) {}

  auto initialize(unsigned mode, uint32_t origin) -> void;
  auto decode() -> void;  // eight more pixels into result()
  auto bpp() const -> unsigned { return 1u << mode_; }
  auto result() const -> uint32_t;  // bit planes: byte n = plane n's row (mode 0: older rows above)
  auto serialize(Archive& ar) -> void;

  // Hardware tables (fullsnes: EvolutionProb / EvolutionNextMps / EvolutionNextLps /
  // Mode2ContextTable), shared with the test encoder.
  static const std::array<Byte, 53> kProbability;
  static const std::array<Byte, 53> kNextMps;
  static const std::array<Byte, 53> kNextLps;
  static const std::array<Byte, 15> kMode2Context;
  static constexpr unsigned kContexts = 31;
  // Neighbourhood class of the pixels a (left), b (above) and c (above-left): 0 all equal,
  // 1 a=b, 2 b=c, 3 a=c, 4 all different.
  static auto neighbour_class(unsigned a, unsigned b, unsigned c) -> unsigned;

private:
  Reader read_;
  unsigned mode_ = 0;
  // Per context: probability state and whether 0/1 are swapped.
  std::array<Byte, kContexts> state_{};
  std::array<Byte, kContexts> invert_{};
  // Arithmetic decoder: interval top, 16-bit code window, bits left in its low byte, source.
  unsigned top_ = 255;
  uint32_t window_ = 0;
  unsigned window_bits_ = 8;
  uint32_t source_ = 0;
  // Pixel predictor: recently used colours first, and the decoded pixels (newest lowest).
  std::array<Byte, 16> order_{};
  uint32_t out_ = 0;
  unsigned c_ = 0;  // the previous row-above pixel

  auto decode_bit(unsigned context, unsigned& decoded) -> void;
  auto place_pixel(unsigned a, unsigned b, unsigned decoded) -> void;
};

class SPC7110 final : public Coprocessor {
public:
  SPC7110(const std::vector<Byte>& rom, std::vector<Byte>& ram, bool rtc);

  auto chip() const -> Chip override { return Chip::SPC7110; }
  auto reset() -> void override;
  auto owns_map() const -> bool override { return true; }
  auto read(Address address) -> std::optional<Byte> override;
  auto write(Address address, Byte value) -> bool override;
  auto step(uint32_t master_clocks) -> void override;
  auto serialize(Archive& ar) -> void override;

  auto data_rom_read(uint32_t address) const -> Byte;
  auto rtc() -> RTC4513* { return rtc_.get(); }
  auto rtc_save(int64_t now) -> std::vector<Byte> override {
    if (!rtc_) return {};
    const auto data = rtc_->save_file(now);
    return {data.begin(), data.end()};
  }
  auto rtc_load(std::span<const Byte> data, int64_t now) -> bool override { return rtc_ && rtc_->load_file(data, now); }

  static constexpr uint32_t kProgramRomSize = 0x100000;

private:
  const std::vector<Byte>& rom_;
  std::vector<Byte>& ram_;
  std::unique_ptr<RTC4513> rtc_;
  std::array<Byte, 0x40> r_{};  // $4800-$483F
  // Decompression unit.
  Byte dcu_mode_ = 0;
  uint32_t dcu_address_ = 0;
  uint32_t dcu_offset_ = 0;
  std::array<Byte, 32> dcu_tile_{};
  SPC7110Decompressor decompressor_;
  LogLimiter<96> log_;

  auto ram_index(Address address) -> std::optional<uint32_t>;
  auto log_check_area(const char* what) const -> void;
  auto& reg(unsigned address) { return r_[address & 0x3F]; }
  auto io_read(unsigned address) -> std::optional<Byte>;
  auto io_write(unsigned address, Byte value) -> void;
  auto mcu_rom_read(uint32_t linear) const -> Byte;  // linear = (bank & $3F) << 16 | offset

  auto dcu_load_address() -> void;
  auto dcu_begin_transfer() -> void;
  auto dcu_read() -> Byte;

  auto data_offset() const -> uint32_t;
  auto data_adjust() const -> uint32_t;
  auto data_stride() const -> uint32_t;
  auto set_data_offset(uint32_t value) -> void;
  auto set_data_adjust(uint32_t value) -> void;
  auto data_port_read() -> void;
  auto data_port_increment_4810() -> void;
  auto data_port_increment(unsigned trigger) -> void;  // $4814 = 1, $4815 = 2, $481A = 3

  auto alu_multiply() -> void;
  auto alu_divide() -> void;
};

} // namespace snes::coprocessor
