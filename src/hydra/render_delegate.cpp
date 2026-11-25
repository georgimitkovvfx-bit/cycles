/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/render_delegate.h"
#include "hydra/camera.h"
#include "hydra/curves.h"
#include "hydra/cylinder_light.h"
#include "hydra/field.h"
#ifdef WITH_HOUDINI_IMAGES
#  include "hydra/houdini_image.h"
#endif
#include "hydra/instancer.h"
#include "hydra/light.h"
#include "hydra/material.h"
#ifdef WITH_HYDRA_MATERIALX
#  include "hydra/materialx.h"
#endif
#include "hydra/mesh.h"
#include "hydra/node_util.h"
#include "hydra/pointcloud.h"
#include "hydra/render_buffer.h"
#include "hydra/render_pass.h"
#include "hydra/render_settings.h"
#include "hydra/session.h"
#include "hydra/volume.h"
#include "integrator/denoiser.h"
#include "scene/background.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "scene/object.h"
#include "scene/osl.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "util/log.h"
#include "util/murmurhash.h"
#include "util/path.h"
#include "util/string.h"
#include "util/time.h"
#include "util/version.h"
#include "session/session.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>

#include <pxr/base/tf/getenv.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/volumeFieldBindingSchema.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/imaging/hgi/tokens.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(HdCyclesRenderSettingsTokens, HD_CYCLES_RENDER_SETTINGS_TOKENS);

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (cycles)
    (openvdbAsset)
    (houdiniFieldAsset)
    (reloadTextures)
    ((houdiniCopTextureChanged, "houdini:cop_texture_changed"))
);
// clang-format on

