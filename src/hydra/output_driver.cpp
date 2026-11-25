/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/output_driver.h"
#include "hydra/bake.h"
#include "hydra/render_buffer.h"
#include "hydra/session.h"

#include "scene/pass.h"
#include "scene/scene.h"
#include "session/session.h"
#include "util/time.h"

#include <algorithm>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

// Cycles writes the distance along the camera axis (1e10 for background), Hydra expects window
// depth in 0..1 like an OpenGL depth buffer. Houdini uses it for the tumble pivot.
void convert_depth(float *depth, const size_t num_pixels, const GfMatrix4d &projection)
{
  for (size_t i = 0; i < num_pixels; ++i) {
    if (depth[i] >= 1e9f) {
      depth[i] = 1.0f;
      continue;
    }
    // Point on the view axis at that distance (camera looks down -Z)
    const double z = -depth[i];
    const double clip_z = z * projection[2][2] + projection[3][2];
    const double clip_w = z * projection[2][3] + projection[3][3];
    const double ndc_z = clip_w != 0.0 ? clip_z / clip_w : 1.0;
    depth[i] = float(std::clamp(ndc_z * 0.5 + 0.5, 0.0, 1.0));
  }
}

}  // namespace

HdCyclesOutputDriver::HdCyclesOutputDriver(HdCyclesSession *renderParam)
    : _renderParam(renderParam)
{
}

void HdCyclesOutputDriver::write_render_tile(const Tile &tile)
{
  const auto bindingsLock = _renderParam->LockAovBindings();
  update_render_buffers(tile, true);

  // Update convergence state of all render buffers
  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindings()) {
    if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(aovBinding.renderBuffer)) {
      renderBuffer->SetConverged(true);
    }
  }
}

void HdCyclesOutputDriver::convert_units(const TfToken &aovName,
                                         float *data,
                                         const size_t num_pixels,
                                         const size_t channels) const
{
  if (aovName != HdAovTokens->depth || channels != 1) {
    return;
  }
  if (_renderParam->IsViewport() || _renderParam->GetDepthToWindow()) {
    convert_depth(data, num_pixels, _renderParam->GetProjectionMatrix());
  }
  else if (_renderParam->GetDepthInMetres()) {
    /* The scene is in stage units, so is the distance: metres for hosts that want them. */
    const float metersPerUnit = float(_renderParam->GetStageMetersPerUnit());
    if (metersPerUnit > 0.0f && metersPerUnit != 1.0f) {
      for (size_t i = 0; i < num_pixels; ++i) {
        if (data[i] < 1e9f) { /* The background stays far. */
          data[i] *= metersPerUnit;
        }
      }
    }
  }
}

bool HdCyclesOutputDriver::read_render_tile(const Tile &tile)
{
  /* Cycles renders without the scene lock, which Hydra holds while it resets the session (and
   * so waits for this render to stop): copy the bake target when the lock is free, otherwise
   * use the last copy of it. */
  Scene *const scene = _renderParam->session->scene.get();
  const BakeSettings bake = _renderParam->GetBake();
  std::unique_lock<thread_mutex> sceneLock(scene->mutex, std::try_to_lock);
  if (sceneLock.owns_lock()) {
    std::shared_ptr<const BakeSources> previousSources;
    {
      const std::lock_guard<std::mutex> lock(_bakeMeshMutex);
      previousSources = _bakeMesh.sources;
    }
    unsigned int sourcesVersion = 0;
    const std::vector<std::string> sourcePrims = _renderParam->GetBakeSourcePrims(sourcesVersion);
    BakeMesh mesh = SnapshotBakeMesh(scene, bake, sourcePrims, sourcesVersion, previousSources);
    sceneLock.unlock();
    const std::lock_guard<std::mutex> lock(_bakeMeshMutex);
    _bakeMesh = std::move(mesh);
    _bakeMeshSettings = bake;
  }
  const std::lock_guard<std::mutex> lock(_bakeMeshMutex);
  static const BakeMesh noMesh;
  return FillBakeTile(
      _bakeMeshSettings == bake && BakeMeshCurrent(_bakeMesh, scene) ? _bakeMesh : noMesh, tile);
}

bool HdCyclesOutputDriver::update_render_tile(const Tile &tile)
{
  update_render_buffers(tile, false);
  return true;
}

