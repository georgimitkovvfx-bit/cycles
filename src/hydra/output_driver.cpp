/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/output_driver.h"
#include "hydra/render_buffer.h"
#include "hydra/session.h"

#include "session/session.h"
#include "util/time.h"

#include <algorithm>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

// Cycles writes the distance along the camera axis in meters (1e10 for background), Hydra expects
// window depth in 0..1 like an OpenGL depth buffer. Houdini uses it for the tumble pivot.
void convert_depth(float *depth,
                   const size_t num_pixels,
                   const GfMatrix4d &projection,
                   const double metersPerUnit)
{
  for (size_t i = 0; i < num_pixels; ++i) {
    if (depth[i] >= 1e9f) {
      depth[i] = 1.0f;
      continue;
    }
    // Point on the view axis at that distance (camera looks down -Z), in stage units
    const double z = -depth[i] / metersPerUnit;
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
  update_render_buffers(tile, true);

  // Update convergence state of all render buffers
  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindings()) {
    if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(aovBinding.renderBuffer)) {
      renderBuffer->SetConverged(true);
    }
  }
}

bool HdCyclesOutputDriver::update_render_tile(const Tile &tile)
{
  update_render_buffers(tile, false);
  return true;
}

void HdCyclesOutputDriver::update_render_buffers(const Tile &tile, const bool final)
{
  std::vector<float> pixels;

  // The Cycles display driver updates the display AOV texture while rendering. The final
  // image also goes to the buffer, which Houdini reads for slap comps and snapshots.
  const HdRenderPassAovBinding displayAovBinding = _renderParam->GetDisplayAovBinding();
  const auto *const displayBuffer = static_cast<HdCyclesRenderBuffer *>(
      displayAovBinding.renderBuffer);
  const bool skipDisplayAov = !final && displayBuffer && displayBuffer->IsResourceUsed();

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

  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindings()) {
    if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(aovBinding.renderBuffer)) {
      if (aovBinding == displayAovBinding ? skipDisplayAov : skipOtherAovs) {
        continue;
      }

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
        if (aovBinding.aovName == HdAovTokens->depth && channels == 1 &&
            _renderParam->IsViewport()) {
          convert_depth(data,
                        tile.size.x * tile.size.y,
                        _renderParam->GetProjectionMatrix(),
                        _renderParam->GetStageMetersPerUnit());
        }
        renderBuffer->Unmap();
      }
      else {
        pixels.resize(channels * tile.size.x * tile.size.y);
        if (tile.get_pass_pixels(
                _renderParam->GetPassName(aovBinding.aovName), channels, pixels.data()))
        {
          if (aovBinding.aovName == HdAovTokens->depth && channels == 1 &&
            _renderParam->IsViewport()) {
            convert_depth(pixels.data(),
                          tile.size.x * tile.size.y,
                          _renderParam->GetProjectionMatrix(),
                          _renderParam->GetStageMetersPerUnit());
          }
          // Integer buffers hold IDs (Hydra's primId/instanceId/elementId, Karma's primid)
          const bool isId = format == HdFormatInt32;
          renderBuffer->Map();
          renderBuffer->WritePixels(pixels.data(),
                                    GfVec2i(tile.offset.x, tile.offset.y),
                                    GfVec2i(tile.size.x, tile.size.y),
                                    channels,
                                    isId);
          renderBuffer->Unmap();
        }
        else {
          // Do not warn on missing elementId, which is a standard AOV but is not implememted
          if (aovBinding.aovName != HdAovTokens->elementId) {
            TF_RUNTIME_ERROR("Could not find pass for AOV '%s'", aovBinding.aovName.GetText());
          }
        }
      }
    }
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
