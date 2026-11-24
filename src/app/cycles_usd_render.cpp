/* SPDX-FileCopyrightText: 2026 USD Nodes (hdcycles-blender patch 0010)
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* cycles_usd_render: render frames of a USD file with Cycles through its Hydra delegate, without Blender.
 *
 * The stage, the Hydra render index and the Cycles scene stay in memory for the whole frame list: each frame sets the
 * stage time, syncs only what that changed (UsdImaging marks time-varying prims dirty) and renders in the same
 * session. Prints CYCLES_USD_RENDER_TIME lines (load, frame <frame> <sync s> <total s> <peak MB>) for USD Nodes'
 * Render node and tools/bench. */

#include "session/output_driver.h"

#include "device/device.h"
#include "hydra/camera.h"
#include "hydra/render_delegate.h"
#include "hydra/session.h"
#include "scene/camera.h"
#include "scene/film.h"
#include "scene/object.h"
#include "scene/shader.h"
#include "scene/pass.h"
#include "scene/scene.h"
#include "session/buffers.h"
#include "session/session.h"
#include "util/log.h"
#include "util/murmurhash.h"
#include "util/path.h"
#include "util/string.h"
#include "util/time.h"
#include "util/unique_ptr.h"

#ifdef WITH_OCIO
#  include <OpenColorIO/OpenColorIO.h>
#endif

#include <pxr/base/plug/registry.h>
#include <pxr/imaging/hd/legacyTaskFactory.h>
#include <pxr/imaging/hd/legacyTaskSchema.h>
#include <pxr/imaging/hd/mergingSceneIndex.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/geomUtil/cuboidMeshGenerator.h>
#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h>
#include <pxr/imaging/hdsi/implicitSurfaceSceneIndex.h>
#include <pxr/imaging/hdsi/velocityMotionResolvingSceneIndex.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdRender/var.h>
#include <pxr/usdImaging/usdImaging/materialBindingsResolvingSceneIndex.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#endif

#include <sys/resource.h>
#include <unistd.h>

#include <OpenImageIO/imageio.h>

#include <cmath>
#include <unordered_map>
#include <cstdio>
#include <filesystem>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

/* A task that only gives the render index its render tags (as in hydra/file_reader.cpp). */
class TagsTask : public HdTask {
 public:
  TagsTask(HdSceneDelegate * /*delegate*/, const SdfPath &id)
      : HdTask(id), tags({HdRenderTagTokens->geometry, HdRenderTagTokens->render})
  {
  }

 protected:
  void Sync(HdSceneDelegate * /*delegate*/, HdTaskContext * /*ctx*/, HdDirtyBits * /*bits*/) override {}
  void Prepare(HdTaskContext * /*ctx*/, HdRenderIndex * /*index*/) override {}
  void Execute(HdTaskContext * /*ctx*/) override {}
  const TfTokenVector &GetRenderTags() const override
  {
    return tags;
  }

  TfTokenVector tags;
};

/* Gives the meshes HdsiImplicitSurfaceSceneIndex makes of Cubes their face normals (uniform), which it leaves out:
 * its cube shares 8 corners, and smoothing them darkened every face (the delegate renders uniform normals flat,
 * patch 0017). Its other shapes have their own cap points, so smooth normals are right there. ``shapes`` is the
 * scene before the conversion: there the prim is still a cube. */
class CubeNormalsSceneIndex : public HdSingleInputFilteringSceneIndexBase {
 public:
  static TfRefPtr<CubeNormalsSceneIndex> New(const HdSceneIndexBaseRefPtr &input,
                                             const HdSceneIndexBaseRefPtr &shapes)
  {
    return TfCreateRefPtr(new CubeNormalsSceneIndex(input, shapes));
  }

  HdSceneIndexPrim GetPrim(const SdfPath &path) const override
  {
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(path);
    if (prim.primType != HdPrimTypeTokens->mesh || !prim.dataSource ||
        _shapes->GetPrim(path).primType != HdPrimTypeTokens->cube)
    {
      return prim;
    }
    static const HdContainerDataSourceHandle normals = [] {
      VtVec3fArray values(GeomUtilCuboidMeshGenerator::ComputeNumNormals());
      GeomUtilCuboidMeshGenerator::GenerateNormals(values.begin());
      return HdRetainedContainerDataSource::New(
          HdPrimvarsSchema::GetSchemaToken(),
          HdRetainedContainerDataSource::New(
              HdTokens->normals,
              HdPrimvarSchema::Builder()
                  .SetPrimvarValue(HdRetainedTypedSampledDataSource<VtVec3fArray>::New(values))
                  .SetInterpolation(
                      HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->uniform))
                  .SetRole(HdPrimvarSchema::BuildRoleDataSource(HdPrimvarSchemaTokens->normal))
                  .Build()));
    }();
    prim.dataSource = HdOverlayContainerDataSource::New(normals, prim.dataSource);
    return prim;
  }

  SdfPathVector GetChildPrimPaths(const SdfPath &path) const override
  {
    return _GetInputSceneIndex()->GetChildPrimPaths(path);
  }

 protected:
  CubeNormalsSceneIndex(const HdSceneIndexBaseRefPtr &input, const HdSceneIndexBaseRefPtr &shapes)
      : HdSingleInputFilteringSceneIndexBase(input), _shapes(shapes)
  {
  }

  void _PrimsAdded(const HdSceneIndexBase & /*sender*/,
                   const HdSceneIndexObserver::AddedPrimEntries &entries) override
  {
    _SendPrimsAdded(entries);
  }
  void _PrimsRemoved(const HdSceneIndexBase & /*sender*/,
                     const HdSceneIndexObserver::RemovedPrimEntries &entries) override
  {
    _SendPrimsRemoved(entries);
  }
  void _PrimsDirtied(const HdSceneIndexBase & /*sender*/,
                     const HdSceneIndexObserver::DirtiedPrimEntries &entries) override
  {
    _SendPrimsDirtied(entries);
  }

 private:
  HdSceneIndexBaseRefPtr _shapes;
};

