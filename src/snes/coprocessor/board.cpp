// SPDX-License-Identifier: MIT
#include "snes/coprocessor/board.hpp"
#include "snes/coprocessor/cx4.hpp"
#include "snes/coprocessor/cx4_rom.hpp"
#include "snes/coprocessor/gsu.hpp"
#include "snes/coprocessor/necdsp.hpp"
#include "snes/coprocessor/obc1.hpp"
#include "snes/coprocessor/sa1.hpp"
#include "snes/coprocessor/sdd1.hpp"
#include "snes/coprocessor/spc7110.hpp"
#include "utils/logging.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>

namespace snes::coprocessor {

namespace {

// Header fields relative to the $xxC0 block.
constexpr int kTitle = 0x00;
constexpr int kMapMode = 0x15;
constexpr int kChipset = 0x16;
constexpr int kRamSize = 0x18;
constexpr int kExtendedMarker = 0x1A;  // $33 when the extended header ($xxB0-$xxBF) is valid
// $xxBD (expansion RAM) and $xxBF (chip subtype) sit just before the block: header - 3, header - 1.

constexpr size_t kMaxChipRam = 256 * 1024;
constexpr size_t kGsuDefaultRam = 32 * 1024;  // Star Fox has no extended header
constexpr size_t kSa1MinimumRam = 2 * 1024;

// Chipset high nibble -> chip (low nibble >= 3).
struct ChipsetRule { Byte high; int subtype; Chip chip; };  // subtype -1: any
constexpr std::array<ChipsetRule, 10> kChipsets = {{
  {0x0, -1, Chip::DSP1B},        // refined by kDspTitles
  {0x1, -1, Chip::SuperFX},
  {0x2, -1, Chip::OBC1},
  {0x3, -1, Chip::SA1},
  {0x4, -1, Chip::SDD1},
  {0x5, -1, Chip::SRTC},
  {0xF, 0x00, Chip::SPC7110},
  {0xF, 0x01, Chip::ST010},      // refined by kDspTitles
  {0xF, 0x02, Chip::ST018},
  {0xF, 0x10, Chip::Cx4},
}};

// Games whose NEC firmware isn't the default for their chipset.
struct TitleRule { const char* title; Chip chip; };
constexpr std::array<TitleRule, 7> kDspTitles = {{
  {"PILOTWINGS", Chip::DSP1},
  {"DUNGEON MASTER", Chip::DSP2},
  {"SD\xB6\xDE\xDD\xC0\xDE\xD1GX", Chip::DSP3},  // SD Gundam GX (half-width katakana)
  {"PLANETS CHAMP TG3000", Chip::DSP4},
  {"TOP GEAR 3000", Chip::DSP4},
  {"2DAN MORITA SHOUGI", Chip::ST011},
  {"F1 ROC II", Chip::ST010},
}};

constexpr std::array<FirmwareSpec, 8> kFirmware = {{
  // Cx4: optional data ROM (1024 x 24-bit math tables). Generated if missing; cx4.rom works too.
  {Chip::Cx4, "cx4.data.rom", 3072, "", "",
   "ae8d4d1961b93421ff00b3caa1d0f0ce7783e749772a3369c36b3dbf0d37ef18"},
  {Chip::DSP1, "dsp1.rom", 8192, "dsp1.program.rom", "dsp1.data.rom",
   "91e87d11e1c30d172556bed2211cce2efa94ba595f58c5d264809ef4d363a97b"},
  {Chip::DSP1B, "dsp1b.rom", 8192, "dsp1b.program.rom", "dsp1b.data.rom",
   "d789cb3c36b05c0b23b6c6f23be7aa37c6e78b6ee9ceac8d2d2aa9d8c4d35fa9"},
  {Chip::DSP2, "dsp2.rom", 8192, "dsp2.program.rom", "dsp2.data.rom",
   "03ef4ef26c9f701346708cb5d07847b5203cf1b0818bf2930acd34510ffdd717"},
  {Chip::DSP3, "dsp3.rom", 8192, "dsp3.program.rom", "dsp3.data.rom",
   "0971b08f396c32e61989d1067dddf8e4b14649d548b2188f7c541b03d7c69e4e"},
  {Chip::DSP4, "dsp4.rom", 8192, "dsp4.program.rom", "dsp4.data.rom",
   "752d03b2d74441e430b7f713001fa241f8bbcfc1a0d890ed4143f174dbe031da"},
  {Chip::ST010, "st010.rom", 53248, "st010.program.rom", "st010.data.rom",
   "fa9bced838fedea11c6f6ace33d1878024bdd0d02cc9485899d0bdd4015ec24c"},
  {Chip::ST011, "st011.rom", 53248, "st011.program.rom", "st011.data.rom",
   "8b2b3f3f3e6e29f4d21d8bc736b400bc988b7d2214ebee15643f01c1fee2f364"},
}};

// Where each NEC board puts the DSP. The DR/SR select line is A14 on LoROM boards and A12 on
// the HiROM board; the uPD96050 boards use A0 and expose the data RAM.
constexpr NecBoard kDsp1LoRom1MB{"DSP-1 LoROM (<= 1 MB)", {0x30, 0x3F, 0x8000, 0xFFFF, 0x4000}, std::nullopt};
constexpr NecBoard kDsp1LoRom2MB{"DSP-1 LoROM (2 MB)", {0x60, 0x6F, 0x0000, 0x7FFF, 0x4000}, std::nullopt};
constexpr NecBoard kDsp1HiRom{"DSP-1 HiROM", {0x00, 0x1F, 0x6000, 0x7FFF, 0x1000}, std::nullopt};
constexpr NecBoard kDsp2Board{"DSP-2 LoROM", {0x20, 0x3F, 0x8000, 0xFFFF, 0x4000}, std::nullopt};
constexpr NecBoard kDsp3Board{"DSP-3 LoROM", {0x20, 0x3F, 0x8000, 0xFFFF, 0x4000}, std::nullopt};
constexpr NecBoard kDsp4Board{"DSP-4 LoROM", {0x30, 0x3F, 0x8000, 0xFFFF, 0x4000}, std::nullopt};
constexpr NecBoard kSt01xBoard{"ST010/ST011 LoROM", {0x60, 0x67, 0x0000, 0x3FFF, 0x0001},
                               NecWindow{0x68, 0x6F, 0x0000, 0x7FFF, 0x0FFF}};
inline constexpr NecVariant kUpd96050St011{"uPD96050 (ST011)", 14, 11, 11, 16, 16384, 2048, 2048, 15'000'000};

auto title_of(const std::vector<Byte>& rom, size_t header) -> std::string {
  std::string title;
  for (int i = 0; i < 21; i++) title.push_back(static_cast<char>(rom[header + kTitle + static_cast<size_t>(i)]));
  while (!title.empty() && (title.back() == ' ' || title.back() == '\0')) title.pop_back();
  return title;
}

auto size_from_code(Byte code) -> size_t {
  code &= 0x0F;
  if (code == 0) return 0;
  return std::size_t{1024} << std::min<Byte>(code, 8);
}

auto read_file(const std::filesystem::path& path) -> std::optional<std::vector<Byte>> {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::vector<Byte>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Loads a chip's firmware (combined or split). On failure says what's missing.
auto load_firmware(const FirmwareSpec& spec, const std::string& dir, std::vector<Byte>& out, std::string& why) -> bool {
  const std::filesystem::path base(dir);
  if (auto data = read_file(base / spec.file)) {
    if (data->size() != spec.size) {
      why = std::string(spec.file) + " is " + std::to_string(data->size()) + " bytes, expected " + std::to_string(spec.size);
      return false;
    }
    out = std::move(*data);
    return true;
  }
  if (*spec.program == '\0') {  // single-file firmware
    why = "missing " + std::string(spec.file) + " (" + std::to_string(spec.size) + " bytes)";
    return false;
  }
  auto program = read_file(base / spec.program);
  auto data = read_file(base / spec.data);
  if (program && data) {
    if (program->size() + data->size() != spec.size) {
      why = std::string(spec.program) + " + " + spec.data + " are " + std::to_string(program->size() + data->size()) +
            " bytes, expected " + std::to_string(spec.size);
      return false;
    }
    out = std::move(*program);
    out.insert(out.end(), data->begin(), data->end());
    return true;
  }
  why = "missing " + std::string(spec.file) + " (" + std::to_string(spec.size) + " bytes)";
  return false;
}

auto nec_board_for(const BoardInfo& board, size_t rom_size) -> const NecBoard& {
  switch (board.chip) {
    case Chip::DSP2: return kDsp2Board;
    case Chip::DSP3: return kDsp3Board;
    case Chip::DSP4: return kDsp4Board;
    case Chip::ST010: case Chip::ST011: return kSt01xBoard;
    default: break;
  }
  if (board.hirom) return kDsp1HiRom;
  return rom_size > 0x100000 ? kDsp1LoRom2MB : kDsp1LoRom1MB;
}

} // namespace

auto chip_name(Chip chip) -> const char* {
  switch (chip) {
    case Chip::None: return "none";
    case Chip::DSP1: return "DSP-1";
    case Chip::DSP1B: return "DSP-1B";
    case Chip::DSP2: return "DSP-2";
    case Chip::DSP3: return "DSP-3";
    case Chip::DSP4: return "DSP-4";
    case Chip::ST010: return "ST010";
    case Chip::ST011: return "ST011";
    case Chip::SuperFX: return "Super FX";
    case Chip::SA1: return "SA-1";
    case Chip::OBC1: return "OBC1";
    case Chip::Cx4: return "Cx4";
    case Chip::SDD1: return "S-DD1";
    case Chip::SPC7110: return "SPC7110";
    case Chip::ST018: return "ST018";
    case Chip::SRTC: return "S-RTC";
    case Chip::SuperGameBoy: return "Super Game Boy";
    case Chip::Unknown: return "unknown chip";
  }
  return "?";
}

auto detect_board(const std::vector<Byte>& rom, size_t header, bool hirom) -> BoardInfo {
  BoardInfo info;
  info.hirom = hirom;
  if (rom.size() < header + 0x40 || header < 0x10) return info;
  info.map_mode = rom[header + kMapMode];
  info.chipset = rom[header + kChipset];
  info.title = title_of(rom, header);
  const Byte low = info.chipset & 0x0F;
  const Byte high = info.chipset >> 4;
  const bool extended = rom[header + kExtendedMarker] == 0x33;
  const Byte subtype = rom[header - 1];
  info.battery = low == 0x2 || low == 0x5 || low == 0x6 || low == 0x9 || low == 0xA;
  if (low < 0x3) return info;  // ROM, ROM+RAM, ROM+RAM+battery

  info.chip = Chip::Unknown;
  for (const ChipsetRule& rule : kChipsets) {
    if (rule.high == high && (rule.subtype < 0 || rule.subtype == subtype)) {
      info.chip = rule.chip;
      break;
    }
  }
  if (high == 0xE) info.chip = low == 0x3 ? Chip::SuperGameBoy : Chip::Unknown;
  if (info.chip == Chip::DSP1B || info.chip == Chip::ST010) {
    for (const TitleRule& rule : kDspTitles) {
      if (info.title == rule.title) info.chip = rule.chip;
    }
  }

  const size_t ram = size_from_code(rom[header + kRamSize]);
  const size_t expansion = extended ? size_from_code(rom[header - 3]) : 0;
  switch (info.chip) {
    case Chip::SuperFX: info.ram_bytes = std::max({ram, expansion, extended ? size_t{0} : kGsuDefaultRam}); break;
    // The SA-1 always has BW-RAM wired up (its bitmap modes and work RAM live there); a header
    // without RAM gets a small non-battery one (absindx's "NO SRAM" test cart relies on it).
    case Chip::SA1: info.ram_bytes = std::max({ram, expansion, kSa1MinimumRam}); break;
    default: info.ram_bytes = ram; break;
  }
  if (info.ram_bytes == 0 && info.chip == Chip::SuperFX) info.ram_bytes = kGsuDefaultRam;
  info.ram_bytes = std::min(info.ram_bytes, kMaxChipRam);
  return info;
}

auto firmware_spec(Chip chip) -> const FirmwareSpec* {
  for (const FirmwareSpec& spec : kFirmware) {
    if (spec.chip == chip) return &spec;
  }
  return nullptr;
}

auto create_coprocessor(const BoardInfo& board, const std::vector<Byte>& rom, std::vector<Byte>& ram,
                        const std::string& firmware_dir, std::string& error) -> std::unique_ptr<Coprocessor> {
  using utils::LogChannel;
  using utils::Logger;
  error.clear();
  switch (board.chip) {
    case Chip::None:
      return nullptr;
    case Chip::SuperFX:
      return std::make_unique<GSU>(rom, ram);
    case Chip::SA1:
      return std::make_unique<SA1>(rom, ram);
    case Chip::OBC1:
      return std::make_unique<OBC1>(ram);
    case Chip::SDD1:
      return std::make_unique<SDD1>(rom, ram);
    case Chip::SPC7110:  // chipset $F9 adds the RTC-4513 (Tengai Makyou Zero)
      return std::make_unique<SPC7110>(rom, ram, board.chipset == 0xF9);
    case Chip::Cx4: {
      const FirmwareSpec* spec = firmware_spec(Chip::Cx4);
      std::vector<Byte> image;
      std::string why;
      const char* used = "generated";
      if (load_firmware(*spec, firmware_dir, image, why)) {
        used = spec->file;
      } else {
        const FirmwareSpec alternate{Chip::Cx4, "cx4.rom", spec->size, "", "", spec->sha256};
        std::string why_alternate;
        if (load_firmware(alternate, firmware_dir, image, why_alternate)) {
          used = alternate.file;
        } else if (why.rfind("missing", 0) == 0 && why_alternate.rfind("missing", 0) == 0) {
          const auto generated = generate_cx4_data_rom();
          image.assign(generated.begin(), generated.end());
        } else {
          const std::string folder = firmware_dir.empty() ? std::string("firmware/") : firmware_dir;
          error = "Cx4 firmware can't be used: " + (why.rfind("missing", 0) == 0 ? why_alternate : why) + " (in " + folder + ")";
          return nullptr;
        }
      }
      if (std::strcmp(used, "generated") != 0 && sha256_hex(image) != spec->sha256) {
        Logger::warning(LogChannel::Rom, "Cx4: %s doesn't match the known dump (SHA-256 %s); it may not work", used,
                        sha256_hex(image).c_str());
      }
      Logger::info(LogChannel::Rom, "Cx4 (HG51B169, LLE), data ROM %s", used);
      return std::make_unique<Cx4>(rom, ram, std::span<const Byte>(image.data(), image.size()));
    }
    case Chip::DSP1: case Chip::DSP1B: case Chip::DSP2: case Chip::DSP3: case Chip::DSP4:
    case Chip::ST010: case Chip::ST011: {
      Chip chip = board.chip;
      const FirmwareSpec* spec = firmware_spec(chip);
      std::vector<Byte> image;
      std::string why;
      if (!load_firmware(*spec, firmware_dir, image, why)) {
        // DSP-1 and DSP-1B differ only in bug fixes: either will do, with a warning.
        const Chip other = chip == Chip::DSP1 ? Chip::DSP1B : chip == Chip::DSP1B ? Chip::DSP1 : Chip::None;
        std::string ignored;
        if (other != Chip::None && load_firmware(*firmware_spec(other), firmware_dir, image, ignored)) {
          Logger::warning(LogChannel::Rom, "%s: %s; using %s instead", chip_name(chip), why.c_str(), firmware_spec(other)->file);
          chip = other;
          spec = firmware_spec(other);
        } else {
          const std::string folder = firmware_dir.empty() ? std::string("firmware/") : firmware_dir;
          if (why.rfind("missing", 0) == 0) {
            error = std::string(chip_name(board.chip)) + " firmware not found: put " + spec->file + " (" +
                    std::to_string(spec->size) + " bytes) in " + folder;
          } else {
            error = std::string(chip_name(board.chip)) + " firmware can't be used: " + why + " (in " + folder + ")";
          }
          return nullptr;
        }
      }
      if (sha256_hex(image) != spec->sha256) {
        Logger::warning(LogChannel::Rom, "%s: %s doesn't match the known dump (SHA-256 %s); it may not work",
                        chip_name(chip), spec->file, sha256_hex(image).c_str());
      }
      const bool upd96050 = chip == Chip::ST010 || chip == Chip::ST011;
      const NecVariant& variant = chip == Chip::ST011 ? kUpd96050St011 : upd96050 ? kUpd96050 : kUpd7725;
      const NecBoard& nec_board = nec_board_for(board, rom.size());
      auto dsp = std::make_unique<NecDsp>(chip, variant, nec_board);
      const size_t program_bytes = variant.program_words * 3;
      dsp->core().load_firmware(std::span<const uint8_t>(image.data(), program_bytes),
                                std::span<const uint8_t>(image.data() + program_bytes, image.size() - program_bytes));
      Logger::info(LogChannel::Rom, "%s (%s) on %s, firmware %s", chip_name(chip), variant.name, nec_board.name, spec->file);
      return dsp;
    }
    default:
      Logger::warning(LogChannel::Rom, "%s isn't emulated: running as a plain cartridge, the game won't work properly",
                      chip_name(board.chip));
      return nullptr;
  }
}

// ---- SHA-256 (FIPS 180-4), for firmware identification ------------------------------------------

auto sha256_hex(const std::vector<Byte>& data) -> std::string {
  static constexpr std::array<uint32_t, 64> k = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  };
  std::array<uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::vector<Byte> msg = data;
  const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
  msg.push_back(0x80);
  while (msg.size() % 64 != 56) msg.push_back(0);
  for (int i = 7; i >= 0; i--) msg.push_back(static_cast<Byte>(bits >> (i * 8)));
  auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
    std::array<uint32_t, 64> w{};
    for (size_t i = 0; i < 16; i++) {
      w[i] = static_cast<uint32_t>(msg[chunk + i * 4] << 24 | msg[chunk + i * 4 + 1] << 16 |
                                   msg[chunk + i * 4 + 2] << 8 | msg[chunk + i * 4 + 3]);
    }
    for (size_t i = 16; i < 64; i++) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::array<uint32_t, 8> v = h;
    for (size_t i = 0; i < 64; i++) {
      const uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
      const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      const uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
      const uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
      const uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      const uint32_t t2 = s0 + maj;
      v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
    }
    for (size_t i = 0; i < 8; i++) h[i] += v[i];
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (uint32_t word : h) {
    for (int i = 28; i >= 0; i -= 4) out.push_back(kHex[(word >> i) & 0xF]);
  }
  return out;
}

} // namespace snes::coprocessor