namespace {

const TfTokenVector kSupportedRPrimTypes = {
    HdPrimTypeTokens->basisCurves,
    HdPrimTypeTokens->mesh,
    HdPrimTypeTokens->points,
    HdPrimTypeTokens->particleField,
#ifdef WITH_OPENVDB
    HdPrimTypeTokens->volume,
#endif
};

const TfTokenVector kSupportedSPrimTypes = {
    HdPrimTypeTokens->camera,
    /* Hydra syncs sprims in this order: lights before materials, so that a world material
     * knows its dome light (HdCyclesSession::SetWorldMaterial) on the first sync. */
    HdPrimTypeTokens->cylinderLight,
    HdPrimTypeTokens->diskLight,
    HdPrimTypeTokens->distantLight,
    HdPrimTypeTokens->domeLight,
    HdPrimTypeTokens->rectLight,
    HdPrimTypeTokens->sphereLight,
    HdPrimTypeTokens->material,
};

const TfTokenVector kSupportedBPrimTypes = {
    HdPrimTypeTokens->renderBuffer,
    HdPrimTypeTokens->renderSettings,
#ifdef WITH_OPENVDB
    _tokens->openvdbAsset,
#endif
#ifdef WITH_HOUDINI_VOLUMES
    _tokens->houdiniFieldAsset,
#endif
};

// Houdini sends menu values as tokens, while other clients may use strings
std::string GetStringSetting(const VtValue &value, const std::string &defaultValue)
{
  if (value.IsHolding<TfToken>()) {
    return value.UncheckedGet<TfToken>().GetString();
  }
  return VtValue::Cast<std::string>(value).GetWithDefault(defaultValue);
}

// Scene nodes whose sockets are exposed as "cycles:<node>:<socket>" render settings
std::array<std::pair<std::string, Node *>, 3> GetSettingNodes(Scene *scene)
{
  return {{
      {"cycles:integrator:", scene->integrator},
      {"cycles:film:", scene->film},
      {"cycles:background:", scene->background},
  }};
}

Node *FindSettingNode(Scene *scene, const std::string &key, ustring &socketName)
{
  for (const auto &[prefix, node] : GetSettingNodes(scene)) {
    if (key.compare(0, prefix.size(), prefix) == 0) {
      socketName = ustring(key.c_str() + prefix.size());
      return node;
    }
  }
  return nullptr;
}

// Settings fixed when the session is created
bool IsSessionSetting(const TfToken &key)
{
  return key == HdCyclesRenderSettingsTokens->device ||
         key == HdCyclesRenderSettingsTokens->threads ||
         key == HdCyclesRenderSettingsTokens->useCpuToo ||
         key == HdCyclesRenderSettingsTokens->gpuDevices ||
         key == HdCyclesRenderSettingsTokens->shadingSystem ||
         key == HdCyclesRenderSettingsTokens->useResolutionDivider ||
         key == HdCyclesRenderSettingsTokens->curveShape ||
         key == HdCyclesRenderSettingsTokens->useTextureCache ||
         key == HdCyclesRenderSettingsTokens->autoTextureCache ||
         key == HdCyclesRenderSettingsTokens->textureCachePath ||
         key == HdCyclesRenderSettingsTokens->textureLimit ||
         key == HdCyclesRenderSettingsTokens->tileSize ||
         key == HdCyclesRenderSettingsTokens->pixelSize ||
         key == HdCyclesRenderSettingsTokens->useSpatialSplits ||
         key == HdCyclesRenderSettingsTokens->useCompactBvh ||
         key == HdCyclesRenderSettingsTokens->bvhType ||
         key == HdCyclesRenderSettingsTokens->curveSubdivisions ||
         key == HdCyclesRenderSettingsTokens->renderMode;
}

SessionParams GetSessionParams(const HdRenderSettingsMap &settings)
{
  SessionParams params;
  params.threads = 0;
  params.background = false;

  HdRenderSettingsMap::const_iterator it;

  // Pull all setting that contribute to device creation first
  it = settings.find(HdCyclesRenderSettingsTokens->threads);
  if (it != settings.end()) {
    params.threads = VtValue::Cast<int>(it->second).GetWithDefault(params.threads);
  }

  // Shading system is fixed when the scene is created, like the device
  it = settings.find(HdCyclesRenderSettingsTokens->shadingSystem);
  if (it != settings.end() && VtValue::Cast<bool>(it->second).GetWithDefault(false)) {
    params.shadingsystem = SHADINGSYSTEM_OSL;
  }

  // Like Blender, only final renders use tiles: the viewport shows every sample, a tiled render
  // reaches the render buffers when it is done (see HdCyclesSession::CreateSession).
  it = settings.find(HdCyclesRenderSettingsTokens->houdiniViewport);
  const bool viewport = it != settings.end() &&
                        VtValue::Cast<bool>(it->second).GetWithDefault(false);
  params.use_auto_tile = !viewport;
  /* husk renders (renderMode "batch") run as a background session, like Blender's F12 render:
   * no lower resolution start, rarer progressive updates, denoising only at the end. */
  it = settings.find(HdCyclesRenderSettingsTokens->renderMode);
  params.background = !viewport && it != settings.end() &&
                      GetStringSetting(it->second, "") == "batch";
  it = settings.find(HdCyclesRenderSettingsTokens->tileSize);
  if (it != settings.end()) {
    params.tile_size = std::clamp(
        VtValue::Cast<int>(it->second).GetWithDefault(params.tile_size), 8, 8192);
  }
  // Blender's viewport Pixel Size: final renders always use full resolution pixels
  it = settings.find(HdCyclesRenderSettingsTokens->pixelSize);
  if (viewport && it != settings.end()) {
    params.pixel_size = std::max(1, std::atoi(GetStringSetting(it->second, "1").c_str()));
  }
  params.temp_dir = std::filesystem::temp_directory_path().string();

  // Lower resolution start of renders after a change, fixed when the session is created
  it = settings.find(HdCyclesRenderSettingsTokens->useResolutionDivider);
  if (it != settings.end()) {
    params.use_resolution_divider = VtValue::Cast<bool>(it->second).GetWithDefault(true);
  }

  // Get the Cycles device from settings or environment, defaulting to the best available GPU
  std::string deviceType = "AUTO";
  it = settings.find(HdCyclesRenderSettingsTokens->device);
  if (it != settings.end()) {
    deviceType = GetStringSetting(it->second, deviceType);
  }
  else {
    const std::string deviceTypeEnv = TfGetenv("CYCLES_DEVICE");
    if (!deviceTypeEnv.empty()) {
      deviceType = deviceTypeEnv;
    }
  }

  // Move to all uppercase for Device::type_from_string
  std::transform(deviceType.begin(), deviceType.end(), deviceType.begin(), ::toupper);

  vector<DeviceInfo> devices;
  if (deviceType == "AUTO") {
    // Prefer OptiX over CUDA on NVIDIA, like the order of the Blender preferences
    for (const DeviceType type :
         {DEVICE_OPTIX, DEVICE_CUDA, DEVICE_HIP, DEVICE_METAL, DEVICE_ONEAPI})
    {
      for (const DeviceInfo &info : Device::available_devices(DEVICE_MASK(type))) {
        if (info.meets_driver_requirement) {
          devices.push_back(info);
        }
      }
      if (!devices.empty()) {
        break;
      }
    }
  }
  else {
    for (const DeviceInfo &info :
         Device::available_devices(DEVICE_MASK(Device::type_from_string(deviceType.c_str()))))
    {
      if (info.meets_driver_requirement) {
        devices.push_back(info);
      }
    }
  }
  /* GPU subset, like the device checkboxes in Blender's preferences: comma separated indices
   * into the GPUs of the chosen type or parts of their names or ids (PCI bus), empty or "*" for
   * all of them. */
  it = settings.find(HdCyclesRenderSettingsTokens->gpuDevices);
  const std::string gpuDevices = it != settings.end() ? GetStringSetting(it->second, "") : "";
  if (!devices.empty() && devices.front().type != DEVICE_CPU && !gpuDevices.empty() &&
      gpuDevices != "*")
  {
    vector<string> tokens;
    string_split(tokens, string_to_lower(gpuDevices), ",");
    vector<DeviceInfo> chosen;
    for (size_t i = 0; i < devices.size(); i++) {
      for (const string &part : tokens) {
        const string token = string_strip(part);
        const bool index = std::all_of(token.begin(), token.end(), [](const unsigned char c) {
          return std::isdigit(c) != 0;
        });
        if (index ? token == std::to_string(i) :
                    string_to_lower(devices[i].id + " " + devices[i].description).find(token) != string::npos)
        {
          chosen.push_back(devices[i]);
          break;
        }
      }
    }
    if (chosen.empty()) {
      TF_WARN("Cycles: no GPU matches cycles:gpu_devices \"%s\", using all of them",
              gpuDevices.c_str());
      /* husk hides TF warnings, the Cycles log reaches its output. */
      LOG_WARNING << "No GPU matches cycles:gpu_devices \"" << gpuDevices
                  << "\", using all of them";
    }
    else {
      devices = chosen;
    }
  }

  /* Blender's CPU checkbox next to the GPUs. get_multi_device leaves the CPU out of interactive
   * renders and frees a CPU thread for each GPU in background (husk) renders. */
  it = settings.find(HdCyclesRenderSettingsTokens->useCpuToo);
  if (!devices.empty() && devices.front().type != DEVICE_CPU && it != settings.end() &&
      VtValue::Cast<bool>(it->second).GetWithDefault(false))
  {
    for (const DeviceInfo &info : Device::available_devices(DEVICE_MASK_CPU)) {
      devices.push_back(info);
    }
  }

  if (devices.empty()) {
    devices = Device::available_devices(DEVICE_MASK_CPU);
    if (!devices.empty()) {
      params.device = devices.front();
    }
  }
  else {
    params.device = Device::get_multi_device(devices, params.threads, params.background);
  }

  /* Set same device for denoising for now. */
  params.denoise_device = params.device;

  return params;
}

SceneParams GetSceneParams(const HdRenderSettingsMap &settings, const SessionParams &sessionParams)
{
  SceneParams params;
  params.shadingsystem = sessionParams.shadingsystem;

  // Curve shape is fixed when the scene is created, like the shading system
  auto it = settings.find(HdCyclesRenderSettingsTokens->curveShape);
  if (it != settings.end() && GetStringSetting(it->second, "ribbon") == "thick") {
    params.hair_shape = CURVE_THICK;
  }

  // The image manager reads the texture cache settings when the scene is created
  it = settings.find(HdCyclesRenderSettingsTokens->useTextureCache);
  if (it != settings.end()) {
    params.use_texture_cache = VtValue::Cast<bool>(it->second).GetWithDefault(true);
  }
  it = settings.find(HdCyclesRenderSettingsTokens->autoTextureCache);
  if (it != settings.end()) {
    params.auto_texture_cache = VtValue::Cast<bool>(it->second).GetWithDefault(false);
  }
  it = settings.find(HdCyclesRenderSettingsTokens->textureCachePath);
  if (it != settings.end()) {
    params.texture_cache_path = GetStringSetting(it->second, "");
  }
  // Menu of sizes in pixels, "off" for no limit
  it = settings.find(HdCyclesRenderSettingsTokens->textureLimit);
  if (it != settings.end()) {
    params.texture_limit = std::max(0, std::atoi(GetStringSetting(it->second, "off").c_str()));
  }

  // Blender's Performance > Acceleration Structure and Curves > Curve Subdivisions
  it = settings.find(HdCyclesRenderSettingsTokens->useSpatialSplits);
  if (it != settings.end()) {
    params.use_bvh_spatial_split = VtValue::Cast<bool>(it->second).GetWithDefault(false);
  }
  it = settings.find(HdCyclesRenderSettingsTokens->useCompactBvh);
  if (it != settings.end()) {
    params.use_bvh_compact_structure = VtValue::Cast<bool>(it->second).GetWithDefault(true);
  }
  it = settings.find(HdCyclesRenderSettingsTokens->curveSubdivisions);
  if (it != settings.end()) {
    params.hair_subdivisions = std::clamp(
        VtValue::Cast<int>(it->second).GetWithDefault(params.hair_subdivisions), 0, 24);
  }
  /* Blender's Acceleration Structure: static (Embree's better build, single-user transforms baked into their
   * geometry; HdCyclesGeometry::Sync restores them when they change) for background renders (husk), as Blender's
   * F12; dynamic (refits) for the viewport. Blender's STATIC_BVH / DYNAMIC_BVH, or static / dynamic. */
  it = settings.find(HdCyclesRenderSettingsTokens->bvhType);
  const std::string bvhType = TfStringToLower(it != settings.end() ? GetStringSetting(it->second, "") : "");
  const bool staticBvh = bvhType.empty() ? sessionParams.background : bvhType.rfind("static", 0) == 0;
  params.bvh_type = staticBvh ? BVH_TYPE_STATIC : BVH_TYPE_DYNAMIC;

  return params;
}

/* Image files of the scene's used shaders (materials, lights, world), a file per UDIM tile.
 * Called with the scene locked.
 * shortcut: the files are checked to exist, not to load (corrupt files, OSL textures are not
 * seen), count the image manager's load failures if farms need that. */
std::vector<std::string> GetTextureFiles(Scene *scene)
{
  std::vector<std::string> files;
  for (Shader *shader : scene->shaders) {
    /* Deleted and unassigned shaders stay in the scene unreferenced, Cycles loads none of their
     * images. */
    if (!shader->graph || shader->reference_count() == 0) {
      continue;
    }
    for (ShaderNode *node : shader->graph->nodes) {
      string filename;
      array<int> tiles;
      if (node->type == ImageTextureNode::get_node_type()) {
        const ImageTextureNode *image = static_cast<ImageTextureNode *>(node);
        filename = image->get_filename().string();
        tiles = image->get_tiles();
      }
      else if (node->type == EnvironmentTextureNode::get_node_type()) {
        filename = static_cast<EnvironmentTextureNode *>(node)->get_filename().string();
      }
      if (filename.empty()) {
        continue;
      }
      if (tiles.empty()) {
        files.push_back(filename);
      }
      /* The tile file names ImageManager::add_image loads. */
      for (const int tile : tiles) {
        string tileFilename = filename;
        string_replace(tileFilename, "<UDIM>", string_printf("%04d", tile));
        string_replace(tileFilename,
                       "<UVTILE>",
                       string_printf("u%d_v%d", (tile - 1001) % 10 + 1, (tile - 1001) / 10 + 1));
        files.push_back(tileFilename);
      }
    }
  }
  return files;
}

/* A Cycles setting from Karma Render Settings LOP attributes, used while the Cycles setting is not
 * authored, so scenes set up for Karma render alike. Karma's LOP authors all its attributes, also
 * at its defaults. `value` gets the settings and the first Karma attribute's value, and returns
 * the Cycles value, empty for no opinion. */
struct KarmaFallback {
  TfToken cycles;
  std::vector<TfToken> karma;
  std::function<VtValue(const HdRenderSettingsMap &settings, const VtValue &first)> value;
};

const std::vector<KarmaFallback> &GetKarmaFallbacks()
{
  using Settings = HdRenderSettingsMap;
  static const TfToken diffuseLimit("karma:object:diffuselimit");
  static const TfToken reflectLimit("karma:object:reflectlimit");
  static const TfToken refractLimit("karma:object:refractlimit");
  static const TfToken volumeLimit("karma:object:volumelimit");
  static const TfToken colorLimit("karma:global:colorlimit");
  static const TfToken imageFilter("karma:global:imagefilter");

  const auto find = [](const Settings &settings, const TfToken &key) {
    const auto it = settings.find(key);
    return it == settings.end() ? VtValue() : it->second;
  };
  /* Karma's limits are floats, picked stochastically between the integers around them. */
  const auto limit = [](const VtValue &value) {
    const VtValue number = VtValue::Cast<float>(value);
    return number.IsEmpty() ?
               VtValue() :
               VtValue(int(std::lround(std::max(number.UncheckedGet<float>(), 0.0f))));
  };
  const auto inverse = [](const VtValue &value) {
    const float x = VtValue::Cast<float>(value).GetWithDefault(0.0f);
    return x > 0.0f ? VtValue(1.0f / x) : VtValue();
  };
  /* The engine of Karma's denoise image filter, `[["denoise", {"engine": "oidn", ...}], ...]` as
   * the LOP writes it with json.dumps, empty without one.
   * shortcut: string search, not a JSON parse; parse it if other writers' spacing matters. */
  const auto denoiseEngine = [](const VtValue &value) -> std::optional<std::string> {
    if (value.IsEmpty()) {
      return std::nullopt;
    }
    const std::string filters = GetStringSetting(value, "");
    const size_t denoise = filters.find("[\"denoise\",");
    if (denoise == std::string::npos) {
      return std::string();
    }
    static const std::string engineKey = "\"engine\": \"";
    const size_t engine = filters.find(engineKey, denoise);
    if (engine == std::string::npos) {
      return std::string("oidn");
    }
    const size_t begin = engine + engineKey.size();
    return filters.substr(begin, filters.find('"', begin) - begin);
  };

  static const std::vector<KarmaFallback> fallbacks = {
      /* Path Traced Samples: Cycles is a path tracer (one indirect ray per bounce). */
      {HdCyclesRenderSettingsTokens->samples,
       {TfToken("karma:global:pathtracedsamples")},
       [](const Settings &, const VtValue &first) { return VtValue::Cast<int>(first); }},
      {TfToken("cycles:integrator:max_diffuse_bounce"),
       {diffuseLimit},
       [=](const Settings &, const VtValue &first) { return limit(first); }},
      {TfToken("cycles:integrator:max_glossy_bounce"),
       {reflectLimit},
       [=](const Settings &, const VtValue &first) { return limit(first); }},
      {TfToken("cycles:integrator:max_transmission_bounce"),
       {refractLimit},
       [=](const Settings &, const VtValue &first) { return limit(first); }},
      {TfToken("cycles:integrator:max_volume_bounce"),
       {volumeLimit},
       [=](const Settings &, const VtValue &first) { return limit(first); }},
      /* Karma has no total limit: a path does at most the sum of the limits. */
      {TfToken("cycles:integrator:max_bounce"),
       {diffuseLimit, reflectLimit, refractLimit, volumeLimit},
       [=](const Settings &settings, const VtValue &) {
         int total = 0;
         bool any = false;
         for (const TfToken &key : {diffuseLimit, reflectLimit, refractLimit, volumeLimit}) {
           const VtValue value = limit(find(settings, key));
           if (!value.IsEmpty()) {
             total += value.UncheckedGet<int>();
             any = true;
           }
         }
         return any ? VtValue(total) : VtValue();
       }},
      /* Only the indirect limit: Cycles' direct clamp also clamps emission and the background
       * seen by the camera. */
      {TfToken("cycles:integrator:sample_clamp_indirect"),
       {TfToken("karma:global:colorlimitindlinked"),
        colorLimit,
        TfToken("karma:global:colorlimitindirect")},
       [=](const Settings &settings, const VtValue &first) {
         const bool shared = VtValue::Cast<bool>(first).GetWithDefault(true);
         const VtValue value = VtValue::Cast<float>(
             find(settings, shared ? colorLimit : TfToken("karma:global:colorlimitindirect")));
         return value.IsEmpty() ? VtValue() :
                                  VtValue(std::max(value.UncheckedGet<float>(), 0.0f));
       }},
      {TfToken("cycles:integrator:use_denoise"),
       {imageFilter},
       [=](const Settings &, const VtValue &first) {
         const std::optional<std::string> engine = denoiseEngine(first);
         return engine ? VtValue(!engine->empty()) : VtValue();
       }},
      /* Other engines keep Cycles' denoiser. */
      {TfToken("cycles:integrator:denoiser_type"),
       {imageFilter},
       [=](const Settings &, const VtValue &first) {
         const std::optional<std::string> engine = denoiseEngine(first);
         return !engine ? VtValue() :
                *engine == "oidn" ? VtValue(TfToken("openimagedenoise")) :
                *engine == "optix" ? VtValue(TfToken("optix")) :
                                     VtValue();
       }},
      /* Karma's dicing quality 2 dices twice as finely as 1: half the Cycles dicing rate. */
      {HdCyclesRenderSettingsTokens->dicingRate,
       {TfToken("karma:global:dicingqualityscale")},
       [=](const Settings &, const VtValue &first) { return inverse(first); }},
      {HdCyclesRenderSettingsTokens->offscreenDicingScale,
       {TfToken("karma:global:offscreenquality")},
       [=](const Settings &, const VtValue &first) { return inverse(first); }},
      {HdCyclesRenderSettingsTokens->dicingCamera,
       {TfToken("karma:global:dicingcamera")},
       [](const Settings &, const VtValue &first) {
         const std::string camera = GetStringSetting(first, "");
         return camera.empty() ? VtValue() : VtValue(camera);
       }},
      {TfToken("cycles:integrator:use_light_tree"),
       {TfToken("karma:global:light_sampling_mode")},
       [](const Settings &, const VtValue &first) {
         return first.IsEmpty() ? VtValue() : VtValue(GetStringSetting(first, "") != "Uniform");
       }},
      {TfToken("cycles:integrator:seed"),
       {TfToken("karma:global:randomseed")},
       [](const Settings &, const VtValue &first) { return VtValue::Cast<int>(first); }},
      {HdCyclesRenderSettingsTokens->abortMissingTexture,
       {TfToken("karma:global:abortmissingtexture")},
       [](const Settings &, const VtValue &first) { return VtValue::Cast<bool>(first); }},
      {HdCyclesRenderSettingsTokens->abortNoGpu,
       {TfToken("karma:global:abortnogpudevice")},
       [](const Settings &, const VtValue &first) { return VtValue::Cast<bool>(first); }},
      /* Karma's LOP authors its Enable Motion Blur as disableMotionBlur, this is for others. */
      {HdCyclesRenderSettingsTokens->disableMotionBlur,
       {TfToken("karma:global:enablemblur")},
       [](const Settings &, const VtValue &first) {
         const VtValue enable = VtValue::Cast<bool>(first);
         return enable.IsEmpty() ? VtValue() : VtValue(!enable.UncheckedGet<bool>());
       }},
      {HdCyclesRenderSettingsTokens->disableDepthOfField,
       {TfToken("karma:global:enable_dof")},
       [](const Settings &, const VtValue &first) {
         const VtValue enable = VtValue::Cast<bool>(first);
         return enable.IsEmpty() ? VtValue() : VtValue(!enable.UncheckedGet<bool>());
       }},
  };
  return fallbacks;
}

HdCyclesResyncSceneIndexPtr FindResyncSceneIndex(const HdSceneIndexBaseRefPtr &sceneIndex)
{
  if (auto resync = TfDynamic_cast<HdCyclesResyncSceneIndexRefPtr>(sceneIndex)) {
    return resync;
  }
  if (auto filtering = TfDynamic_cast<HdFilteringSceneIndexBaseRefPtr>(sceneIndex)) {
    for (const HdSceneIndexBaseRefPtr &input : filtering->GetInputScenes()) {
      if (HdCyclesResyncSceneIndexPtr resync = FindResyncSceneIndex(input)) {
        return resync;
      }
    }
  }
  return nullptr;
}

}  // namespace

