/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/render_pass.h"
#include "hydra/bake.h"
#include "hydra/camera.h"
#include "hydra/geometry.h"
#include "hydra/output_driver.h"
#include "hydra/render_buffer.h"
#include "hydra/render_delegate.h"
#include "hydra/session.h"

#ifdef WITH_HYDRA_DISPLAY_DRIVER
#  include "hydra/display_driver.h"
#endif

#include "scene/bake.h"
#include "scene/camera.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "scene/scene.h"

#include "session/session.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/collectionExpressionEvaluator.h>
#include <pxr/imaging/hd/collectionsSchema.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/utils.h>

#include <cstring>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

/* The rprims of Selected to Active bake sources: prim paths and their descendants (a group), or
 * USD collections "/prim.collection:name", separated by spaces. */
std::vector<std::string> ResolveBakeSources(HdRenderIndex *renderIndex, const std::string &sources)
{
  static const char *const collectionSeparator = ".collection:";
  const SdfPathVector rprims = renderIndex->GetRprimIds();
  const HdSceneIndexBaseRefPtr sceneIndex = renderIndex->GetTerminalSceneIndex();
  std::vector<std::string> prims;
  for (const std::string &entry : TfStringTokenize(sources)) {
    const size_t collection = entry.find(collectionSeparator);
    if (collection == std::string::npos) {
      const SdfPath path(entry);
      if (!path.IsAbsolutePath()) {
        continue;
      }
      for (const SdfPath &id : rprims) {
        if (id.HasPrefix(path)) {
          prims.push_back(id.GetString());
        }
      }
      continue;
    }
    const SdfPath path(entry.substr(0, collection));
    if (!sceneIndex || !path.IsAbsolutePath()) {
      continue;
    }
    const HdPathExpressionDataSourceHandle expression =
        HdCollectionsSchema::GetFromParent(sceneIndex->GetPrim(path).dataSource)
            .GetCollection(TfToken(entry.substr(collection + std::strlen(collectionSeparator))))
            .GetMembershipExpression();
    if (!expression) {
      continue;
    }
    const HdCollectionExpressionEvaluator evaluator(sceneIndex, expression->GetTypedValue(0.0f));
    for (const SdfPath &id : rprims) {
      if (evaluator.Match(id)) {
        prims.push_back(id.GetString());
      }
    }
  }
  return prims;
}

}  // namespace

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
  /* A batch render that failed (e.g. kernels or OSL shaders the device could not load) never
   * writes its result: it is done, so husk finishes instead of waiting for it forever. */
  const Session *const session = _renderParam->session;
  if (session->params.background && session->progress.get_error()) {
    return true;
  }

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
                                  const TfTokenVector &renderTags)
{
  Scene *const scene = _renderParam->session->scene.get();
  Session *const session = _renderParam->session;

  if (session->progress.get_cancel()) {
    return;  // Something went wrong and cannot continue without recreating the session
  }

  if (session->progress.get_error() && !_errorReported) {
    _errorReported = true;
    TF_RUNTIME_ERROR("Cycles render failed: %s", session->progress.get_error_message().c_str());
  }

  if (scene->mutex.try_lock()) {
    auto *const renderDelegate = static_cast<HdCyclesDelegate *>(
        GetRenderIndex()->GetRenderDelegate());

    const unsigned int settingsVersion = renderDelegate->GetRenderSettingsVersion();

    // Hydra syncs only the prims of the drawn render tags: hide the others, which keep their
    // Cycles objects (a prim whose purpose became guide, the viewer's purpose toggles). An empty
    // list draws every tag.
    HdRenderIndex *const renderIndex = GetRenderIndex();
    const unsigned int renderTagVersion = renderIndex->GetChangeTracker().GetRenderTagVersion();
    if (renderTags != _lastRenderTags || renderTagVersion != _lastRenderTagVersion) {
      _lastRenderTags = renderTags;
      _lastRenderTagVersion = renderTagVersion;
      for (const SdfPath &id : renderIndex->GetRprimIds()) {
        // The render index hands out const prims, the Cycles objects are this delegate's
        if (auto *const prim = dynamic_cast<HdCyclesRenderTagVisibility *>(
                const_cast<HdRprim *>(renderIndex->GetRprim(id))))
        {
          const TfToken tag = renderIndex->GetRenderTag(id);
          prim->SetRenderTagVisible(
              scene,
              renderTags.empty() ||
                  std::find(renderTags.begin(), renderTags.end(), tag) != renderTags.end());
        }
      }
    }

    // Update requested AOV bindings
    const HdRenderPassAovBindingVector &aovBindings = renderPassState->GetAovBindings();
    const bool bakeChanged = _renderParam->bakeChanged.exchange(false);
    if (_renderParam->GetAovBindings() != aovBindings ||
        // Need to resync passes when denoising is enabled or disabled to update the pass mode
        (settingsVersion != _lastSettingsVersion &&
         scene->integrator->use_denoise_is_modified()) ||
        // and for the bake input passes
        bakeChanged)
    {
      if (_renderParam->SyncAovBindings(aovBindings)) {
        // Every shader writes the primvar AOVs: rebuild the shaders, then export the primvars
        // the geometry did not need before.
        _renderParam->RequestResync(true, true);
        _renderParam->UpdateDefaultSurface();
      }
    }

    /* Texture baking: the bake target among the objects of this sync. */
    const BakeSettings bake = _renderParam->GetBake();
    if (!bake.object.empty() || scene->bake_manager->get_baking()) {
      SetBakeTarget(scene, bake);
      /* Again after scene changes: the version also stamps the sources' ray tracing BVH, which
       * the output driver keeps while it stays the same.
       * shortcut: collection edits on prims outside the render index bump no version; they apply
       * with the next scene change. */
      const unsigned int sceneVersion = renderIndex->GetChangeTracker().GetSceneStateVersion();
      const std::string sources = bake.useSources() ? bake.sources : std::string();
      if (sources != _lastBakeSources || sceneVersion != _lastBakeSourcesVersion) {
        _lastBakeSources = sources;
        _lastBakeSourcesVersion = sceneVersion;
        _renderParam->SetBakeSourcePrims(sources.empty() ?
                                             std::vector<std::string>() :
                                             ResolveBakeSources(renderIndex, sources),
                                         sceneVersion);
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

    float exposureScale = 1.0f;
    if (const auto *const camera = static_cast<const HdCyclesCamera *>(
            renderPassState->GetCamera()))
    {
      // husk sends the render settings' aspectRatioConformPolicy as a setting, unless the host
      // overrides the window policy of the camera
      CameraUtilConformWindowPolicy windowPolicy = renderPassState->GetWindowPolicy();
      const VtValue conform = renderDelegate->GetRenderSetting(
          HdCyclesRenderSettingsTokens->aspectRatioConformPolicy);
      if (!renderPassState->GetOverrideWindowPolicy() && !_renderParam->IsViewport()) {
        const TfToken policy = conform.IsHolding<TfToken>() ?
                                   conform.UncheckedGet<TfToken>() :
                                   TfToken(conform.GetWithDefault<std::string>(""));
        if (!policy.IsEmpty()) {
          windowPolicy = HdUtils::ToConformWindowPolicy(policy);
        }
      }
      camera->ApplyCameraSettings(_renderParam, scene->camera, windowPolicy, aspectRatio);
      exposureScale = camera->GetExposureScale();
    }
    else {
      HdCyclesCamera::ApplyCameraSettings(_renderParam,
                                          renderPassState->GetWorldToViewMatrix(),
                                          renderPassState->GetProjectionMatrix(),
                                          renderPassState->GetClipPlanes(),
                                          scene->camera,
                                          aspectRatio);
    }

    scene->film->set_exposure(_renderParam->GetFilmExposure() * exposureScale);

    /* Adaptive subdivision dices in pixels of the render camera, like Blender's default dicing
     * camera, or of the Dicing Camera setting's camera (with the render resolution). Only
     * tessellation reads it, so changes do not restart the render. */
    for (const SocketType &socket : scene->camera->type->inputs) {
      // Setting an array takes it from the camera; dicing needs none (motion, shutter curve)
      if (!socket.is_array()) {
        scene->dicing_camera->set_value(socket, *scene->camera, socket);
      }
    }
    const std::string &dicingCameraPath = _renderParam->GetDicing().camera;
    if (!dicingCameraPath.empty() && SdfPath::IsValidPathString(dicingCameraPath)) {
      if (const auto *const dicingCamera = static_cast<const HdCyclesCamera *>(
              GetRenderIndex()->GetSprim(HdPrimTypeTokens->camera, SdfPath(dicingCameraPath))))
      {
        dicingCamera->ApplyCameraSettings(
            _renderParam, scene->dicing_camera, CameraUtilFit, aspectRatio);
      }
    }
    scene->dicing_camera->set_offscreen_dicing_scale(_renderParam->GetDicing().offscreenScale);

    _renderParam->SetProjectionMatrix(renderPassState->GetProjectionMatrix());

    // Reset session if the session, scene, camera or AOV bindings changed. The session thread may
    // have updated the scene since the sync changed it (sceneChanged).
    const bool sceneChanged = _renderParam->sceneChanged.exchange(false);
    // A hold (geometry changed, SceneLock) cancelled the samples in flight: reset so that they
    // are rendered again
    const bool held = session->render_held();
    const bool reset = scene->need_reset() || sceneChanged ||
                       settingsVersion != _lastSettingsVersion || displayChanged || held;
    if (reset) {
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
      // Background sessions do not restart the clock at a reset (total time per husk frame)
      if (session->params.background) {
        session->progress.set_start_time();
      }
    }

    // Hydra's sync of this frame is done: the session thread updates the device scene, then
    // renders
    if (held) {
      session->release_render_hold();
    }

    scene->mutex.unlock();

    // Start Cycles render thread if not already running. A background session's thread stops
    // when the render is done: start it again only until the new render is done (restarting a
    // finished one resets its progress). Not just at the reset: the thread may have been
    // finishing the previous render then.
    if (!session->params.background || reset || !IsConverged()) {
      session->start();
    }

    renderDelegate->SendResync();
  }

  session->draw();
}

void HdCyclesRenderPass::_MarkCollectionDirty() {}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