/* The stage's RenderVars (Cycles AOV names, hdcycles-blender patch 0004) as Blender lays them out in a multilayer
 * EXR: pass name and channels. The Blender runner writes the same file. */
struct LayerInfo {
  const char *pass;
  const char *channels;
};
const std::unordered_map<std::string, LayerInfo> kLayers = {
    {"color", {"Combined", "RGBA"}},
    {"depth", {"Depth", "Z"}},
    {"z", {"Depth", "Z"}},
    {"mist", {"Mist", "Z"}},
    {"position", {"Position", "XYZ"}},
    {"normal", {"Normal", "XYZ"}},
    {"vector", {"Vector", "XYZW"}},
    {"uv", {"UV", "UVA"}},
    {"object_index", {"Object Index", "X"}},
    {"material_index", {"Material Index", "X"}},
    {"diffuse_direct", {"Diffuse Direct", "RGB"}},
    {"diffuse_indirect", {"Diffuse Indirect", "RGB"}},
    {"diffuse_color", {"Diffuse Color", "RGB"}},
    {"glossy_direct", {"Glossy Direct", "RGB"}},
    {"glossy_indirect", {"Glossy Indirect", "RGB"}},
    {"glossy_color", {"Glossy Color", "RGB"}},
    {"transmission_direct", {"Transmission Direct", "RGB"}},
    {"transmission_indirect", {"Transmission Indirect", "RGB"}},
    {"transmission_color", {"Transmission Color", "RGB"}},
    {"volume_direct", {"Volume Direct", "RGB"}},
    {"volume_indirect", {"Volume Indirect", "RGB"}},
    {"emit", {"Emission", "RGB"}},
    {"environment", {"Environment", "RGB"}},
    {"ambient_occlusion", {"Ambient Occlusion", "RGB"}},
    {"shadow_catcher", {"Shadow Catcher", "RGB"}},
};
const char *const kViewLayer = "ViewLayer";

struct Layer {
  string pass;     /* Cycles pass name (the AOV name) */
  string exr;      /* Blender's pass name */
  string channels; /* one letter per channel */
};

/* Writes the full image when it is done: the beauty as plain RGBA, or every pass as a Blender-style multilayer EXR
 * (`ViewLayer.<pass>.<channel>`, Cryptomatte manifests in the header). Written to a temporary file and renamed, so
 * an interrupted render never leaves a partial image that --skip-existing would keep. */
/* A display image (.png, .jpg): the beauty through Blender's colour management, as Blender saves a render — scale by
 * 2^exposure in scene linear, the look, the view and display, then the 1 / gamma exponent (Blender's
 * create_ocio_display_processor) — and 8 bits with Blender's dither (float_to_byte_dither_v4). */
struct DisplayImage {
  string display = "sRGB";
  string view = "AgX";
  string look = "None";
  float exposure = 0.0f;
  float gamma = 1.0f;
  float dither = 1.0f;
  int quality = 90;
#ifdef WITH_OCIO
  OCIO_NAMESPACE::ConstCPUProcessorRcPtr cpu;
#endif

  /* "" or why the transform cannot be made (unknown display, view or look). */
  string prepare()
  {
#ifdef WITH_OCIO
    namespace OCIO = OCIO_NAMESPACE;
    try {
      const OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig();
      OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();
      string from = OCIO::ROLE_SCENE_LINEAR;
      if (exposure != 0.0f) {
        const double s = std::pow(2.0, double(exposure));
        const double m[16] = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1};
        OCIO::MatrixTransformRcPtr scale = OCIO::MatrixTransform::Create();
        scale->setMatrix(m);
        group->appendTransform(scale);
      }
      bool use_look = !look.empty() && look != "None";
      if (use_look) {
        const char *look_output = OCIO::LookTransform::GetLooksResultColorSpace(
            config, config->getCurrentContext(), look.c_str());
        if (look_output && look_output[0]) {
          OCIO::LookTransformRcPtr lt = OCIO::LookTransform::Create();
          lt->setSrc(from.c_str());
          const char *view_space = config->getDisplayViewColorSpaceName(display.c_str(), view.c_str());
          const OCIO::ConstColorSpaceRcPtr view_colorspace = view_space ? config->getColorSpace(view_space) :
                                                                          nullptr;
          if (view_colorspace && view_colorspace->isData()) {
            lt->setDst(from.c_str());
          }
          else {
            lt->setDst(look_output);
            from = look_output;
          }
          lt->setLooks(look.c_str());
          group->appendTransform(lt);
        }
        else {
          use_look = false;
        }
      }
      OCIO::DisplayViewTransformRcPtr dvt = OCIO::DisplayViewTransform::Create();
      dvt->setSrc(from.c_str());
      dvt->setLooksBypass(use_look);
      dvt->setView(view.c_str());
      dvt->setDisplay(display.c_str());
      group->appendTransform(dvt);
      if (gamma != 1.0f) {
        const double e = 1.0 / std::max(double(FLT_EPSILON), double(gamma));
        const double value[4] = {e, e, e, 1.0};
        OCIO::ExponentTransformRcPtr exponent = OCIO::ExponentTransform::Create();
        exponent->setValue(value);
        group->appendTransform(exponent);
      }
      cpu = config->getProcessor(group)->getDefaultCPUProcessor();
      return "";
    }
    catch (const OCIO::Exception &exception) {
      return string("colour management: ") + exception.what();
    }
#else
    return "this build has no OpenColorIO: write .exr";
#endif
  }

  /* RGBA premultiplied linear, top-down → 8-bit straight RGBA through the display transform. */
  vector<unsigned char> bytes(vector<float> &rgba, const int width, const int height) const
  {
    const size_t count = size_t(width) * height;
    for (size_t i = 0; i < count; i++) {
      float *px = &rgba[i * 4];
      if (px[3] > 0.0f && px[3] < 1.0f) {
        px[0] /= px[3];
        px[1] /= px[3];
        px[2] /= px[3];
      }
    }
#ifdef WITH_OCIO
    OCIO_NAMESPACE::PackedImageDesc image(rgba.data(), width, height, 4);
    cpu->apply(image);
#endif
    auto to_byte = [](const float v) -> unsigned char {
      return static_cast<unsigned char>(
          (v <= 0.0f) ? 0 : ((v > (1.0f - 0.5f / 255.0f)) ? 255 : ((255.0f * v) + 0.5f)));
    };
    vector<unsigned char> out(count * 4);
    for (int y = 0; y < height; y++) {
      const uint32_t by = uint32_t(height - 1 - y); /* Blender's image rows are bottom-up */
      for (int x = 0; x < width; x++) {
        const size_t i = (size_t(y) * width + x) * 4;
        const uint32_t qx = 1103515245u * ((uint32_t(x) >> 1u) ^ by);
        const uint32_t qy = 1103515245u * ((by >> 1u) ^ uint32_t(x));
        const uint32_t n = 1103515245u * (qx ^ (qy >> 3u));
        float v = float(n) * (1.0f / float(0xffffffffu)) * 2.0f - 1.0f;
        v = (v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f)) * (1.0f - std::sqrt(1.0f - std::fabs(v)));
        const float d = dither != 0.0f ? v * 0.0033f * dither : 0.0f;
        out[i + 0] = to_byte(rgba[i + 0] + d);
        out[i + 1] = to_byte(rgba[i + 1] + d);
        out[i + 2] = to_byte(rgba[i + 2] + d);
        out[i + 3] = to_byte(rgba[i + 3]);
      }
    }
    return out;
  }
};