HdCyclesDelegate::HdCyclesDelegate(const HdRenderSettingsMap &settingsMap,
                                   Session *session_,
                                   const bool keep_nodes)
    : HdRenderDelegate()
{
  if (session_) {
    _renderParam = std::make_unique<HdCyclesSession>(session_, keep_nodes);
  }
  else {
    const SessionParams params = GetSessionParams(settingsMap);
    _renderParam = std::make_unique<HdCyclesSession>(params, GetSceneParams(settingsMap, params));
  }

  _settingsMap = settingsMap;
  ApplyKarmaDefaults();
  for (const auto &setting : settingsMap) {
    // Skip over the settings known to be used for initialization only
    if (IsSessionSetting(setting.first)) {
      continue;
    }

    SetRenderSetting(setting.first, setting.second);
  }
}

HdCyclesDelegate::~HdCyclesDelegate() = default;

namespace {

/* The shader of volumes without a material: a Principled Volume of this color (Cycles' own default is 0.5 grey, as in
 * Blender; Karma's unbound volumes scatter white). */
void SetDefaultVolumeColor(Scene *scene, const float3 color)
{
  unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();
  PrincipledVolumeNode *principled = graph->create_node<PrincipledVolumeNode>();
  principled->set_color(color);
  graph->connect(principled->output("Volume"), graph->output()->input("Volume"));
  scene->default_volume->set_graph(std::move(graph));
  scene->default_volume->tag_update(scene);
}

float3 DefaultVolumeColor(const VtValue &value)
{
  if (value.IsHolding<GfVec3f>() || value.IsHolding<GfVec3d>()) {
    const GfVec3f c = VtValue::Cast<GfVec3f>(value).UncheckedGet<GfVec3f>();
    return make_float3(c[0], c[1], c[2]);
  }
  return make_float3(VtValue::Cast<float>(value).GetWithDefault(1.0f));
}

const TfToken &DefaultVolumeColorSetting()
{
  static const TfToken token("cycles:volume:default_color");
  return token;
}

}  // namespace

void HdCyclesDelegate::ApplyKarmaDefaults()
{
  /* Measured with husk (hdcycles-blender motion_test.sh): an unbound VDB fog ball renders as in
   * Karma with white single scattering. Karma's Volume Limit defaults to 0 (Blender's Volume
   * Bounces too), Cycles' core to 7; Karma's unbound volumes scatter white, Cycles' default volume
   * is 0.5 grey (cycles:volume:default_color sets another). Authored settings set again after. */
  Scene *const scene = _renderParam->session->scene.get();
  scene->integrator->set_max_volume_bounce(0);
  const auto color = _settingsMap.find(DefaultVolumeColorSetting());
  SetDefaultVolumeColor(scene,
                        color != _settingsMap.end() ? DefaultVolumeColor(color->second) :
                                                      one_float3());
}

void HdCyclesDelegate::SetDrivers(const HdDriverVector &drivers)
{
  for (HdDriver *hdDriver : drivers) {
    if (hdDriver->name == HgiTokens->renderDriver && hdDriver->driver.IsHolding<Hgi *>()) {
      _hgi = hdDriver->driver.UncheckedGet<Hgi *>();
      break;
    }
  }
}

