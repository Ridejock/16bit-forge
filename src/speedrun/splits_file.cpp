// SPDX-License-Identifier: MIT
#include "speedrun/splits_file.hpp"
#include "speedrun/autosplit.hpp"
#include "speedrun/lss.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace bitforge::speedrun {

namespace {

namespace fs = std::filesystem;

auto read_text(const std::string& path) -> std::optional<std::string> {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

auto write_text(const std::string& path, std::string_view text) -> bool {
  std::error_code ec;
  if (const fs::path dir = fs::path(path).parent_path(); !dir.empty()) fs::create_directories(dir, ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(out);
}

auto last_path(std::string_view data_dir, std::string_view rom_stem) -> std::string {
  return (fs::path(data_dir) / kSplitsFolder / (std::string(rom_stem) + std::string(kLastExtension))).string();
}

}  // namespace

auto default_splits_path(std::string_view data_dir, std::string_view rom_stem) -> std::string {
  return (fs::path(data_dir) / kSplitsFolder / (std::string(rom_stem) + std::string(kLssExtension))).string();
}

auto remembered_splits_path(std::string_view data_dir, std::string_view rom_stem) -> std::string {
  if (const auto text = read_text(last_path(data_dir, rom_stem))) {
    std::string line = text->substr(0, text->find_first_of("\r\n"));
    if (!line.empty()) return line;
  }
  return default_splits_path(data_dir, rom_stem);
}

auto remember_splits_path(std::string_view data_dir, std::string_view rom_stem, std::string_view path) -> bool {
  const std::string record = last_path(data_dir, rom_stem);
  if (path == default_splits_path(data_dir, rom_stem)) {
    std::error_code ec;
    fs::remove(record, ec);
    return !ec;
  }
  return write_text(record, std::string(path) + "\n");
}

auto load_splits(const std::string& path, std::string* error) -> std::optional<LoadedSplits> {
  const auto text = read_text(path);
  if (!text) {
    if (error) *error = "Can't read " + path;
    return std::nullopt;
  }
  std::string why;
  auto run = read_lss(*text, &why);
  if (!run) {
    if (error) *error = path + ": " + why;
    return std::nullopt;
  }
  if (const auto rules = read_text(autosplit_path(path))) parse_autosplits(*rules, *run);
  return LoadedSplits{std::move(*run), SplitsFile{path, *text}};
}

auto save_splits(const Run& run, SplitsFile& file, std::string* error) -> bool {
  std::string text = write_lss(run, file.original);
  const std::string sidecar = autosplit_path(file.path);
  const std::string rules = format_autosplits(run);
  bool ok = write_text(file.path, text);
  if (ok && rules.empty()) {
    std::error_code ec;
    fs::remove(sidecar, ec);
  } else if (ok) {
    ok = write_text(sidecar, rules);
  }
  if (!ok) {
    if (error) *error = "Can't write " + (rules.empty() ? file.path : file.path + " / " + sidecar);
    return false;
  }
  file.original = std::move(text);
  return true;
}

}  // namespace bitforge::speedrun