void HdCyclesOutputDriver::update_render_buffers(const Tile &tile, const bool final)
{
  // The bound render buffers stay alive and bound while this writes them
  const auto bindingsLock = _renderParam->LockAovBindings();
  std::vector<float> pixels;

  // The Cycles display driver updates the display AOV texture while rendering. Until its texture
  // holds an image (no GL context yet, render done before the first upload) the buffer gets the
  // updates. The final image also goes to the buffer, which Houdini reads for slap comps and
  // snapshots.
  const HdRenderPassAovBinding displayAovBinding = _renderParam->GetDisplayAovBinding();
  const auto *const displayBuffer = static_cast<HdCyclesRenderBuffer *>(
      displayAovBinding.renderBuffer);
  const bool skipDisplayAov = !final && displayBuffer && displayBuffer->HasResource();

  /* The Houdini viewport reads the AOVs it does not display only on demand: depth for the tumble
   * pivot, IDs for picking, slap comp inputs. Reading them back takes 30-50 ms per update of a 2K
   * render, longer than the path tracing between the first updates after a reset. Depth and IDs
   * are final after the first sample, so update them once the render ran for 0.25 s (camera
   * changes of a tumble come faster, so tumbling does not wait for them), then at most once a
   * second. The final update writes all of them. */
  bool skipOtherAovs = false;
  if (skipDisplayAov && _renderParam->IsViewport()) {
    double totalTime, renderTime;
    _renderParam->session->progress.get_time(totalTime, renderTime);
    const double time = time_dt();
    const bool updatedSinceReset = time - _aovsUpdateTime <= renderTime;
    if (renderTime < 0.25 || (updatedSinceReset && time - _aovsUpdateTime < 1.0)) {
      skipOtherAovs = true;
    }
    else {
      _aovsUpdateTime = time;
    }
  }

  /* Texture baking: the margin around the UV islands, in every AOV but the IDs.
   * shortcut: margin x pixels work per update, so only final images and husk's progressive
   * updates get it; visit only the pixels next to the islands if it gets slow. */
  const bool useBakeMargin = final || !_renderParam->IsViewport();
  const std::vector<unsigned char> bakeCoverage = (!useBakeMargin ||
                                                   _renderParam->GetBakeObject().empty()) ?
                                                      std::vector<unsigned char>() :
                                                      BakeCoverage(tile);
  /* Baked normals in the Normal Space setting, before the margin. */
  const BakeSettings bake = _renderParam->GetBake();
  const auto bakeNormals = [&](const TfToken &aovName, float *data, const size_t channels) {
    if (bake.object.empty()) {
      return;
    }
    Scene *const scene = _renderParam->session->scene.get();
    const string passName = _renderParam->GetPassName(aovName);
    for (const Pass *pass : scene->passes) {
      if (pass->get_type() == PASS_NORMAL && pass->get_name() == passName) {
        const std::lock_guard<std::mutex> lock(_bakeMeshMutex);
        if (_bakeMeshSettings == bake) {
          ConvertBakeNormals(_bakeMesh,
                             tile,
                             data,
                             int(channels),
                             BakeNormalSpace(int(_renderParam->bakeNormalSpace)));
        }
        return;
      }
    }
  };
  const auto bakeMargin = [&](float *data, const size_t channels) {
    if (!bakeCoverage.empty()) {
      ExtendBakeMargin(
          data, tile.size.x, tile.size.y, int(channels), bakeCoverage, _renderParam->bakeMargin);
    }
  };

  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindings()) {
    if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(aovBinding.renderBuffer)) {
      if (aovBinding == displayAovBinding ? skipDisplayAov : skipOtherAovs) {
        continue;
      }

      // Hydra must not resize the buffer between the size check and the write
      const auto lock = renderBuffer->LockData();
      const HdFormat format = renderBuffer->GetFormat();
      if (format == HdFormatInvalid) {
        continue;  // Skip invalid AOV bindings
      }

      const size_t channels = HdGetComponentCount(format);
      // Avoid extra copy by mapping render buffer directly when dimensions/format match the tile
      if (tile.offset.x == 0 && tile.offset.y == 0 && tile.size.x == renderBuffer->GetWidth() &&
          tile.size.y == renderBuffer->GetHeight() &&
          (format >= HdFormatFloat32 && format <= HdFormatFloat32Vec4))
      {
        float *const data = static_cast<float *>(renderBuffer->Map());
        TF_VERIFY(
            tile.get_pass_pixels(_renderParam->GetPassName(aovBinding.aovName), channels, data));
        convert_units(aovBinding.aovName, data, tile.size.x * tile.size.y, channels);
        bakeNormals(aovBinding.aovName, data, channels);
        bakeMargin(data, channels);
        renderBuffer->Unmap();
      }
      else {
        pixels.resize(channels * tile.size.x * tile.size.y);
        if (tile.get_pass_pixels(
                _renderParam->GetPassName(aovBinding.aovName), channels, pixels.data()))
        {
          convert_units(aovBinding.aovName, pixels.data(), tile.size.x * tile.size.y, channels);
          // Integer buffers hold IDs (Hydra's primId/instanceId/elementId, Karma's primid)
          const bool isId = format == HdFormatInt32;
          if (!isId) {
            bakeNormals(aovBinding.aovName, pixels.data(), channels);
            bakeMargin(pixels.data(), channels);
          }
          renderBuffer->Map();
          renderBuffer->WritePixels(pixels.data(),
                                    GfVec2i(tile.offset.x, tile.offset.y),
                                    GfVec2i(tile.size.x, tile.size.y),
                                    channels,
                                    isId);
          renderBuffer->Unmap();
        }
        else {
          // Do not warn on missing elementId, which is a standard AOV but is not implemented
          if (aovBinding.aovName != HdAovTokens->elementId) {
            TF_RUNTIME_ERROR("Could not find pass for AOV '%s'", aovBinding.aovName.GetText());
          }
        }
      }
    }
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