bool HdCyclesDelegate::IsDisplaySupported() const
{
#if defined(_WIN32) && defined(WITH_HYDRA_DISPLAY_DRIVER)
  return _hgi && _hgi->GetAPIName() == HgiTokens->OpenGL;
#else
  return false;
#endif
}

const TfTokenVector &HdCyclesDelegate::GetSupportedRprimTypes() const
{
  return kSupportedRPrimTypes;
}

const TfTokenVector &HdCyclesDelegate::GetSupportedSprimTypes() const
{
  return kSupportedSPrimTypes;
}

const TfTokenVector &HdCyclesDelegate::GetSupportedBprimTypes() const
{
  return kSupportedBPrimTypes;
}

HdRenderParam *HdCyclesDelegate::GetRenderParam() const
{
  return _renderParam.get();
}

HdResourceRegistrySharedPtr HdCyclesDelegate::GetResourceRegistry() const
{
  return HdResourceRegistrySharedPtr();
}

bool HdCyclesDelegate::IsPauseSupported() const
{
  return true;
}

bool HdCyclesDelegate::Pause()
{
  _renderParam->session->set_pause(true);
  return true;
}

bool HdCyclesDelegate::Resume()
{
  _renderParam->session->set_pause(false);
  return true;
}

HdRenderPassSharedPtr HdCyclesDelegate::CreateRenderPass(HdRenderIndex *index,
                                                         const HdRprimCollection &collection)
{
  _renderIndex = index;
  /* Frame rate of velocities, set before the render index was known (houdini:fps). */
  if (HdCyclesResyncSceneIndexPtr resyncSceneIndex = GetResyncSceneIndex()) {
    resyncSceneIndex->SetTimeCodesPerSecond(_renderParam->GetFramesPerSecond());
  }
  return HdRenderPassSharedPtr(new HdCyclesRenderPass(index, collection, _renderParam.get()));
}

HdInstancer *HdCyclesDelegate::CreateInstancer(HdSceneDelegate *delegate,
                                               const SdfPath &instancerId)
{
  return new HdCyclesInstancer(delegate, instancerId);
}

void HdCyclesDelegate::DestroyInstancer(HdInstancer *instancer)
{
  delete instancer;
}

HdRprim *HdCyclesDelegate::CreateRprim(const TfToken &typeId, const SdfPath &rprimId)
{
  if (typeId == HdPrimTypeTokens->mesh) {
    return new HdCyclesMesh(rprimId);
  }
  if (typeId == HdPrimTypeTokens->basisCurves) {
    return new HdCyclesCurves(rprimId);
  }
  if (typeId == HdPrimTypeTokens->points) {
    return new HdCyclesPoints(rprimId);
  }
  if (typeId == HdPrimTypeTokens->particleField) {
    return new HdCyclesPoints(rprimId, true);
  }
#ifdef WITH_OPENVDB
  if (typeId == HdPrimTypeTokens->volume) {
    return new HdCyclesVolume(rprimId);
  }
#endif

  TF_CODING_ERROR("Unknown Rprim type %s", typeId.GetText());
  return nullptr;
}

void HdCyclesDelegate::DestroyRprim(HdRprim *rPrim)
{
  delete rPrim;
}

HdSprim *HdCyclesDelegate::CreateSprim(const TfToken &typeId, const SdfPath &sprimId)
{
  if (typeId == HdPrimTypeTokens->camera) {
    return new HdCyclesCamera(sprimId);
  }
  if (typeId == HdPrimTypeTokens->material) {
    return new HdCyclesMaterial(sprimId);
  }
  if (typeId == HdPrimTypeTokens->cylinderLight) {
    return new HdCyclesCylinderLight(sprimId);
  }
  if (typeId == HdPrimTypeTokens->diskLight || typeId == HdPrimTypeTokens->distantLight ||
      typeId == HdPrimTypeTokens->domeLight || typeId == HdPrimTypeTokens->rectLight ||
      typeId == HdPrimTypeTokens->sphereLight)
  {
    return new HdCyclesLight(sprimId, typeId);
  }

  TF_CODING_ERROR("Unknown Sprim type %s", typeId.GetText());
  return nullptr;
}

HdSprim *HdCyclesDelegate::CreateFallbackSprim(const TfToken &typeId)
{
  return CreateSprim(typeId, SdfPath::EmptyPath());
}

void HdCyclesDelegate::DestroySprim(HdSprim *sPrim)
{
  delete sPrim;
}

HdBprim *HdCyclesDelegate::CreateBprim(const TfToken &typeId, const SdfPath &bprimId)
{
  if (typeId == HdPrimTypeTokens->renderBuffer) {
    return new HdCyclesRenderBuffer(bprimId);
  }
  if (typeId == HdPrimTypeTokens->renderSettings) {
    return new HdCyclesRenderSettings(bprimId);
  }
#ifdef WITH_OPENVDB
  if (typeId == _tokens->openvdbAsset) {
    return new HdCyclesField(bprimId, typeId);
  }
#endif
#ifdef WITH_HOUDINI_VOLUMES
  if (typeId == _tokens->houdiniFieldAsset) {
    return new HdCyclesField(bprimId, typeId);
  }
#endif

  TF_CODING_ERROR("Unknown Bprim type %s", typeId.GetText());
  return nullptr;
}

HdBprim *HdCyclesDelegate::CreateFallbackBprim(const TfToken &typeId)
{
  return CreateBprim(typeId, SdfPath::EmptyPath());
}

void HdCyclesDelegate::DestroyBprim(HdBprim *bPrim)
{
  delete bPrim;
}

void HdCyclesDelegate::CommitResources(HdChangeTracker *tracker)
{
  TF_UNUSED(tracker);

  const SceneLock lock(_renderParam.get());

#ifdef WITH_HOUDINI_IMAGES
  /* houdini:cop_texture_changed: convert the op: images again, through Houdini's resolver cache
   * again so that its viewport keeps watching their COPs, and update the shaders whose image file
   * changed. Only the op: images: this is set every frame of a time dependent COP, other images
   * would stat files and list UDIM directories each time. */
  if (_copTexturesChanged.exchange(false)) {
    HdCycles::ForgetHoudiniOpImages();
    const auto convertAgain = [](auto *image) {
      const ustring file = image->get_filename();
      if (!HdCycles::IsHoudiniOpImage(file.string())) {
        return false;
      }
      const ustring resolved(HdCycles::ResolveHoudiniImage(file.string()));
      if (resolved == file) {
        return false;
      }
      image->set_filename(resolved);
      image->handle.clear();
      return true;
    };
    bool changed = false;
    for (Shader *shader : lock.scene->shaders) {
      bool shaderChanged = false;
      for (ShaderNode *node : shader->graph->nodes) {
        if (node->type == ImageTextureNode::get_node_type()) {
          shaderChanged |= convertAgain(static_cast<ImageTextureNode *>(node));
        }
        else if (node->type == EnvironmentTextureNode::get_node_type()) {
          shaderChanged |= convertAgain(static_cast<EnvironmentTextureNode *>(node));
        }
      }
      if (shaderChanged) {
        shader->tag_update(lock.scene);
        changed = true;
      }
    }
    if (changed) {
      ++_settingsVersion;
    }
  }
#endif

  _renderParam->UpdateScene();
  _renderParam->rprimsResyncing = false;
  _renderParam->materialsResyncing = false;

  /* Requests of the prims just synced, for the next sync. */
  SendResync();
}

/* Called at the start of HdRenderIndex::SyncAll, which syncs the prims SendResync() dirtied. */
void HdCyclesDelegate::Update()
{
  /* Restart the render with the synced prims. The session thread takes scene changes into the
   * running render without restarting it, which the render pass could miss. */
  if (_renderParam->resyncPending.exchange(false)) {
    ++_settingsVersion;
  }
}

void HdCyclesDelegate::SendResync()
{
  if (!_renderIndex) {
    return;
  }
  const bool materials = _renderParam->resyncMaterials.exchange(false);
  const bool rprims = _renderParam->resyncRprims.exchange(false);
  const bool lights = _renderParam->resyncLights.exchange(false);
  const std::set<std::string> materialIds = _renderParam->TakeMaterialResyncs();
  if (!(materials || rprims || lights || !materialIds.empty()) || !_renderIndex) {
    return;
  }

  HdCyclesResyncSceneIndexPtr resyncSceneIndex = GetResyncSceneIndex();
  if (!resyncSceneIndex) {
    TF_WARN("No Cycles resync scene index, changed render settings may need a render restart");
    return;
  }

  const SdfPath &root = SdfPath::AbsoluteRootPath();
  HdSceneIndexObserver::DirtiedPrimEntries entries;
  const HdDataSourceLocatorSet materialLocators = {HdMaterialSchema::GetDefaultLocator()};
  if (materials) {
    for (const SdfPath &id : _renderIndex->GetSprimSubtree(HdPrimTypeTokens->material, root)) {
      entries.emplace_back(id, materialLocators);
    }
    _renderParam->materialsResyncing = true;
  }
  else {
    for (const std::string &material : materialIds) {
      /* A world material may not be synced yet: its first sync sees its world. */
      if (SdfPath::IsValidPathString(material) &&
          _renderIndex->GetSprim(HdPrimTypeTokens->material, SdfPath(material)))
      {
        entries.emplace_back(SdfPath(material), materialLocators);
      }
    }
  }
  if (rprims) {
    /* Transforms and points at the motion steps, and the primvars and volume fields shaders read
     * (the fields only dirty volumes).
     * shortcut: every rprim, also when one material reads new attributes; filter by the bound
     * materials if re-reading the primvars of big scenes gets slow. */
    const HdDataSourceLocatorSet locators = {HdXformSchema::GetDefaultLocator(),
                                             HdPrimvarsSchema::GetDefaultLocator(),
                                             HdVolumeFieldBindingSchema::GetDefaultLocator()};
    for (const SdfPath &id : _renderIndex->GetRprimSubtree(root)) {
      entries.emplace_back(id, locators);
    }
    const HdDataSourceLocatorSet cameraLocators = {HdXformSchema::GetDefaultLocator()};
    for (const SdfPath &id : _renderIndex->GetSprimSubtree(HdPrimTypeTokens->camera, root)) {
      entries.emplace_back(id, cameraLocators);
    }
    /* The rebuilt materials request an rprim sync for the attributes they read now, which this
     * sync does already: another one would re-read every rprim and restart the render again. */
    _renderParam->rprimsResyncing = true;
  }
  if (lights) {
    const HdDataSourceLocatorSet lightLocators = {HdLightSchema::GetDefaultLocator()};
    for (const TfToken &type : {HdPrimTypeTokens->distantLight, HdPrimTypeTokens->domeLight}) {
      for (const SdfPath &id : _renderIndex->GetSprimSubtree(type, root)) {
        entries.emplace_back(id, lightLocators);
      }
    }
  }
  resyncSceneIndex->Resync(entries);
  _renderParam->resyncPending = true;
}

