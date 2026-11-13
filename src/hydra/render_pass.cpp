/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/render_pass.h"
#include "hydra/camera.h"
#include "hydra/output_driver.h"
#include "hydra/render_buffer.h"
#include "hydra/render_delegate.h"
#include "hydra/session.h"

#ifdef WITH_HYDRA_DISPLAY_DRIVER
#  include "hydra/display_driver.h"
#endif

#include "scene/camera.h"
#include "scene/integrator.h"
#include "scene/scene.h"

#include "session/session.h"

#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPassState.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdCyclesRenderPass::HdCyclesRenderPass(HdRenderIndex *index,
                                       const HdRprimCollection &collection,
                                       HdCyclesSession *renderParam)
    : HdRenderPass(index, collection), _renderParam(renderParam)
{
  Session *const session = _renderParam->session;
  // Reset cancel state so session thread can continue rendering
  session->progress.reset();

  _renderParam->EnableResync();

  session->set_output_driver(make_unique<HdCyclesOutputDriver>(renderParam));

  const auto *const renderDelegate = static_cast<const HdCyclesDelegate *>(
      GetRenderIndex()->GetRenderDelegate());
  if (renderDelegate->IsDisplaySupported()) {
#ifdef WITH_HYDRA_DISPLAY_DRIVER
    session->set_display_driver(
        make_unique<HdCyclesDisplayDriver>(renderParam, renderDelegate->GetHgi()));
#endif
  }
}

HdCyclesRenderPass::~HdCyclesRenderPass()
{
  Session *const session = _renderParam->session;
  session->cancel(true);
}

bool HdCyclesRenderPass::IsConverged() const
{
  /* Changed settings or re-synced prims restart the render at the next execute. */
  if (GetRenderIndex()->GetRenderDelegate()->GetRenderSettingsVersion() != _lastSettingsVersion ||
      _renderParam->IsResyncing() || _renderParam->sceneChanged)
  {
    return false;
  }

  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindings()) {
    if (aovBinding.renderBuffer && !aovBinding.renderBuffer->IsConverged()) {
      return false;
    }
  }

  return true;
}

