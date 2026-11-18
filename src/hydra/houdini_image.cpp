/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Built with the Houdini Development Kit definitions, and separate from the other sources to keep
 * Houdini headers out of them. */

#include "hydra/houdini_image.h"

#include <IMG/IMG_File.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

#ifdef _WIN32
#  include <process.h>
#  define HDCYCLES_GETPID _getpid
#else
#  include <unistd.h>
#  define HDCYCLES_GETPID getpid
#endif

namespace HdCycles {

namespace fs = std::filesystem;

namespace {

std::string Lower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return char(std::tolower(c));
  });
  return text;
}

bool IsHoudiniOnlyFormat(const std::string &filename)
{
  return Lower(fs::path(filename).extension().string()) == ".rat";
}

/* The conversion cache, shared by Houdini sessions and husk. */
const fs::path &CacheDirectory()
{
  static const fs::path directory = [] {
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / "hdCycles_houdini_images";
    fs::create_directories(dir, ec);
    return dir;
  }();
  return directory;
}

/* Identifies a file by its path, size and modification time. */
std::string FileStamp(const fs::path &file)
{
  std::error_code ec;
  const auto size = fs::file_size(file, ec);
  const auto time = fs::last_write_time(file, ec).time_since_epoch().count();
  return file.string() + "|" + std::to_string(size) + "|" + std::to_string(time);
}

std::string Hex(const size_t value)
{
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

bool Convert(const fs::path &source, const fs::path &destination)
{
  std::error_code ec;
  if (fs::exists(destination, ec)) {
    return true;
  }
  static std::mutex mutex;
  const std::lock_guard<std::mutex> lock(mutex);
  if (fs::exists(destination, ec)) {
    return true;
  }
  /* Other processes may convert the same file: write a file of our own, then move it. */
  const fs::path temporary = destination.parent_path() /
                             (destination.stem().string() + ".tmp" +
                              std::to_string(HDCYCLES_GETPID()) + ".tif");
  if (!IMG_File::copyToFile(source.string().c_str(), temporary.string().c_str())) {
    fs::remove(temporary, ec);
    return false;
  }
  fs::rename(temporary, destination, ec);
  if (ec) {
    fs::remove(temporary, ec);
    return fs::exists(destination, ec);
  }
  return true;
}

}  // namespace

std::string ResolveHoudiniImage(const std::string &filename)
{
  if (!IsHoudiniOnlyFormat(filename) || CacheDirectory().empty()) {
    return filename;
  }

  const size_t udim = filename.find("<UDIM>");
  if (udim == std::string::npos) {
    const fs::path destination = CacheDirectory() /
                                 (Hex(std::hash<std::string>()(FileStamp(filename))) + ".tif");
    return Convert(filename, destination) ? destination.string() : filename;
  }

  /* UDIM tiles: prefix<UDIM>suffix in the pattern's directory. */
  const fs::path pattern(filename);
  const std::string name = pattern.filename().string();
  const size_t token = name.find("<UDIM>");
  if (token == std::string::npos) {
    return filename;
  }
  const std::string prefix = Lower(name.substr(0, token));
  const std::string suffix = Lower(name.substr(token + 6));
  std::vector<std::pair<std::string, fs::path>> tiles;
  std::error_code ec;
  for (const fs::directory_entry &entry : fs::directory_iterator(pattern.parent_path(), ec)) {
    const std::string file = Lower(entry.path().filename().string());
    if (file.size() != prefix.size() + 4 + suffix.size() || file.rfind(prefix, 0) != 0 ||
        file.compare(file.size() - suffix.size(), suffix.size(), suffix) != 0)
    {
      continue;
    }
    const std::string number = file.substr(prefix.size(), 4);
    if (std::all_of(number.begin(), number.end(), ::isdigit) && number >= "1001") {
      tiles.emplace_back(number, entry.path());
    }
  }
  if (tiles.empty()) {
    return filename;
  }
  std::sort(tiles.begin(), tiles.end());
  std::string stamp = filename;
  for (const auto &[number, path] : tiles) {
    stamp += "|" + FileStamp(path);
  }
  const std::string key = Hex(std::hash<std::string>()(stamp));
  for (const auto &[number, path] : tiles) {
    if (!Convert(path, CacheDirectory() / (key + "." + number + ".tif"))) {
      return filename;
    }
  }
  return (CacheDirectory() / (key + ".<UDIM>.tif")).string();
}

}  // namespace HdCycles