HdCyclesResyncSceneIndexPtr HdCyclesDelegate::GetResyncSceneIndex()
{
  if (!_resyncSceneIndex && _renderIndex) {
    _resyncSceneIndex = FindResyncSceneIndex(_renderIndex->GetTerminalSceneIndex());
  }
  return _resyncSceneIndex;
}

TfToken HdCyclesDelegate::GetMaterialBindingPurpose() const
{
  return HdTokens->full;
}

TfTokenVector HdCyclesDelegate::GetMaterialRenderContexts() const
{
  TfTokenVector contexts = {_tokens->cycles};
#ifdef WITH_HYDRA_MATERIALX
  contexts.insert(contexts.end(), MaterialXRenderContexts().begin(), MaterialXRenderContexts().end());
#endif
  return contexts;
}

VtDictionary HdCyclesDelegate::GetRenderStats() const
{
  const Stats &stats = _renderParam->session->stats;
  const Progress &progress = _renderParam->session->progress;

  double totalTime;
  double renderTime;
  progress.get_time(totalTime, renderTime);
  const double fractionDone = progress.get_progress();

  std::string status;
  std::string substatus;
  progress.get_status(status, substatus);
  if (!substatus.empty()) {
    status += " | " + substatus;
  }

  Session *const session = _renderParam->session;
  /* The scheduler's sample count (RenderScheduler::set_sample_params). */
  const int samplesTotal = session->params.use_sample_subset ?
                               std::max(std::min(session->params.sample_subset_offset +
                                                     session->params.sample_subset_length,
                                                 session->params.samples) -
                                            session->params.sample_subset_offset,
                                        0) :
                               session->params.samples;
  const int samplesDone = std::min(progress.get_current_sample(), samplesTotal);
  /* Remaining time from the render time so far, like Blender's status bar. */
  const double remaining = (fractionDone > 0.0 && fractionDone < 1.0) ?
                               renderTime * (1.0 / fractionDone - 1.0) :
                               0.0;
  /* A multi device (several GPUs, or GPU and CPU) lists its devices instead of "Multi Device". */
  std::string device;
  if (session->device) {
    for (const DeviceInfo &info : session->device->info.multi_devices) {
      device += (device.empty() ? "" : " + ") + info.description;
    }
    if (device.empty()) {
      device = session->device->info.description;
    }
  }

  VtDictionary renderStats = {{"rendererName", VtValue("Cycles")},
                              {"rendererVersion",
                               VtValue(GfVec3i(CYCLES_VERSION_MAJOR,
                                               CYCLES_VERSION_MINOR,
                                               CYCLES_VERSION_PATCH))},
                              {"cyclesSamples",
                               VtValue(std::to_string(samplesDone) + " / " +
                                       std::to_string(samplesTotal))},
                              {"cyclesRemaining",
                               VtValue(remaining > 0.0 ? time_human_readable_from_seconds(remaining) :
                                                         std::string("-"))},
                              {"cyclesDevice", VtValue(device)},
                              {"percentDone", VtValue(floor_to_int(fractionDone * 100))},
                              {"fractionDone", VtValue(fractionDone)},
                              {"loadClockTime", VtValue(totalTime - renderTime)},
                              {"peakMemory", VtValue(stats.mem_peak)},
                              {"totalClockTime", VtValue(totalTime)},
                              {"totalMemory", VtValue(stats.mem_used)},
                              {"renderProgressAnnotation", VtValue(status)}};

  // Cryptomatte metadata of the specification, which husk writes into the image with the stats
  Scene *const scene = _renderParam->session->scene.get();
  const int cryptomatte = scene->film->get_cryptomatte_passes();
  if (cryptomatte != CRYPT_NONE) {
    const thread_scoped_lock lock(scene->mutex);
    auto addLayer = [&](const string &name, const string &manifest) {
      const uint32_t hash = util_murmur_hash3(name.c_str(), name.length(), 0);
      const string prefix = string_printf("cryptomatte/%08x", hash).substr(0, 19) + "/";
      renderStats[prefix + "name"] = VtValue(name);
      renderStats[prefix + "hash"] = VtValue(string("MurmurHash3_32"));
      renderStats[prefix + "conversion"] = VtValue(string("uint32_to_float32"));
      renderStats[prefix + "manifest"] = VtValue(manifest);
    };
    if (cryptomatte & CRYPT_OBJECT) {
      addLayer("CryptoObject", scene->object_manager->get_cryptomatte_objects(scene));
    }
    if (cryptomatte & CRYPT_MATERIAL) {
      addLayer("CryptoMaterial", scene->shader_manager->get_cryptomatte_materials(scene));
    }
    if (cryptomatte & CRYPT_ASSET) {
      addLayer("CryptoAsset", scene->object_manager->get_cryptomatte_assets(scene));
    }
  }

  /* Farm aborts, like Karma's Cancel Render on No Working GPU Devices and Redshift's Abort on
   * Missing Resources: husk stops with "Delegate fatal error" when huskErrorStatus is not 0. */
  const auto enabled = [&](const TfToken &key) {
    return VtValue::Cast<bool>(GetRenderSetting(key)).GetWithDefault(false);
  };
  std::string abortReason;
  if (enabled(HdCyclesRenderSettingsTokens->abortNoGpu) && session->device &&
      (session->device->info.type == DEVICE_CPU || progress.get_error()))
  {
    /* The device error through the progress, which locks it against the render threads. */
    abortReason = "no working GPU device (" +
                  (session->device->info.type == DEVICE_CPU ? device :
                                                              progress.get_error_message()) +
                  ")";
  }
  if (abortReason.empty() && enabled(HdCyclesRenderSettingsTokens->abortMissingTexture)) {
    /* Don't wait for a scene update in progress, the next stats call checks again. */
    std::vector<std::string> files;
    bool locked = false;
    {
      const thread_scoped_lock lock(scene->mutex, std::try_to_lock);
      if (lock.owns_lock()) {
        files = GetTextureFiles(scene);
        locked = true;
      }
    }
    /* Files are checked outside the scene lock (slow network storage), again when they change. */
    const std::lock_guard<std::mutex> lock(_abortMutex);
    if (locked && files != _checkedTextures) {
      _missingTexture.clear();
      for (const std::string &file : files) {
        if (!path_exists(file)) {
          _missingTexture = file;
          break;
        }
      }
      _checkedTextures = std::move(files);
    }
    if (!_missingTexture.empty()) {
      abortReason = "missing texture " + _missingTexture;
    }
  }
  if (!abortReason.empty()) {
    renderStats["huskErrorStatus"] = VtValue(1);
    renderStats["renderProgressAnnotation"] = VtValue("Cycles abort: " + abortReason);
    /* Stats are polled while rendering: report the reason once. */
    if (!_abortReported.exchange(true)) {
      TF_RUNTIME_ERROR("Cycles render aborted: %s", abortReason.c_str());
      /* husk hides TF errors at every verbosity, the Cycles log reaches the farm log. */
      LOG_ERROR << "Cycles render aborted: " << abortReason;
    }
  }

  return renderStats;
}

HdAovDescriptor HdCyclesDelegate::GetDefaultAovDescriptor(const TfToken &name) const
{
  if (name == HdAovTokens->color) {
    HdFormat colorFormat = HdFormatFloat32Vec4;
    if (IsDisplaySupported()) {
      // Can use Cycles 'DisplayDriver' in OpenGL, but it only supports 'half4' format
      colorFormat = HdFormatFloat16Vec4;
    }

    return HdAovDescriptor(colorFormat, false, VtValue(GfVec4f(0.0f)));
  }
  if (name == HdAovTokens->depth) {
    return HdAovDescriptor(HdFormatFloat32, false, VtValue(1.0f));
  }
  if (name == HdAovTokens->normal) {
    return HdAovDescriptor(HdFormatFloat32Vec3, false, VtValue(GfVec3f(0.0f)));
  }
  if (name == HdAovTokens->primId || name == HdAovTokens->instanceId) {
    return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
  }

  const PassType type = HdCyclesSession::GetPassType(name);
  if (type != PASS_NONE) {
    switch (Pass::get_info(type).num_components) {
      case 1:
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(0.0f));
      case 3:
        return HdAovDescriptor(HdFormatFloat32Vec3, false, VtValue(GfVec3f(0.0f)));
      case 4:
        return HdAovDescriptor(HdFormatFloat32Vec4, false, VtValue(GfVec4f(0.0f)));
      default:
        break;
    }
  }

  // Render vars are queried by prim name ("Normal", "DiffDir", ...); the pass comes from their
  // source name in the binding settings. Houdini leaves out AOVs without a valid descriptor.
  return HdAovDescriptor(HdFormatFloat32Vec3, false, VtValue(GfVec3f(0.0f)));
}