bool is_display_image(const string &path)
{
  const string lower = string_to_lower(path);
  return string_endswith(lower, ".png") || string_endswith(lower, ".jpg") || string_endswith(lower, ".jpeg");
}

class LayerOutputDriver : public OutputDriver {
 public:
  LayerOutputDriver(string filepath,
                    vector<Layer> layers,
                    bool multilayer,
                    vector<std::pair<string, string>> meta,
                    const DisplayImage *display = nullptr)
      : filepath_(std::move(filepath)),
        layers_(std::move(layers)),
        multilayer_(multilayer),
        meta_(std::move(meta)),
        display_(display)
  {
  }

  void write_render_tile(const Tile &tile) override
  {
    if (!(tile.size == tile.full_size)) {
      return;
    }
    const int width = tile.size.x, height = tile.size.y;
    int channels = 0;
    vector<string> names;
    for (const Layer &layer : layers_) {
      for (const char c : layer.channels) {
        names.push_back(multilayer_ ? string_printf("%s.%s.%c", kViewLayer, layer.exr.c_str(), c) :
                                      string(1, c));
      }
      channels += int(layer.channels.size());
    }
    vector<float> pixels(size_t(width) * height * channels, 0.0f);
    int offset = 0;
    for (const Layer &layer : layers_) {
      const int count = int(layer.channels.size());
      vector<float> buffer(size_t(width) * height * count);
      if (!tile.get_pass_pixels(layer.pass, count, buffer.data())) {
        printf("CYCLES_USD_RENDER_NOTE no pixels for pass %s\n", layer.pass.c_str());
      }
      /* Cycles' buffers are bottom-up, images top-down. */
      for (int y = 0; y < height; ++y) {
        const float *src = buffer.data() + size_t(height - 1 - y) * width * count;
        float *dst = pixels.data() + size_t(y) * width * channels + offset;
        for (int x = 0; x < width; ++x) {
          std::copy_n(src + size_t(x) * count, count, dst + size_t(x) * channels);
        }
      }
      offset += count;
    }

    OIIO::ImageSpec spec(width, height, channels, OIIO::TypeDesc::FLOAT);
    spec.channelnames.assign(names.begin(), names.end());
    spec.attribute("compression", "zip");
    for (const auto &[key, value] : meta_) {
      spec.attribute(key, value);
    }
    vector<unsigned char> bytes;
    if (display_) {
      /* The beauty only (layers_[0], RGBA), as Blender saves a PNG / JPEG: alpha kept in a PNG. */
      bytes = display_->bytes(pixels, width, height);
      const bool jpeg = !string_endswith(string_to_lower(filepath_), ".png");
      if (jpeg) {
        for (size_t i = 0, count = size_t(width) * height; i < count; i++) {
          std::copy_n(&bytes[i * 4], 3, &bytes[i * 3]);
        }
        bytes.resize(size_t(width) * height * 3);
      }
      spec = OIIO::ImageSpec(width, height, jpeg ? 3 : 4, OIIO::TypeDesc::UINT8);
      /* Straight alpha, as Blender stores a PNG: without this OIIO takes the pixels as premultiplied and divides
       * them by alpha again (edges came out too bright). */
      spec.attribute("oiio:UnassociatedAlpha", 1);
      if (jpeg) {
        spec.attribute("Compression", string_printf("jpeg:%d", display_->quality));
      }
    }
    const std::filesystem::path final_path(filepath_);
    const std::filesystem::path partial = final_path.parent_path() /
                                          ("." + final_path.stem().string() + ".partial" +
                                           final_path.extension().string());
    std::error_code error;
    std::filesystem::create_directories(final_path.parent_path(), error);
    unique_ptr<OIIO::ImageOutput> out(OIIO::ImageOutput::create(partial.string()));
    if (!out || !out->open(partial.string(), spec) ||
        !(display_ ? out->write_image(OIIO::TypeDesc::UINT8, bytes.data()) :
                     out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) ||
        !out->close())
    {
      printf("CYCLES_USD_RENDER_ERROR cannot write %s\n", filepath_.c_str());
      written_ = false;
      return;
    }
    std::filesystem::rename(partial, final_path, error);
    written_ = !error;
  }

