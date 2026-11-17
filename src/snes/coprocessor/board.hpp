// SPDX-License-Identifier: MIT
#pragma once
// Cartridge board detection from the internal header, and the coprocessor factory.
//
// The header block sits at $xxB0-$xxFF of the bank holding the vectors. The fields used:
//   $FFD5 map mode       low nibble: 0 LoROM, 1 HiROM, 2 LoROM (S-DD1), 3 SA-1, 5 ExHiROM, A SPC7110
//   $FFD6 chipset        low nibble >= 3 means a coprocessor; the high nibble says which:
//                        0 DSP (NEC), 1 Super FX, 2 OBC1, 3 SA-1, 4 S-DD1, 5 S-RTC, E other,
//                        F custom, told apart by $FFBF. Low nibble 2/5/6/9/A = battery RAM.
//   $FFD8 RAM size       1 KB << n
//   $FFDA = $33          the extended header is present:
//   $FFBD expansion RAM  1 KB << n (Super FX work RAM)
//   $FFBF chip subtype   for chipset $Fx: 0 SPC7110, 1 ST010/ST011, 2 ST018, $10 Cx4
//                        (SPC7110 chipset $F5; $F9 adds the RTC-4513)
// Which DSP firmware a DSP cartridge needs isn't in the header; a short title table decides.
#include "snes/coprocessor/coprocessor.hpp"

#include <memory>
#include <string>
#include <vector>

namespace bitforge::coprocessor {

struct BoardInfo {
  Chip chip = Chip::None;
  bool hirom = false;
  bool battery = false;
  size_t ram_bytes = 0;     // cartridge RAM the chip uses (BW-RAM, GSU RAM, SRAM)
  Byte chipset = 0;         // $FFD6
  Byte map_mode = 0;        // $FFD5
  std::string title;
};

// `header` is the ROM offset of the 64-byte block at $xxC0 (the cartridge's chosen layout).
auto detect_board(const std::vector<Byte>& rom, size_t header, bool hirom) -> BoardInfo;

// Firmware a chip needs, if any.
struct FirmwareSpec {
  Chip chip;
  const char* file;       // combined image: program ROM then data ROM, little-endian (the common combined .rom dump format)
  size_t size;
  const char* program;    // split images (dsp1b.program.rom + dsp1b.data.rom) are accepted too
  const char* data;
  const char* sha256;     // of the combined image
};
auto firmware_spec(Chip chip) -> const FirmwareSpec*;

// Builds the chip for a detected board. `rom` and `ram` are the cartridge's and must outlive
// it. Returns nullptr (and leaves `error` empty) for a plain cartridge or an unsupported chip,
// or nullptr with `error` set when required firmware is missing or the wrong size. Cx4 has no
// required firmware: a missing data ROM uses the generated math tables instead.
auto create_coprocessor(const BoardInfo& board, const std::vector<Byte>& rom, std::vector<Byte>& ram,
                        const std::string& firmware_dir, std::string& error) -> std::unique_ptr<Coprocessor>;

auto sha256_hex(const std::vector<Byte>& data) -> std::string;

} // namespace bitforge::coprocessor