HdRenderSettingDescriptorList HdCyclesDelegate::GetRenderSettingDescriptors() const
{
  Scene *const scene = _renderParam->session->scene.get();

  HdRenderSettingDescriptorList descriptors;

  descriptors.push_back({
      "Time Limit",
      HdCyclesRenderSettingsTokens->timeLimit,
      VtValue(0.0),
  });
  descriptors.push_back({
      "Sample Count",
      HdCyclesRenderSettingsTokens->samples,
      VtValue(1024),
  });
  descriptors.push_back({
      "Sample Offset",
      HdCyclesRenderSettingsTokens->sampleOffset,
      VtValue(0),
  });

  for (const auto &[prefix, node] : GetSettingNodes(scene)) {
    for (const SocketType &socket : node->type->inputs) {
      if (socket.type == SocketType::NODE) {
        continue;
      }
      descriptors.push_back({socket.ui_name.string(),
                             TfToken(prefix + socket.name.string()),
                             GetNodeValue(node, socket)});
    }
  }

  return descriptors;
}

void HdCyclesDelegate::SetRenderSetting(const PXR_NS::TfToken &key, const PXR_NS::VtValue &value)
{
  Scene *const scene = _renderParam->session->scene.get();
  Session *const session = _renderParam->session;
  const std::vector<float> motionTimes = _renderParam->GetMotionTimes();

#ifdef WITH_HOUDINI_IMAGES
  /* Houdini's viewport sets this when a COP read by an op: image changed (or the frame of a time
   * dependent one), which syncs no material (KNOWN_ISSUES #55). It sets it on its main thread,
   * also while a sync runs, and only then clears the changed COPs from its resolver cache (all of
   * them on frame changes): converting here read the stale raster, left the COP unwatched once
   * Houdini cleared it, and blocked the main thread on the scene. The next sync converts. */
  if (key == _tokens->houdiniCopTextureChanged) {
    _copTexturesChanged = true;
    return;
  }
#endif

  /* Houdini's viewport creates the render delegate without settings and sets them afterwards,
   * also the ones it restarts the delegate for. Recreate the session for those while nothing
   * uses it, then set the other settings again on it. */
  /* Houdini sets the settings of a render settings prim that went away (display flag moved above
   * it) to empty values: they go back to their defaults, not to zero (zero film exposure renders
   * black). */
  if (value.IsEmpty()) {
    _settingsMap.erase(key);
  }
  else {
    _settingsMap[key] = value;
  }
  /* Karma settings only set the Cycles settings they are a fallback for. */
  if (TfStringStartsWith(key.GetString(), "karma:")) {
    ApplyKarmaFallbacks(key);
    return;
  }
  /* Authored or unset: no longer from Karma (unset gets Karma's value again at the end). */
  _karmaFallbacks.erase(key);
  if (IsSessionSetting(key)) {
    /* A thread count limits TBB in the whole process, which hangs Houdini with one thread: keep
     * the count the session was created with (all threads, unless husk set it). */
    HdRenderSettingsMap settings = _settingsMap;
    settings[HdCyclesRenderSettingsTokens->threads] = VtValue(session->params.threads);
    const SessionParams params = GetSessionParams(settings);
    const SceneParams sceneParams = GetSceneParams(settings, params);
    if (!session->params.modified(params) && !scene->params.modified(sceneParams)) {
      return;
    }
    if (_renderIndex || !_renderParam->IsSessionUnused()) {
      TF_WARN("Cycles render setting %s only applies to a new render", key.GetText());
      return;
    }
    _renderParam->RecreateSession(params, sceneParams);
    ApplyKarmaDefaults();
    settings = _settingsMap;
    for (const auto &[otherKey, otherValue] : settings) {
      /* Settings from Karma are set again by their Karma settings. */
      if (!IsSessionSetting(otherKey) && !_karmaFallbacks.count(otherKey)) {
        SetRenderSetting(otherKey, otherValue);
      }
    }
    return;
  }

  if (key == HdCyclesRenderSettingsTokens->stageMetersPerUnit) {
    _renderParam->SetStageMetersPerUnit(
        VtValue::Cast<double>(value).GetWithDefault(_renderParam->GetStageMetersPerUnit()));
  }
  else if (key == HdCyclesRenderSettingsTokens->stageUpAxis) {
    _renderParam->SetStageUpAxis(
      VtValue::Cast<TfToken>(value).GetWithDefault(_renderParam->GetStageUpAxis()));
  }
  else if (key == HdCyclesRenderSettingsTokens->timeLimit) {
    session->set_time_limit(value.IsEmpty() ? SessionParams().time_limit :
                                              VtValue::Cast<double>(value).GetWithDefault(
                                                  session->params.time_limit));
  }
  else if (key == HdCyclesRenderSettingsTokens->samples) {
    static const int max_samples = Integrator::MAX_SAMPLES;
    int samples = value.IsEmpty() ?
                      SessionParams().samples :
                      VtValue::Cast<int>(value).GetWithDefault(session->params.samples);
    samples = std::min(std::max(1, samples), max_samples);
    session->set_samples(samples);
  }
  else if (key == HdCyclesRenderSettingsTokens->sampleOffset) {
    session->params.sample_subset_offset = value.IsEmpty() ?
                                               0 :
                                               VtValue::Cast<int>(value).GetWithDefault(
                                                   session->params.sample_subset_offset);
    session->params.sample_subset_length = Integrator::MAX_SAMPLES;
    session->params.use_sample_subset = session->params.sample_subset_offset > 0;
    ++_settingsVersion;
  }
  else if (key == HdCyclesRenderSettingsTokens->windowDepth) {
    _renderParam->SetDepthToWindow(VtValue::Cast<bool>(value).GetWithDefault(false));
  }
  else if (key == HdCyclesRenderSettingsTokens->sunIntensityScale) {
    if (_renderParam->SetSunIntensityScale(VtValue::Cast<float>(value).GetWithDefault(1.0f))) {
      _renderParam->resyncLights = true;
    }
  }
  else if (key == HdCyclesRenderSettingsTokens->domeCameraVisibleDefault) {
    if (_renderParam->SetDomeCameraVisibleDefault(
            VtValue::Cast<bool>(value).GetWithDefault(false)))
    {
      _renderParam->resyncLights = true;
    }
  }
  else if (key == HdCyclesRenderSettingsTokens->houdiniViewport) {
    _renderParam->SetViewport(VtValue::Cast<bool>(value).GetWithDefault(false));
    /* Houdini redraws a viewport render that has not converged about every 0.2 s: show new
     * samples at each redraw, instead of every 2 s after a few seconds. */
    session->params.max_display_update_interval = _renderParam->IsViewport() ? 0.2 : 0.0;
    session->params.use_auto_tile = !_renderParam->IsViewport();
    /* Pixel Size only applies to the viewport: set it again when it came before this setting
     * (may recreate the session, so nothing after this uses it). */
    const auto pixelSize = _settingsMap.find(HdCyclesRenderSettingsTokens->pixelSize);
    if (pixelSize != _settingsMap.end()) {
      const VtValue pixelSizeValue = pixelSize->second;
      SetRenderSetting(HdCyclesRenderSettingsTokens->pixelSize, pixelSizeValue);
    }
  }
  else if (key == HdCyclesRenderSettingsTokens->houdiniFps) {
    const double fps = _renderParam->GetFramesPerSecond();
    _renderParam->SetFramesPerSecond(VtValue::Cast<double>(value).GetWithDefault(0.0));
    /* Velocities move points by the frame rate. */
    if (HdCyclesResyncSceneIndexPtr resyncSceneIndex = GetResyncSceneIndex()) {
      resyncSceneIndex->SetTimeCodesPerSecond(_renderParam->GetFramesPerSecond());
    }
    if (_renderParam->GetFramesPerSecond() != fps) {
      _renderParam->RequestResync(false, true);
    }
  }
  else if (key == HdCyclesRenderSettingsTokens->disableMotionBlur ||
           key == HdCyclesRenderSettingsTokens->instantaneousShutter)
  {
    /* instantaneousShutter is the deprecated UsdRenderSettings name of disableMotionBlur. */
    const auto get = [&](const TfToken &setting) {
      const auto it = _settingsMap.find(setting);
      return it != _settingsMap.end() && VtValue::Cast<bool>(it->second).GetWithDefault(false);
    };
    _renderParam->SetMotionBlurDisabled(get(HdCyclesRenderSettingsTokens->disableMotionBlur) ||
                                        get(HdCyclesRenderSettingsTokens->instantaneousShutter));
  }
  else if (key == DefaultVolumeColorSetting()) {
    /* A grey (float) or a color: volumes without a material (white when unset, as Karma). */
    SetDefaultVolumeColor(scene, value.IsEmpty() ? one_float3() : DefaultVolumeColor(value));
    ++_settingsVersion;
  }
  else if (key == HdCyclesRenderSettingsTokens->disableDepthOfField) {
    const bool disabled = VtValue::Cast<bool>(value).GetWithDefault(false);
    /* Camera only: restart the render, so a finished one gets it too. */
    if (_renderParam->depthOfFieldDisabled.exchange(disabled) != disabled) {
      ++_settingsVersion;
    }
  }
  else if (key == HdCyclesRenderSettingsTokens->motionSteps) {
    _renderParam->SetMotionSteps(VtValue::Cast<int>(value).GetWithDefault(1));
  }
  else if (key == HdCyclesRenderSettingsTokens->dicingRate ||
           key == HdCyclesRenderSettingsTokens->previewDicingRate ||
           key == HdCyclesRenderSettingsTokens->maxSubdivisions ||
           key == HdCyclesRenderSettingsTokens->offscreenDicingScale ||
           key == HdCyclesRenderSettingsTokens->dicingCamera)
  {
    // Blender's Subdivision panel, with Blender's defaults and limits
    const auto get = [&](const TfToken &setting, const auto fallback) {
      const auto it = _settingsMap.find(setting);
      return it == _settingsMap.end() ?
                 fallback :
                 VtValue::Cast<decltype(fallback)>(it->second).GetWithDefault(fallback);
    };
    HdCyclesSession::DicingSettings dicing;
    dicing.rate = std::max(0.1f, get(HdCyclesRenderSettingsTokens->dicingRate, 1.0f));
    dicing.previewRate = std::max(0.1f,
                                  get(HdCyclesRenderSettingsTokens->previewDicingRate, 8.0f));
    dicing.maxLevel = std::clamp(get(HdCyclesRenderSettingsTokens->maxSubdivisions, 12), 0, 16);
    dicing.offscreenScale = std::max(
        1.0f, get(HdCyclesRenderSettingsTokens->offscreenDicingScale, 4.0f));
    if (const auto it = _settingsMap.find(HdCyclesRenderSettingsTokens->dicingCamera);
        it != _settingsMap.end())
    {
      dicing.camera = GetStringSetting(it->second, "");
    }
    _renderParam->SetDicing(dicing);
  }
  else if (key == HdCyclesRenderSettingsTokens->materialOverride) {
    _renderParam->SetMaterialOverride(GetStringSetting(value, ""));
  }
  else if (key == HdCyclesRenderSettingsTokens->bakeObject ||
           key == HdCyclesRenderSettingsTokens->bakeInstance ||
           key == HdCyclesRenderSettingsTokens->bakeUVMap ||
           key == HdCyclesRenderSettingsTokens->bakeSelectedToActive ||
           key == HdCyclesRenderSettingsTokens->bakeSource ||
           key == HdCyclesRenderSettingsTokens->bakeCageObject ||
           key == HdCyclesRenderSettingsTokens->bakeCageExtrusion ||
           key == HdCyclesRenderSettingsTokens->bakeMaxRayDistance)
  {
    const auto get = [&](const TfToken &setting) {
      const auto it = _settingsMap.find(setting);
      return it == _settingsMap.end() ? VtValue() : it->second;
    };
    const auto getFloat = [&](const TfToken &setting) {
      return std::max(0.0f, VtValue::Cast<float>(get(setting)).GetWithDefault(0.0f));
    };
    BakeSettings bake;
    bake.object = GetStringSetting(get(HdCyclesRenderSettingsTokens->bakeObject), "");
    bake.instance = std::max(
        0, VtValue::Cast<int>(get(HdCyclesRenderSettingsTokens->bakeInstance)).GetWithDefault(0));
    bake.uvMap = GetStringSetting(get(HdCyclesRenderSettingsTokens->bakeUVMap), "");
    bake.selectedToActive =
        VtValue::Cast<bool>(get(HdCyclesRenderSettingsTokens->bakeSelectedToActive))
            .GetWithDefault(false);
    bake.sources = GetStringSetting(get(HdCyclesRenderSettingsTokens->bakeSource), "");
    bake.cage = GetStringSetting(get(HdCyclesRenderSettingsTokens->bakeCageObject), "");
    bake.cageExtrusion = getFloat(HdCyclesRenderSettingsTokens->bakeCageExtrusion);
    bake.maxRayDistance = getFloat(HdCyclesRenderSettingsTokens->bakeMaxRayDistance);
    _renderParam->SetBake(bake);
  }
  else if (key == HdCyclesRenderSettingsTokens->bakeNormalSpace) {
    const std::string space = GetStringSetting(value, "tangent");
    const int bakeNormalSpace = space == "world" ? 0 : space == "object" ? 1 : 2;
    /* Output only: restart the render, so a finished one gets it too. */
    if (_renderParam->bakeNormalSpace.exchange(bakeNormalSpace) != bakeNormalSpace) {
      ++_settingsVersion;
    }
  }
  else if (key == HdCyclesRenderSettingsTokens->bakeMargin) {
    const int bakeMargin = std::max(0, VtValue::Cast<int>(value).GetWithDefault(16));
    /* Output only: restart the render, so a finished one gets it too. */
    if (_renderParam->bakeMargin.exchange(bakeMargin) != bakeMargin) {
      ++_settingsVersion;
    }
  }
  else if (key == HdCyclesRenderSettingsTokens->shutterOpen ||
           key == HdCyclesRenderSettingsTokens->shutterClose)
  {
    _renderParam->SetShutterSetting(key == HdCyclesRenderSettingsTokens->shutterClose,
                                    float(VtValue::Cast<double>(value).GetWithDefault(0.0)));
  }
  else if (key == HdCyclesRenderSettingsTokens->transparentBackground) {
    _renderParam->SetTransparentBackground(
        value.IsEmpty() ? std::nullopt :
                          std::optional<bool>(VtValue::Cast<bool>(value).GetWithDefault(false)));
    ++_settingsVersion;
  }
  else if (key == HdCyclesRenderSettingsTokens->fastGi ||
           key == HdCyclesRenderSettingsTokens->fastGiMethod ||
           key == HdCyclesRenderSettingsTokens->aoBounces ||
           key == HdCyclesRenderSettingsTokens->aoFactor)
  {
    if (key == HdCyclesRenderSettingsTokens->fastGi) {
      _fastGi = VtValue::Cast<bool>(value).GetWithDefault(false);
    }
    else if (key == HdCyclesRenderSettingsTokens->fastGiMethod) {
      _fastGiMethod = GetStringSetting(value, "replace");
    }
    else if (key == HdCyclesRenderSettingsTokens->aoBounces) {
      _aoBounces = VtValue::Cast<int>(value).GetWithDefault(1);
    }
    else {
      _aoFactor = VtValue::Cast<float>(value).GetWithDefault(1.0f);
    }
    /* Like Blender: Replace uses AO instead of indirect light after the bounces, Add adds AO to
     * diffuse surfaces. Cycles has no bounces without the replace factor. */
    const bool add = _fastGiMethod == "add";
    const float factor = _fastGi ? _aoFactor : 0.0f;
    scene->integrator->set_ao_factor(add ? 0.0f : factor);
    scene->integrator->set_ao_additive_factor(add ? factor : 0.0f);
    scene->integrator->set_ao_bounces(_fastGi ? _aoBounces : 0);
    ++_settingsVersion;
  }
  else {
    ustring socketName;
    if (Node *const node = FindSettingNode(scene, key.GetString(), socketName)) {
      /* The Fast GI settings set the AO sockets when authored, in any order. Without them, the
       * sockets authored by older Cycles render settings still apply. */
      const bool fastGiSocket = node == scene->integrator &&
                                (socketName == "ao_factor" || socketName == "ao_additive_factor" ||
                                 socketName == "ao_bounces");
      const bool fastGiAuthored = _settingsMap.count(HdCyclesRenderSettingsTokens->fastGi) ||
                                  _settingsMap.count(HdCyclesRenderSettingsTokens->fastGiMethod) ||
                                  _settingsMap.count(HdCyclesRenderSettingsTokens->aoBounces) ||
                                  _settingsMap.count(HdCyclesRenderSettingsTokens->aoFactor);
      const SocketType *socket = node->type->find_input(socketName);
      if (socket && !(fastGiSocket && fastGiAuthored)) {
        if (value.IsEmpty() && node == scene->integrator && socketName == "max_volume_bounce") {
          scene->integrator->set_max_volume_bounce(0); /* Karma's default (ApplyKarmaDefaults) */
        }
        else if (value.IsEmpty()) {
          node->set_default_value(*socket);
        }
        else {
          SetNodeValue(node, *socket, value);
        }
        ++_settingsVersion;

        /* The render pass multiplies it by the exposure of the camera. */
        if (node == scene->film && socketName == "exposure") {
          _renderParam->SetFilmExposure(scene->film->get_exposure());
        }

        /* The session overwrites the integrator sample subset with its own on every reset. */
        if (node == scene->integrator &&
            (socketName == "use_sample_subset" || socketName == "sample_subset_offset" ||
             socketName == "sample_subset_length"))
        {
          session->params.use_sample_subset = scene->integrator->get_use_sample_subset();
          session->params.sample_subset_offset = scene->integrator->get_sample_subset_offset();
          session->params.sample_subset_length = scene->integrator->get_sample_subset_length();
        }
      }
    }
  }

  /* Without the Cycles Motion Blur setting, motion blur is on when the shutter is open, as in Karma
   * (husk and the viewport send the camera's shutter as shutter:open / close); disableMotionBlur
   * still turns it off. */
  static const TfToken motionBlurSetting("cycles:integrator:motion_blur");
  if (!_settingsMap.count(motionBlurSetting)) {
    const bool open = _renderParam->HasOpenShutter();
    if (scene->integrator->get_motion_blur() != open) {
      scene->integrator->set_motion_blur(open);
      ++_settingsVersion;
    }
  }

  /* Motion blur on or off, or a new shutter: geometry and cameras need their motion steps. */
  if (_renderParam->GetMotionTimes() != motionTimes) {
    _renderParam->RequestResync(false, true);
  }

  if (TfStringStartsWith(key.GetString(), "cycles:integrator:denois") ||
      key == HdCyclesRenderSettingsTokens->houdiniViewport)
  {
    UpdateDenoiser();
  }

  SendResync();

  if (value.IsEmpty()) {
    ApplyKarmaFallbacks(key);
  }
}