  bool written() const
  {
    return written_;
  }

 private:
  string filepath_;
  vector<Layer> layers_;
  bool multilayer_;
  vector<std::pair<string, string>> meta_;
  const DisplayImage *display_;
  bool written_ = false;
};

/* The stage's RenderVars' source names (USD Nodes' Render Settings / Cycles AOV names), in product order. */
vector<string> stage_aovs(const UsdStageRefPtr &stage, const UsdRenderSettings &settings)
{
  vector<string> names;
  if (!settings) {
    return names;
  }
  SdfPathVector products;
  settings.GetProductsRel().GetForwardedTargets(&products);
  for (const SdfPath &productPath : products) {
    const UsdRenderProduct product(stage->GetPrimAtPath(productPath));
    SdfPathVector vars;
    if (product) {
      product.GetOrderedVarsRel().GetForwardedTargets(&vars);
    }
    for (const SdfPath &varPath : vars) {
      const UsdRenderVar var(stage->GetPrimAtPath(varPath));
      std::string source;
      if (var && !var.GetSourceNameAttr().Get(&source)) {
        source = varPath.GetName();
      }
      if (var && std::find(names.begin(), names.end(), source) == names.end()) {
        names.push_back(source);
      }
    }
  }
  return names;
}

string cryptomatte_key(const string &layer, const char *key)
{
  const uint32_t hash = util_murmur_hash3(layer.c_str(), int(layer.size()), 0);
  return string_printf("cryptomatte/%s/%s", string_printf("%08x", hash).substr(0, 7).c_str(), key);
}

struct Options {
  string usd;
  string image;
  string camera;
  string device = "CPU";
  vector<double> frames;
  int width = 0;
  int height = 0;
  int samples = 0;
  int threads = 0;
  string bvh = "auto";
  string texture_cache;      /* "", off, on (use .tx files), auto (and make missing ones) */
  string texture_cache_path; /* "" → ~/.cache/cycles_usd_render/tx */
  int texture_limit = -1;    /* pixels, 0 = none; -1 → the stage's cycles:texture_limit */
  bool skip_existing = false;
  bool prepare_textures = false;
  bool stats = false;
  bool cycles_defaults = false;
  DisplayImage display; /* for .png / .jpg images */
};

long peak_mb()
{
  rusage usage;
  getrusage(RUSAGE_SELF, &usage);
  return usage.ru_maxrss / 1024;
}

/* "image.####.exr" → "image.0012.exr". */
string frame_path(const string &pattern, const double frame)
{
  const size_t start = pattern.find('#');
  if (start == string::npos) {
    return pattern;
  }
  size_t end = start;
  while (end < pattern.size() && pattern[end] == '#') {
    end++;
  }
  return pattern.substr(0, start) +
         string_printf("%0*d", int(end - start), int(std::lround(frame))) + pattern.substr(end);
}

/* "1-24", "1,5,9" or "12". */
vector<double> parse_frames(const string &text)
{
  vector<double> frames;
  vector<string> parts;
  string_split(parts, text, ",");
  for (const string &part : parts) {
    const size_t dash = part.find('-', 1);
    if (dash != string::npos) {
      for (int f = std::stoi(part.substr(0, dash)); f <= std::stoi(part.substr(dash + 1)); f++) {
        frames.push_back(f);
      }
    }
    else if (!part.empty()) {
      frames.push_back(std::stod(part));
    }
  }
  return frames;
}

[[noreturn]] void fail(const string &message)
{
  printf("CYCLES_USD_RENDER_ERROR %s\n", message.c_str());
  fflush(stdout);
  exit(2);
}

void usage()
{
  printf(
      "usage: cycles_usd_render <file.usd> <image (#### for the frame)> [--frames 1-24] [--camera /path]\n"
      "         [--resolution 1280x720] [--samples N] [--device CPU|CUDA] [--threads N] [--bvh auto|static|dynamic]\n"
      "         [--texture-cache off|on|auto] [--texture-cache-path DIR] [--texture-limit PIXELS]\n"
      "         [--skip-existing] [--prepare-textures] [--stats] [--cycles-defaults]\n"
      "         [--display sRGB] [--view AgX] [--look None] [--exposure 0] [--gamma 1] [--dither 1] [--quality 90]\n"
      "  image               .exr: linear, with the stage's passes; .png / .jpg: the beauty through Blender's colour\n"
      "                      management (--display / --view / --look / --exposure / --gamma, as Render > Color\n"
      "                      Management; --dither as Render > Output > Dither; --quality for JPEG)\n"
      "  --skip-existing     frames whose image exists are not rendered again (resumable sequences, farm retries)\n"
      "  --prepare-textures  make the .tx texture cache for the first frame and exit (before a farm render)\n"
      "  --stats             print Cycles' scene update times per frame (where the time goes)\n"
      "  --cycles-defaults   Cycles' own integrator defaults, not Blender's (bounces, filter glossy, adaptive)\n");
  exit(1);
}