void HdCyclesRenderPass::_Execute(const HdRenderPassStateSharedPtr &renderPassState,
                                  const TfTokenVector & /*renderTags*/)
{
  Scene *const scene = _renderParam->session->scene.get();
  Session *const session = _renderParam->session;

  if (session->progress.get_cancel()) {
    return;  // Something went wrong and cannot continue without recreating the session
  }

  if (scene->mutex.try_lock()) {
    auto *const renderDelegate = static_cast<HdCyclesDelegate *>(
        GetRenderIndex()->GetRenderDelegate());

    const unsigned int settingsVersion = renderDelegate->GetRenderSettingsVersion();

    // Update requested AOV bindings
    const HdRenderPassAovBindingVector &aovBindings = renderPassState->GetAovBindings();
    if (_renderParam->GetAovBindings() != aovBindings ||
        // Need to resync passes when denoising is enabled or disabled to update the pass mode
        (settingsVersion != _lastSettingsVersion && scene->integrator->use_denoise_is_modified()))
    {
      if (_renderParam->SyncAovBindings(aovBindings)) {
        // Every shader writes the primvar AOVs: rebuild the shaders, then export the primvars
        // the geometry did not need before.
        _renderParam->RequestResync(true, true);
        _renderParam->UpdateDefaultSurface();
      }
    }

    bool displayChanged = false;
    if (renderDelegate->IsDisplaySupported()) {
      /* The display driver draws the first requested color AOV into an OpenGL texture. Houdini
       * draws the environment map of a dome light over AOVs given as textures (its viewport
       * shows only the dome then), so with a dome light the output driver writes that AOV
       * like the others, as Houdini expects from Karma. */
      // From the render index: the scene's light state lags one reset behind
      const bool hasDomeLight = !GetRenderIndex()
                                     ->GetSprimSubtree(HdPrimTypeTokens->domeLight,
                                                       SdfPath::AbsoluteRootPath())
                                     .empty();
      HdRenderPassAovBinding displayAovBinding;
      if (!aovBindings.empty() && aovBindings.front().aovName == HdAovTokens->color &&
          aovBindings.front().renderBuffer && !hasDomeLight)
      {
        displayAovBinding = aovBindings.front();
      }
      const HdRenderPassAovBinding previous = _renderParam->GetDisplayAovBinding();
      if (displayAovBinding != previous) {
        if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(previous.renderBuffer))
        {
          renderBuffer->SetResource(VtValue());
        }
        _renderParam->SetDisplayAovBinding(displayAovBinding);
        // The new target got none of the updates so far (black once the render converged)
        displayChanged = true;
      }
    }

    // Update camera dimensions to the viewport size
    CameraUtilFraming framing = renderPassState->GetFraming();
    // husk passes no framing: its viewport is the data window (dataWindowNDC) inside the render
    // settings resolution, in OpenGL's convention (y up), and the render buffers have its size.
    const GfVec2i resolution = renderDelegate
                                   ->GetRenderSetting(HdCyclesRenderSettingsTokens->resolution)
                                   .GetWithDefault<GfVec2i>(GfVec2i(0));
    if (!framing.IsValid() && !_renderParam->IsViewport() && resolution[0] > 0 &&
        resolution[1] > 0)
    {
      const GfVec4f vp = renderPassState->GetViewport();
      if (vp[2] > 0.0f && vp[3] > 0.0f) {
        const GfVec2i size{int(vp[2]), int(vp[3])};
        framing = CameraUtilFraming(
            GfRange2f(GfVec2f(0.0f), GfVec2f(resolution)),
            GfRect2i(GfVec2i(int(vp[0]), resolution[1] - int(vp[1]) - size[1]), size[0], size[1]),
            renderDelegate->GetRenderSetting(HdCyclesRenderSettingsTokens->pixelAspectRatio)
                .GetWithDefault<float>(1.0f));
      }
    }

    float aspectRatio = 0.0f;
    if (!framing.IsValid()) {
      const GfVec4f vp = renderPassState->GetViewport();
      GfVec2i size(static_cast<int>(vp[2]), static_cast<int>(vp[3]));

      // The render buffers can have a different size than the viewport. Houdini binds buffers
      // the size of the whole viewer while looking through a camera, sets the viewport to the
      // size of the camera gate, and stretches the buffers over the gate. Render at the buffer
      // size, since the display only draws into a buffer of the same size as the render, with
      // the camera window conformed to the aspect ratio of the viewport.
      for (const HdRenderPassAovBinding &aovBinding : aovBindings) {
        if (aovBinding.renderBuffer && aovBinding.renderBuffer->GetWidth() != 0 &&
            aovBinding.renderBuffer->GetHeight() != 0)
        {
          const GfVec2i bufferSize(int(aovBinding.renderBuffer->GetWidth()),
                                   int(aovBinding.renderBuffer->GetHeight()));
          if (bufferSize != size && vp[2] > 0.0f && vp[3] > 0.0f) {
            aspectRatio = vp[2] / vp[3];
            size = bufferSize;
          }
          break;
        }
      }

      framing = CameraUtilFraming(GfRect2i(GfVec2i(0), size[0], size[1]));
    }
    else {
      // The camera window is conformed to the display window, with non-square pixels
      aspectRatio = framing.pixelAspectRatio * framing.displayWindow.GetSize()[0] /
                    framing.displayWindow.GetSize()[1];
    }

    // The camera spans the display window, the render covers the data window (crop) inside it
    const GfVec2i displaySize(int(framing.displayWindow.GetSize()[0]),
                              int(framing.displayWindow.GetSize()[1]));
    scene->camera->set_full_width(displaySize[0]);
    scene->camera->set_full_height(displaySize[1]);

    if (const auto *const camera = static_cast<const HdCyclesCamera *>(
            renderPassState->GetCamera()))
    {
      camera->ApplyCameraSettings(_renderParam, scene->camera, aspectRatio);
    }
    else {
      HdCyclesCamera::ApplyCameraSettings(_renderParam,
                                          renderPassState->GetWorldToViewMatrix(),
                                          renderPassState->GetProjectionMatrix(),
                                          renderPassState->GetClipPlanes(),
                                          scene->camera,
                                          aspectRatio);
    }

    _renderParam->SetProjectionMatrix(renderPassState->GetProjectionMatrix());

    // Reset session if the session, scene, camera or AOV bindings changed. The session thread may
    // have updated the scene since the sync changed it (sceneChanged).
    const bool sceneChanged = _renderParam->sceneChanged.exchange(false);
    if (scene->need_reset() || sceneChanged || settingsVersion != _lastSettingsVersion ||
        displayChanged)
    {
      _lastSettingsVersion = settingsVersion;

      // Reset convergence state of all render buffers
      _renderParam->ResetConverged();

      // The buffer holds the data window, placed at its offset in the camera's raster (the
      // display window). The window is the whole buffer: Cycles' window is the part of the
      // buffer that is not overscan.
      BufferParams buffer_params;
      buffer_params.full_x = framing.dataWindow.GetMinX() -
                             static_cast<int>(framing.displayWindow.GetMin()[0]);
      // Cycles' raster y goes up, the windows' y goes down
      buffer_params.full_y = displaySize[1] - framing.dataWindow.GetHeight() -
                             (framing.dataWindow.GetMinY() -
                              static_cast<int>(framing.displayWindow.GetMin()[1]));
      buffer_params.full_width = displaySize[0];
      buffer_params.full_height = displaySize[1];

      buffer_params.window_x = 0;
      buffer_params.window_y = 0;
      buffer_params.window_width = framing.dataWindow.GetWidth();
      buffer_params.window_height = framing.dataWindow.GetHeight();

      buffer_params.width = buffer_params.window_width;
      buffer_params.height = buffer_params.window_height;

      session->reset(session->params, buffer_params);
    }

    scene->mutex.unlock();

    // Start Cycles render thread if not already running
    session->start();

    renderDelegate->SendResync();
  }

  session->draw();
}

void HdCyclesRenderPass::_MarkCollectionDirty() {}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
