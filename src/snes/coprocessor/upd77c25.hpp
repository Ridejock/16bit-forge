// SPDX-License-Identifier: MIT
#pragma once
// NEC uPD77C25 (uPD7725) and uPD96050 signal processors, low-level.
//
// Both run the same 24-bit instruction set; the uPD96050 (ST010/ST011) has bigger memories, a
// wider PC/RP/DP and a deeper stack. Every instruction takes one cycle. The program and data
// ROMs come from user-supplied firmware; nothing is bundled.
//
//   OP  00 pp aaaa s dd hhhh r ssss dddd   ALU op (P select, ALU, A/B, DP/RP modify) + move
//   RT  01 ...same as OP...                OP, then return
//   JP  10 bbbbbbbbb nnnnnnnnnnn kk        conditional jump / call (BRCH, NA, bank)
//   LD  11 iiiiiiiiiiiiiiii ---- dddd      load 16-bit immediate
#include "snes/serialize.hpp"
#include "snes/types.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace bitforge::coprocessor {

struct NecVariant {
  const char* name;
  int pc_bits;
  int rp_bits;
  int dp_bits;
  int stack_depth;
  uint32_t program_words;  // 24-bit
  uint32_t data_rom_words; // 16-bit
  uint32_t data_ram_words; // 16-bit
  uint32_t clock_hz;
};

inline constexpr NecVariant kUpd7725{"uPD77C25", 11, 10, 8, 4, 2048, 1024, 256, 7'600'000};
inline constexpr NecVariant kUpd96050{"uPD96050", 14, 11, 11, 16, 16384, 2048, 2048, 11'000'000};

class UPD77C25 {
public:
  explicit UPD77C25(const NecVariant& variant = kUpd7725);

  auto variant() const -> const NecVariant& { return *variant_; }
  // Firmware size in bytes: program ROM (3 bytes/word) then data ROM (2 bytes/word), little-endian.
  auto firmware_bytes() const -> size_t { return variant_->program_words * 3 + variant_->data_rom_words * 2; }
  auto load_firmware(std::span<const uint8_t> program, std::span<const uint8_t> data) -> void;
  auto set_program_word(uint32_t index, uint32_t word) -> void { program_.at(index) = word & 0xFFFFFF; }
  auto set_data_rom_word(uint32_t index, uint16_t word) -> void { data_rom_.at(index) = word; }

  auto reset() -> void;
  auto exec() -> void;  // one instruction

  // Host (S-CPU) side.
  auto read_sr() const -> Byte { return static_cast<Byte>(status() >> 8); }
  auto read_dr() -> Byte;
  auto write_dr(Byte value) -> void;
  auto read_dp(Word address) const -> Byte;   // uPD96050 data RAM, byte-addressed
  auto write_dp(Word address, Byte value) -> void;
  auto data_ram() -> std::span<uint16_t> { return data_ram_; }

  // Registers, for tests and debugging.
  struct Flags {
    bool ov0 = false, ov1 = false, z = false, c = false, s0 = false, s1 = false;
    auto pack() const -> Byte {
      return static_cast<Byte>(ov0 | ov1 << 1 | z << 2 | c << 3 | s0 << 4 | s1 << 5);
    }
    auto serialize(Archive& ar) -> void { ar(ov0, ov1, z, c, s0, s1); }
  };
  struct Status {
    bool p0 = false, p1 = false, ei = false, sic = false, soc = false, drc = false;
    bool dma = false, drs = false, usf0 = false, usf1 = false, rqm = false;
    auto serialize(Archive& ar) -> void { ar(p0, p1, ei, sic, soc, drc, dma, drs, usf0, usf1, rqm); }
  };
  struct Regs {
    std::array<uint16_t, 16> stack{};
    uint16_t pc = 0, rp = 0, dp = 0;
    uint8_t sp = 0;
    uint16_t si = 0, so = 0;
    int16_t k = 0, l = 0, m = 0, n = 0, a = 0, b = 0;
    uint16_t tr = 0, trb = 0, dr = 0;
    Status sr;
    Flags fa, fb;
    auto serialize(Archive& ar) -> void {
      ar(stack, pc, rp, dp, sp, si, so, k, l, m, n, a, b, tr, trb, dr, sr, fa, fb);
    }
  };
  auto regs() const -> const Regs& { return r_; }
  auto regs() -> Regs& { return r_; }
  auto status(bool raw = false) const -> uint16_t;
  auto set_status(uint16_t value) -> void;

  auto serialize(Archive& ar) -> void;

private:
  const NecVariant* variant_;
  uint16_t pc_mask_, rp_mask_, dp_mask_;
  uint8_t sp_mask_;
  std::vector<uint32_t> program_;
  std::vector<uint16_t> data_rom_;
  std::vector<uint16_t> data_ram_;
  Regs r_;

  auto exec_op(uint32_t opcode) -> void;
  auto bus_source(unsigned src) -> uint16_t;
  auto alu(unsigned op, uint16_t acc, uint16_t p, bool other_carry, Flags& f) -> uint16_t;
  auto exec_jp(uint32_t opcode) -> void;
  auto exec_ld(uint16_t id, unsigned dst) -> void;
  auto ram(uint32_t index) -> uint16_t& { return data_ram_[index % data_ram_.size()]; }
};

} // namespace bitforge::coprocessor