Options parse(int argc, const char **argv)
{
  Options o;
  vector<string> positional;
  for (int i = 1; i < argc; i++) {
    const string arg = argv[i];
    auto value = [&]() -> string {
      if (i + 1 >= argc) {
        usage();
      }
      return argv[++i];
    };
    if (arg == "--frames") {
      o.frames = parse_frames(value());
    }
    else if (arg == "--camera") {
      o.camera = value();
    }
    else if (arg == "--resolution") {
      const string size = value();
      const size_t x = size.find('x');
      if (x == string::npos) {
        usage();
      }
      o.width = std::stoi(size.substr(0, x));
      o.height = std::stoi(size.substr(x + 1));
    }
    else if (arg == "--samples") {
      o.samples = std::stoi(value());
    }
    else if (arg == "--device") {
      o.device = value();
    }
    else if (arg == "--threads") {
      o.threads = std::stoi(value());
    }
    else if (arg == "--bvh") {
      o.bvh = value();
    }
    else if (arg == "--texture-cache") {
      o.texture_cache = value();
    }
    else if (arg == "--texture-cache-path") {
      o.texture_cache_path = value();
    }
    else if (arg == "--texture-limit") {
      o.texture_limit = std::stoi(value());
    }
    else if (arg == "--skip-existing") {
      o.skip_existing = true;
    }
    else if (arg == "--prepare-textures") {
      o.prepare_textures = true;
    }
    else if (arg == "--stats") {
      o.stats = true;
    }
    else if (arg == "--cycles-defaults") {
      o.cycles_defaults = true;
    }
    else if (arg == "--display") {
      o.display.display = value();
    }
    else if (arg == "--view") {
      o.display.view = value();
    }
    else if (arg == "--look") {
      o.display.look = value();
    }
    else if (arg == "--exposure") {
      o.display.exposure = std::stof(value());
    }
    else if (arg == "--gamma") {
      o.display.gamma = std::stof(value());
    }
    else if (arg == "--dither") {
      o.display.dither = std::stof(value());
    }
    else if (arg == "--quality") {
      o.display.quality = std::stoi(value());
    }
    else if (arg == "-h" || arg == "--help") {
      usage();
    }
    else {
      positional.push_back(arg);
    }
  }
  if (positional.size() != 2) {
    usage();
  }
  o.usd = positional[0];
  o.image = positional[1];
  return o;
}

DeviceInfo choose_device(const string &name)
{
  const DeviceTypeMask mask = name == "CUDA" ? DEVICE_MASK_CUDA : DEVICE_MASK_CPU;
  const vector<DeviceInfo> devices = Device::available_devices(mask);
  if (devices.empty()) {
    fail("no " + name + " device");
  }
  return devices.size() == 1 ? devices.front() : Device::get_multi_device(devices, 0, true);
}

/* The camera: --camera, else the render settings' camera, else the first one in the stage. */
SdfPath choose_camera(const UsdStageRefPtr &stage, const UsdRenderSettings &settings, const string &wanted)
{
  if (!wanted.empty()) {
    return SdfPath(wanted);
  }
  if (settings) {
    SdfPathVector targets;
    settings.GetCameraRel().GetForwardedTargets(&targets);
    if (!targets.empty()) {
      return targets.front();
    }
  }
  for (const UsdPrim &prim : stage->Traverse()) {
    if (prim.IsA<UsdGeomCamera>()) {
      return prim.GetPath();
    }
  }
  fail("no camera in the stage");
}

/* The executable sits in <plugin>/bin: Cycles' resources (kernels) in <plugin>/hdCycles/resources, USD's plugins in
 * <plugin>/usd and <plugin>/usd_plugins. */
std::filesystem::path plugin_folder()
{
  char buffer[4096];
  const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length <= 0) {
    fail("cannot find the executable's folder");
  }
  buffer[length] = '\0';
  return std::filesystem::path(buffer).parent_path().parent_path();
}