void HdCyclesDelegate::ApplyKarmaFallbacks(const TfToken &key)
{
  for (const KarmaFallback &fallback : GetKarmaFallbacks()) {
    if (fallback.cycles != key &&
        std::find(fallback.karma.begin(), fallback.karma.end(), key) == fallback.karma.end())
    {
      continue;
    }
    /* An authored Cycles setting wins. */
    const bool fromKarma = _karmaFallbacks.count(fallback.cycles) != 0;
    if (_settingsMap.count(fallback.cycles) && !fromKarma) {
      continue;
    }
    const auto first = _settingsMap.find(fallback.karma.front());
    const VtValue value = fallback.value(
        _settingsMap, first == _settingsMap.end() ? VtValue() : first->second);
    if (value.IsEmpty() && !fromKarma) {
      continue;
    }
    /* Empty resets it to the Cycles default. Setting it erases it from _karmaFallbacks. */
    SetRenderSetting(fallback.cycles, value);
    if (!value.IsEmpty()) {
      _karmaFallbacks.insert(fallback.cycles);
    }
  }
}

void HdCyclesDelegate::UpdateDenoiser()
{
  Integrator *const integrator = _renderParam->session->scene->integrator;

  /* The sockets set for DLSS, from their settings when it is not used. */
  for (const char *name : {"denoiser_type",
                           "denoiser_passes",
                           "denoise_start_sample",
                           "denoiser_quality",
                           "denoiser_upscale_factor",
                           "scrambling_distance"})
  {
    const SocketType *socket = integrator->type->find_input(ustring(name));
    const auto setting = _settingsMap.find(TfToken(std::string("cycles:integrator:") + name));
    if (setting != _settingsMap.end()) {
      SetNodeValue(integrator, *socket, setting->second);
    }
    else {
      integrator->set_default_value(*socket);
    }
  }
  if (integrator->get_denoiser_type() != DENOISER_DLSS) {
    return;
  }

  /* Like Blender, DLSS only denoises interactive renders. It needs an NVIDIA RTX GPU and a
   * driver with the DLSS Ray Reconstruction runtime (620 or newer). */
  const DeviceInfo &device = _renderParam->session->params.denoise_device;
  if (!_renderParam->IsViewport() || !(device.denoisers & DENOISER_DLSS)) {
    const DenoiserType fallback = _renderParam->IsViewport() ?
                                      Denoiser::automatic_viewport_denoiser_type(device) :
                                      DENOISER_OPENIMAGEDENOISE;
    integrator->set_denoiser_type(fallback != DENOISER_NONE ? fallback :
                                                              DENOISER_OPENIMAGEDENOISE);
    if (!_dlssWarned) {
      TF_WARN("Cycles: DLSS %s, denoising with %s instead",
              _renderParam->IsViewport() ? "is not supported by this GPU or driver" :
                                           "only denoises the viewport",
              denoiserTypeToHumanReadable(integrator->get_denoiser_type()));
      _dlssWarned = true;
    }
    return;
  }

  /* What Blender's viewport sets for DLSS. Every sample is denoised, at full resolution: the
   * output driver only writes full resolution passes, which the other AOVs need.
   * shortcut: no upscale modes, add them when the output driver can write upscaled AOVs. */
  integrator->set_denoiser_passes(DENOISER_PASS_ALBEDO | DENOISER_PASS_SPECULAR_ALBEDO |
                                  DENOISER_PASS_NORMAL | DENOISER_PASS_ROUGHNESS |
                                  DENOISER_PASS_DEPTH | DENOISER_PASS_BACKWARD_MOTION |
                                  DENOISER_PASS_SPECULAR_MOTION);
  integrator->set_denoise_start_sample(0);
  integrator->set_denoiser_quality(DENOISER_QUALITY_HIGH);
  integrator->set_denoiser_upscale_factor(1.0f);
  /* DLSS accumulates 1 sample renders, which scrambling makes too different. */
  integrator->set_scrambling_distance(1.0f);
}

