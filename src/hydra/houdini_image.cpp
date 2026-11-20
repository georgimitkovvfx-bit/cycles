/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Built with the Houdini Development Kit definitions, and separate from the other sources to keep
 * Houdini headers out of them. */

#include "hydra/houdini_image.h"

#include <IMG/IMG_File.h>
#include <TIL/TIL_CopResolver.h>
#include <TIL/TIL_Raster.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
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

std::mutex &ConvertMutex()
{
  static std::mutex mutex;
  return mutex;
}

/* Hash of a file's whole content, read in chunks. */
std::string ContentHash(const fs::path &file)
{
  std::ifstream in(file, std::ios::binary);
  constexpr std::streamsize chunk = 1 << 20;
  std::string buffer(size_t(chunk), '\0');
  size_t hash = 0;
  while (in) {
    in.read(buffer.data(), chunk);
    const size_t part = std::hash<std::string_view>()(
        std::string_view(buffer.data(), size_t(in.gcount())));
    hash ^= part + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
  }
  return Hex(hash);
}

/* The file of each op: image, and the version of its COP node then. */
struct OpImage {
  int opid;
  size_t version;
  std::string file;
};
std::unordered_map<std::string, OpImage> opImages;

/* Copernicus and COP images ("op:" paths), which have no file to stamp: the image of now, named
 * by its content, so a changed image gets a new file (and Cycles loads it). */
std::string ConvertOpImage(const std::string &path)
{
  const std::lock_guard<std::mutex> lock(ConvertMutex());
  std::error_code ec;
  /* Every material/light sync resolves the image again: no cook while the COP is unchanged
   * (KNOWN_ISSUES #55). Frame changes keep the version, Houdini then sets
   * houdini:cop_texture_changed (ForgetHoudiniOpImages). */
  if (const auto it = opImages.find(path);
      it != opImages.end() &&
      TIL_CopResolver::getRasterVersion(it->second.opid) == it->second.version &&
      fs::exists(it->second.file, ec))
  {
    return it->second.file;
  }
  const std::string name = "op_" + Hex(std::hash<std::string>()(path));
  const fs::path temporary = CacheDirectory() /
                             (name + ".tmp" + std::to_string(HDCYCLES_GETPID()) + "_" +
                              Hex(std::hash<std::thread::id>()(std::this_thread::get_id())) +
                              ".tif");
  /* Houdini's image library has no op: format, the COP resolver cooks the node (only inside
   * Houdini, husk has no resolver). Through the resolver cache, whose COPs Houdini's viewport
   * watches to set houdini:cop_texture_changed; Houdini clears its stale entries then. */
  int opid = -1;
  bool saved = false;
  const TIL_CopResolver::autoEnableCache cache(true);
  if (TIL_Raster *raster = TIL_CopResolver::getRaster(path.c_str(), opid)) {
    /* Named by the cooked pixels: an unchanged image (every material/light sync) is not saved
     * and hashed again (KNOWN_ISSUES #55). */
    const std::string header = std::to_string(raster->getXres()) + "x" +
                               std::to_string(raster->getYres()) + "|" +
                               std::to_string(int(raster->getFormat())) + "|" +
                               std::to_string(int(raster->getPacking())) + "|";
    const void *pixels = raster->getPixels();
    const size_t hash = std::hash<std::string_view>()(std::string_view(
        static_cast<const char *>(pixels), pixels ? size_t(raster->getSize()) : 0));
    const fs::path destination = CacheDirectory() /
                                 (name + "_r" +
                                  Hex(hash ^ (std::hash<std::string>()(header) << 1)) + ".tif");
    const OpImage image = {opid, TIL_CopResolver::getRasterVersion(opid), destination.string()};
    if (pixels && fs::exists(destination, ec)) {
      TIL_CopResolver::doneWithRaster(raster);
      opImages[path] = image;
      return destination.string();
    }
    if (pixels && IMG_File::saveRasterAsFile(temporary.string().c_str(), raster)) {
      TIL_CopResolver::doneWithRaster(raster);
      fs::rename(temporary, destination, ec);
      if (ec) {
        fs::remove(temporary, ec);
        return fs::exists(destination, ec) ? destination.string() : path;
      }
      opImages[path] = image;
      return destination.string();
    }
    saved = !pixels && IMG_File::saveRasterAsFile(temporary.string().c_str(), raster);
    TIL_CopResolver::doneWithRaster(raster);
  }
  if (!saved && !IMG_File::copyToFile(path.c_str(), temporary.string().c_str())) {
    fs::remove(temporary, ec);
    return path;
  }
  const fs::path destination = CacheDirectory() / (name + "_" + ContentHash(temporary) + ".tif");
  if (fs::exists(destination, ec)) {
    fs::remove(temporary, ec);
  }
  else {
    fs::rename(temporary, destination, ec);
    if (ec) {
      fs::remove(temporary, ec);
      return fs::exists(destination, ec) ? destination.string() : path;
    }
  }
  return destination.string();
}

bool Convert(const fs::path &source, const fs::path &destination)
{
  std::error_code ec;
  if (fs::exists(destination, ec)) {
    return true;
  }
  const std::lock_guard<std::mutex> lock(ConvertMutex());
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

namespace {

std::string ResolveSource(const std::string &filename);

/* Source of each converted file, so resolving a converted file (Reload Textures resolves the
 * compiled shader graphs again) converts its source again when it changed. */
std::mutex sourcesMutex;
std::unordered_map<std::string, std::string> sources;

}  // namespace

void ForgetHoudiniOpImages()
{
  const std::lock_guard<std::mutex> lock(ConvertMutex());
  opImages.clear();
  /* Not the resolver cache: it is Houdini's, its viewport clears it after setting
   * houdini:cop_texture_changed (HUSD_Imaging::handleCopTextureChange). */
}

bool IsHoudiniOpImage(const std::string &filename)
{
  if (filename.rfind("op:", 0) == 0) {
    return true;
  }
  const std::lock_guard<std::mutex> lock(sourcesMutex);
  const auto it = sources.find(filename);
  return it != sources.end() && it->second.rfind("op:", 0) == 0;
}

std::string ResolveHoudiniImage(const std::string &filename)
{
  std::string source = filename;
  {
    const std::lock_guard<std::mutex> lock(sourcesMutex);
    if (const auto it = sources.find(filename); it != sources.end()) {
      source = it->second;
    }
  }
  const std::string resolved = ResolveSource(source);
  if (resolved != source) {
    const std::lock_guard<std::mutex> lock(sourcesMutex);
    sources[resolved] = source;
  }
  return resolved;
}

namespace {

std::string ResolveSource(const std::string &filename)
{
  if (CacheDirectory().empty()) {
    return filename;
  }
  if (filename.rfind("op:", 0) == 0) {
    return ConvertOpImage(filename);
  }
  if (!IsHoudiniOnlyFormat(filename)) {
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

}  // namespace

}  // namespace HdCycles