int render(const Options &o)
{
  const double started = time_dt();
#ifdef WITH_OPENVDB
  /* OpenVDB's grid types must be registered before reading VDB files (Blender does this at startup; the delegate as
   * a plugin inherits it, this program does not): without it every volume fails to load. */
  openvdb::initialize();
#endif
  if (const char *level = getenv("CYCLES_USD_RENDER_LOG")) {
    log_level_set(level); /* Cycles' own log: "info", "debug" … */
  }
  const std::filesystem::path plugin = plugin_folder();
  path_init((plugin / "hdCycles" / "resources").string());
  const std::filesystem::path ocio = plugin / "colormanagement" / "config.ocio";
  if (!getenv("OCIO") && std::filesystem::exists(ocio)) {
    /* Blender's colour management (sRGB, Linear Rec.709, AgX …): texture colour spaces resolve as in Blender. */
    setenv("OCIO", ocio.c_str(), 1);
  }
  PlugRegistry::GetInstance().RegisterPlugins(
      std::vector<std::string>{(plugin / "usd").string(), (plugin / "usd_plugins").string()});
  const bool display_image = is_display_image(o.image);
  DisplayImage display = o.display;
  if (display_image) {
    if (const string problem = display.prepare(); !problem.empty()) {
      fail(problem);
    }
  }

  const UsdStageRefPtr stage = UsdStage::Open(o.usd);
  if (!stage) {
    fail("cannot open " + o.usd);
  }
  const UsdRenderSettings settings = UsdRenderSettings::GetStageRenderSettings(stage);
  vector<double> frames = o.frames;
  if (frames.empty()) {
    frames.push_back(stage->HasAuthoredTimeCodeRange() ? stage->GetStartTimeCode() : 1.0);
  }

  SessionParams session_params;
  session_params.background = true;
  session_params.device = choose_device(o.device);
  session_params.threads = o.threads;
  session_params.samples = o.samples > 0 ? o.samples : 64;
  SceneParams scene_params;
  /* Textures: tiled, mipmapped .tx files load only the tiles and mip levels the render needs. "auto" (default) makes
   * missing ones once, into a cache folder (not next to the textures); the stage's cycles:texture_cache and
   * cycles:texture_limit apply unless given here. */
  auto stage_setting = [&](const char *name) -> VtValue {
    VtValue value;
    if (settings) {
      const UsdAttribute attribute = settings.GetPrim().GetAttribute(TfToken(name));
      if (attribute && attribute.HasAuthoredValue()) {
        attribute.Get(&value);
      }
    }
    return value;
  };
  string texture_cache = o.texture_cache;
  if (texture_cache.empty()) {
    const VtValue value = stage_setting("cycles:texture_cache");
    texture_cache = value.IsHolding<TfToken>() ? value.UncheckedGet<TfToken>().GetString() :
                    value.IsHolding<std::string>() ? value.UncheckedGet<std::string>() :
                                                     "auto";
  }
  if (texture_cache != "off" && texture_cache != "on" && texture_cache != "auto") {
    fail("--texture-cache must be off, on or auto");
  }
  scene_params.use_texture_cache = texture_cache != "off";
  scene_params.auto_texture_cache = texture_cache == "auto";
  scene_params.texture_cache_path = o.texture_cache_path;
  if (scene_params.texture_cache_path.empty()) {
    const char *home = getenv("XDG_CACHE_HOME");
    scene_params.texture_cache_path = home ? string(home) + "/cycles_usd_render/tx" :
                                             string(getenv("HOME") ? getenv("HOME") : "/tmp") +
                                                 "/.cache/cycles_usd_render/tx";
  }
  if (scene_params.auto_texture_cache) {
    std::error_code error;
    std::filesystem::create_directories(scene_params.texture_cache_path, error);
  }
  int texture_limit = o.texture_limit;
  if (texture_limit < 0) {
    const VtValue value = stage_setting("cycles:texture_limit");
    texture_limit = value.IsHolding<int>() ? value.UncheckedGet<int>() : 0;
  }
  scene_params.texture_limit = std::max(0, texture_limit);
  scene_params.texture_resolution = 1.0f;

  /* Static BVH by default, sequences too: Embree's medium / high build quality traces faster than the dynamic one's
   * low quality (M: ~20 %, L: ~8 % a frame with the rebuild). A static BVH bakes single-user transforms into the
   * geometry; the delegate restores it when the object moves (hdcycles-blender patch 0015). --bvh dynamic: refits
   * instead, for scenes where most objects move. */
  const bool dynamic = o.bvh == "dynamic";
  scene_params.bvh_type = dynamic ? BVH_TYPE_DYNAMIC : BVH_TYPE_STATIC;
  unique_ptr<Session> session = make_unique<Session>(session_params, scene_params);
  Scene *scene = session->scene.get();

  HdRenderSettingsMap settings_map;
  settings_map.insert(std::make_pair(HdCyclesRenderSettingsTokens->stageMetersPerUnit,
                                     VtValue(UsdGeomGetStageMetersPerUnit(stage))));
  HdCyclesDelegate delegate(settings_map, session.get(), true);
  /* As the Blender runner's delegate, which owns its session: the same surface for geometry without a material. */
  HdCyclesSession::SetDefaultSurface(session->scene.get());
  /* Nothing renders while Hydra syncs here: rprims fill their geometry in parallel (HdCyclesSession). */
  static_cast<HdCyclesSession *>(delegate.GetRenderParam())->SetExclusiveSync(true);
  /* No Hydra instanceId AOV here: no per-instance attribute (memory, attribute repacks on every change). */
  static_cast<HdCyclesSession *>(delegate.GetRenderParam())->SetInstanceIdAov(false);
  /* Blender's defaults for what the stage does not set (Cycles' own differ: 7 bounces of every kind, no glossy
   * filter, no adaptive sampling), so a stage renders as it would in Blender: a volume with Cycles' 7 volume bounces
   * rendered ~10 % lighter than with Blender's 0. --cycles-defaults keeps Cycles'. */
  if (!o.cycles_defaults) {
    const std::pair<const char *, VtValue> blender_defaults[] = {
        {"cycles:integrator:max_bounce", VtValue(12)},
        {"cycles:integrator:max_diffuse_bounce", VtValue(4)},
        {"cycles:integrator:max_glossy_bounce", VtValue(4)},
        {"cycles:integrator:max_transmission_bounce", VtValue(12)},
        {"cycles:integrator:max_volume_bounce", VtValue(0)},
        {"cycles:integrator:transparent_max_bounce", VtValue(8)},
        {"cycles:integrator:sample_clamp_indirect", VtValue(10.0f)},
        {"cycles:integrator:filter_glossy", VtValue(1.0f)},
        {"cycles:integrator:use_adaptive_sampling", VtValue(true)},
        {"cycles:integrator:adaptive_threshold", VtValue(0.01f)},
        /* A dome that does not say renderlightgeo is seen by the camera, as Blender's World (patch 0027). */
        {"cycles:background:camera_visible_default", VtValue(true)},
    };
    for (const auto &[name, value] : blender_defaults) {
      delegate.SetRenderSetting(TfToken(name), value);
    }
  }
  /* The stage's cycles:* settings (USD Nodes' Cycles Render Settings node) win over --samples, as in Blender. */
  if (settings) {
    for (const UsdAttribute &attribute : settings.GetPrim().GetAttributes()) {
      VtValue value;
      if (TfStringStartsWith(attribute.GetName().GetString(), "cycles:") && attribute.HasAuthoredValue() &&
          attribute.Get(&value))
      {
        delegate.SetRenderSetting(attribute.GetName(), value);
      }
    }
  }
  session_params.samples = session->params.samples;

  /* Passes: the stage's RenderVars (USD Nodes' Render Settings), through the delegate's AOV mapping (patch 0004) — a
   * Blender-style multilayer EXR; without them the beauty as plain RGBA. */
  vector<Layer> layers;
  bool multilayer = false;
  vector<string> aovs = stage_aovs(stage, settings);
  const bool beauty_only = aovs.empty() || (aovs.size() == 1 && aovs[0] == "color");
  if (display_image && !beauty_only) {
    /* As Blender saves a PNG / JPEG: the combined pass only. */
    printf("CYCLES_USD_RENDER_NOTE the passes need an .exr image: %s gets the beauty only\n", o.image.c_str());
  }
  if (beauty_only || display_image) {
    Pass *pass = scene->create_node<Pass>();
    pass->set_name(ustring("combined"));
    pass->set_type(PASS_COMBINED);
    layers.push_back({"combined", "Combined", "RGBA"});
  }
  else {
    if (std::find(aovs.begin(), aovs.end(), "color") == aovs.end()) {
      aovs.insert(aovs.begin(), "color");
    }
    HdRenderPassAovBindingVector bindings;
    for (const string &aov : aovs) {
      Layer layer{aov, "", ""};
      if (const auto known = kLayers.find(aov); known != kLayers.end()) {
        layer.exr = known->second.pass;
        layer.channels = known->second.channels;
      }
      else if (string_startswith(aov, "lightgroup_")) {
        layer.exr = "Combined_" + aov.substr(strlen("lightgroup_"));
        layer.channels = "RGBA";
      }
      else if (string_startswith(aov, "Crypto")) {
        layer.exr = aov;
        layer.channels = "rgba"; /* Cryptomatte's convention, as Blender writes it */
      }
      else {
        printf("CYCLES_USD_RENDER_NOTE no Cycles pass for AOV '%s'\n", aov.c_str());
        continue;
      }
      HdRenderPassAovBinding binding;
      binding.aovName = TfToken(aov);
      bindings.push_back(binding);
      layers.push_back(layer);
    }
    static_cast<HdCyclesSession *>(delegate.GetRenderParam())->SyncAovBindings(bindings);
    multilayer = true;
    if (!string_endswith(string_to_lower(o.image), ".exr")) {
      fail("the render settings ask for passes: the image must be .exr (a multilayer EXR)");
    }
  }
  /* Motion blur follows the render settings (UsdRender's disableMotionBlur) and the camera's shutter. */
  if (settings) {
    bool disable = false;
    settings.GetDisableMotionBlurAttr().Get(&disable);
    delegate.SetRenderSetting(TfToken("cycles:motion_blur"), VtValue(!disable));
  }

  int width = o.width, height = o.height;
  if ((width <= 0 || height <= 0) && settings) {
    GfVec2i resolution(1280, 720);
    settings.GetResolutionAttr().Get(&resolution);
    width = resolution[0];
    height = resolution[1];
  }
  if (width <= 0 || height <= 0) {
    width = 1280;
    height = 720;
  }

  const unique_ptr<HdRenderIndex> render_index(HdRenderIndex::New(&delegate, {}));
  /* USD's standard chain (native and point instancing, draw modes, flattening), then the renderer's scene index
   * plugins, resolved material bindings and implicit shapes as meshes (the delegate draws meshes only). */
  UsdImagingCreateSceneIndicesInfo create_info;
  create_info.stage = stage;
  const UsdImagingSceneIndices scene_indices = UsdImagingCreateSceneIndices(create_info);
  UsdImagingStageSceneIndexRefPtr stage_index = scene_indices.stageSceneIndex;
  if (scene_indices.postInstancingNoticeBatchingSceneIndex) {
    /* Pass every change on at once: a frame's time change must reach the render index before it syncs. */
    scene_indices.postInstancingNoticeBatchingSceneIndex->SetBatchingEnabled(false);
  }
  stage_index->SetTime(UsdTimeCode(frames.front()));
  HdSceneIndexBaseRefPtr filtered = scene_indices.finalSceneIndex;
  /* velocities / accelerations → motion of points and instances over the shutter (UsdGeom's rules). */
  filtered = HdsiVelocityMotionResolvingSceneIndex::New(
      filtered,
      HdRetainedContainerDataSource::New(
          HdsiVelocityMotionResolvingSceneIndexTokens->timeCodesPerSecond,
          HdRetainedTypedSampledDataSource<double>::New(stage->GetTimeCodesPerSecond())));
  filtered = HdSceneIndexPluginRegistry::GetInstance().AppendSceneIndicesForRenderer("Cycles", filtered);
  filtered = HdSiExtComputationPrimvarPruningSceneIndex::New(filtered);
  filtered = UsdImagingMaterialBindingsResolvingSceneIndex::New(filtered, nullptr);
  const HdSceneIndexBaseRefPtr shapes = filtered;
  {
    const HdDataSourceBaseHandle to_mesh = HdRetainedTypedSampledDataSource<TfToken>::New(
        HdsiImplicitSurfaceSceneIndexTokens->toMesh);
    filtered = HdsiImplicitSurfaceSceneIndex::New(
        filtered,
        HdRetainedContainerDataSource::New(HdPrimTypeTokens->sphere,
                                           to_mesh,
                                           HdPrimTypeTokens->cube,
                                           to_mesh,
                                           HdPrimTypeTokens->cone,
                                           to_mesh,
                                           HdPrimTypeTokens->cylinder,
                                           to_mesh,
                                           HdPrimTypeTokens->capsule,
                                           to_mesh,
                                           HdPrimTypeTokens->plane,
                                           to_mesh));
  }
  filtered = CubeNormalsSceneIndex::New(filtered, shapes);

  const SdfPath root = SdfPath::AbsoluteRootPath();
  const SdfPath task_path("/_cyclesUsdRender/tags");
  HdRprimCollection collection(HdTokens->geometry, HdReprSelector(HdReprTokens->smoothHull));
  collection.SetRootPath(root);
  const TfTokenVector render_tags = {HdRenderTagTokens->geometry, HdRenderTagTokens->render};
  HdContainerDataSourceHandle task_source =
      HdLegacyTaskSchema::Builder()
          .SetFactory(HdRetainedTypedSampledDataSource<HdLegacyTaskFactorySharedPtr>::New(
              HdMakeLegacyTaskFactory<TagsTask>()))
          .SetCollection(HdRetainedTypedSampledDataSource<HdRprimCollection>::New(collection))
          .SetRenderTags(HdRetainedTypedSampledDataSource<TfTokenVector>::New(render_tags))
          .Build();
  HdRetainedSceneIndexRefPtr task_index = HdRetainedSceneIndex::New();
  task_index->AddPrims({{task_path,
                         HdPrimTypeTokens->task,
                         HdRetainedContainerDataSource::New(HdLegacyTaskSchema::GetSchemaToken(),
                                                            task_source)}});
  HdMergingSceneIndexRefPtr merging = HdMergingSceneIndex::New();
  merging->AddInputScene(filtered, root);
  merging->AddInputScene(task_index, root);
  render_index->InsertSceneIndex(merging, root, false);

  HdTaskContext task_context;
  HdTaskSharedPtrVector tasks;
  if (const HdTaskSharedPtr &task = render_index->GetTask(task_path)) {
    tasks.push_back(task);
  }
  const SdfPath camera_path = choose_camera(stage, settings, o.camera);

  BufferParams buffer_params;
  buffer_params.width = buffer_params.full_width = width;
  buffer_params.height = buffer_params.full_height = height;
  printf("CYCLES_USD_RENDER_TIME load %.3f\n", time_dt() - started);
  fflush(stdout);

  for (const double frame : frames) {
    const double frame_started = time_dt();
    if (o.skip_existing && !o.prepare_textures && std::filesystem::exists(frame_path(o.image, frame))) {
      printf("CYCLES_USD_RENDER_SKIP frame %g (%s exists)\n", frame, frame_path(o.image, frame).c_str());
      fflush(stdout);
      continue;
    }
    stage_index->SetTime(UsdTimeCode(frame));
    stage_index->ApplyPendingUpdates();
    /* SyncAll syncs the dirty prims of the collections enqueued since the last sync, then forgets them (a render
     * task enqueues its collection every frame): enqueue it every frame, or later frames sync nothing. */
    render_index->EnqueueCollectionToSync(collection);
    render_index->SyncAll(&tasks, &task_context);
    delegate.CommitResources(&render_index->GetChangeTracker());

    const HdCyclesCamera *camera = dynamic_cast<const HdCyclesCamera *>(
        render_index->GetSprim(HdPrimTypeTokens->camera, camera_path));
    if (!camera) {
      fail("no camera " + camera_path.GetString());
    }
    scene->camera->set_full_width(width);
    scene->camera->set_full_height(height);
    camera->ApplyCameraSettings(delegate.GetRenderParam(), scene->camera);
    const double synced = time_dt() - frame_started;

    if (o.prepare_textures) {
      /* Load every texture once (Cycles makes the missing .tx files then) on a tiny image; write nothing. */
      BufferParams tiny;
      tiny.width = tiny.full_width = tiny.height = tiny.full_height = 8;
      SessionParams one = session_params;
      one.samples = 1;
      session->set_output_driver(nullptr);
      session->reset(one, tiny);
      session->start();
      session->wait();
      printf("CYCLES_USD_RENDER_PREPARED textures in %.3f s\n", time_dt() - frame_started);
      fflush(stdout);
      return 0;
    }

    /* Cryptomatte manifests: object / material / asset names → ids, as Blender stores them in the EXR header. */
    vector<std::pair<string, string>> meta;
    const std::pair<const char *, int> crypto_types[] = {{"CryptoObject", 0}, {"CryptoMaterial", 1}, {"CryptoAsset", 2}};
    for (const auto &[prefix, kind] : crypto_types) {
      if (std::none_of(layers.begin(), layers.end(),
                       [prefix](const Layer &layer) { return string_startswith(layer.pass, prefix); }))
      {
        continue;
      }
      const string layer = string(kViewLayer) + "." + prefix;
      const string manifest = kind == 0 ? scene->object_manager->get_cryptomatte_objects(scene) :
                              kind == 1 ? scene->shader_manager->get_cryptomatte_materials(scene) :
                                          scene->object_manager->get_cryptomatte_assets(scene);
      meta.emplace_back(cryptomatte_key(layer, "name"), layer);
      meta.emplace_back(cryptomatte_key(layer, "hash"), "MurmurHash3_32");
      meta.emplace_back(cryptomatte_key(layer, "conversion"), "uint32_to_float32");
      meta.emplace_back(cryptomatte_key(layer, "manifest"), manifest);
    }

    const string image = frame_path(o.image, frame);
    auto driver = make_unique<LayerOutputDriver>(
        image, layers, multilayer, std::move(meta), display_image ? &display : nullptr);
    const LayerOutputDriver *writer = driver.get();
    session->set_output_driver(std::move(driver));
    if (o.stats) {
      scene->enable_update_stats();
    }
    session->reset(session_params, buffer_params);
    session->start();
    session->wait();
    if (o.stats && scene->update_stats) {
      printf("CYCLES_USD_RENDER_STATS frame %g\n%s\n", frame, scene->update_stats->full_report().c_str());
      scene->update_stats->clear();
    }
    if (!writer->written()) {
      fail("the image was not written: " + image);
    }
    if (session->progress.get_cancel()) {
      fail("render cancelled: " + session->progress.get_cancel_message());
    }
    if (!session->progress.get_error_message().empty()) {
      fail(session->progress.get_error_message());
    }
    printf("CYCLES_USD_RENDER_TIME frame %g %.3f %.3f %ld\n",
           frame,
           synced,
           time_dt() - frame_started,
           peak_mb());
    printf("CYCLES_USD_RENDER_MEMORY frame %g device_mb %zu device_peak_mb %zu\n",
           frame,
           session->device->stats.mem_used >> 20,
           session->device->stats.mem_peak >> 20);
    printf("CYCLES_USD_RENDER_OK %s\n", image.c_str());
    fflush(stdout);
  }
  /* Destroyed in reverse order: the render index (its prims leave the scene), the delegate, then the session. */
  return 0;
}

}  // namespace

HDCYCLES_NAMESPACE_CLOSE_SCOPE

int main(int argc, const char **argv)
{
  return HD_CYCLES_NS::render(HD_CYCLES_NS::parse(argc, argv));
}