VtValue HdCyclesDelegate::GetRenderSetting(const TfToken &key) const
{
  Scene *const scene = _renderParam->session->scene.get();
  Session *const session = _renderParam->session;

  if (key == HdCyclesRenderSettingsTokens->stageMetersPerUnit) {
    return VtValue(_renderParam->GetStageMetersPerUnit());
  }
  if (key == HdCyclesRenderSettingsTokens->stageUpAxis) {
    return VtValue(_renderParam->GetStageUpAxis());
  }
  if (key == HdCyclesRenderSettingsTokens->device) {
    return VtValue(TfToken(Device::string_from_type(session->params.device.type)));
  }
  if (key == HdCyclesRenderSettingsTokens->threads) {
    return VtValue(session->params.threads);
  }
  if (key == HdCyclesRenderSettingsTokens->shadingSystem) {
    return VtValue(session->params.shadingsystem == SHADINGSYSTEM_OSL);
  }
  if (key == HdCyclesRenderSettingsTokens->useResolutionDivider) {
    return VtValue(session->params.use_resolution_divider);
  }
  if (key == HdCyclesRenderSettingsTokens->curveShape) {
    return VtValue(TfToken(scene->params.hair_shape == CURVE_THICK ? "thick" : "ribbon"));
  }
  if (key == HdCyclesRenderSettingsTokens->tileSize) {
    return VtValue(session->params.tile_size);
  }
  if (key == HdCyclesRenderSettingsTokens->pixelSize) {
    return VtValue(TfToken(std::to_string(session->params.pixel_size)));
  }
  if (key == HdCyclesRenderSettingsTokens->useSpatialSplits) {
    return VtValue(scene->params.use_bvh_spatial_split);
  }
  if (key == HdCyclesRenderSettingsTokens->useCompactBvh) {
    return VtValue(scene->params.use_bvh_compact_structure);
  }
  if (key == HdCyclesRenderSettingsTokens->curveSubdivisions) {
    return VtValue(scene->params.hair_subdivisions);
  }
  if (key == HdCyclesRenderSettingsTokens->timeLimit) {
    return VtValue(session->params.time_limit);
  }
  if (key == HdCyclesRenderSettingsTokens->samples) {
    return VtValue(session->params.samples);
  }
  if (key == HdCyclesRenderSettingsTokens->sampleOffset) {
    return VtValue((session->params.use_sample_subset) ? session->params.sample_subset_offset : 0);
  }
  if (key == HdCyclesRenderSettingsTokens->fastGi) {
    return VtValue(_fastGi);
  }
  if (key == HdCyclesRenderSettingsTokens->fastGiMethod) {
    return VtValue(TfToken(_fastGiMethod));
  }
  if (key == HdCyclesRenderSettingsTokens->aoBounces) {
    return VtValue(_aoBounces);
  }
  if (key == HdCyclesRenderSettingsTokens->aoFactor) {
    return VtValue(_aoFactor);
  }

  ustring socketName;
  if (Node *const node = FindSettingNode(scene, key.GetString(), socketName)) {
    if (const SocketType *socket = node->type->find_input(socketName)) {
      return GetNodeValue(node, *socket);
    }
  }

  // Other settings as they were set, like HdRenderDelegate does (husk's resolution)
  const auto it = _settingsMap.find(key);
  return it != _settingsMap.end() ? it->second : VtValue();
}

/* Houdini lists the commands in the viewport renderer menu. */
HdCommandDescriptors HdCyclesDelegate::GetCommandDescriptors() const
{
  return {HdCommandDescriptor(_tokens->reloadTextures, "Reload Textures")};
}

bool HdCyclesDelegate::InvokeCommand(const TfToken &command, const HdCommandArgs & /*args*/)
{
  if (command != _tokens->reloadTextures) {
    return false;
  }

  /* Shaders add their images again when compiled without an image handle, and the image manager
   * loads the files that changed on disk since they were loaded (OIIOImageLoader::equals). OSL
   * holds handles of its own. */
#ifdef WITH_HOUDINI_IMAGES
  HdCycles::ForgetHoudiniOpImages();
#endif
  const SceneLock lock(_renderParam.get());
  lock.scene->osl_manager->free_textures(lock.scene);
  for (Shader *shader : lock.scene->shaders) {
    /* UDIM tiles added or removed on disk. */
    HdCyclesMaterial::SetUdimTiles(shader->graph.get());
    for (ShaderNode *node : shader->graph->nodes) {
      if (node->special_type == SHADER_SPECIAL_TYPE_IMAGE_SLOT) {
        static_cast<ImageSlotTextureNode *>(node)->handle.clear();
      }
    }
    shader->tag_update(lock.scene);
  }
  /* Restart the render, see Update(). */
  ++_settingsVersion;
  return true;
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
