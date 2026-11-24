/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "util/thread.h"

#include <mutex>
#include <optional>

#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/pathExpression.h>

#include <unordered_map>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

struct SceneLock {
  /* `holdRender`: the change deletes or rewrites geometry, see the constructor. */
  SceneLock(const PXR_NS::HdRenderParam *renderParam, bool holdRender = false);
  ~SceneLock();

  CCL_NS::Scene *scene;

 private:
  CCL_NS::thread_scoped_lock sceneLock;
};

class HdCyclesSession final : public PXR_NS::HdRenderParam {
 public:
  HdCyclesSession(CCL_NS::Session *session_, const bool keep_nodes);
  HdCyclesSession(const CCL_NS::SessionParams &params);
  ~HdCyclesSession() override;

  void UpdateScene();

  /* The surface of geometry without a material: Blender's default (a Principled BSDF, as native Cycles renders an
   * object without a material) with the object's colour (``displayColor``, else 0.8) as its base colour. Set by the
   * session that owns its Cycles session; ``cycles_usd_render`` sets it on its own. */
  static void SetDefaultSurface(CCL_NS::Scene *scene);

  double GetStageMetersPerUnit() const
  {
    return _stageMetersPerUnit;
  }

  /* `cycles:viewport:window_depth`: the `depth` AOV as window depth (0..1, like an OpenGL depth buffer), which a
   * viewport compositing Hydra's depth reads (UsdImagingGL). Off by default: final renders keep Cycles' distance
   * (Blender's Z pass). After hdCycles-h22 (Houdini's viewport), commit 14fb1d2c8. */
  bool GetDepthToWindow() const
  {
    return _depthToWindow;
  }

  void SetDepthToWindow(const bool depthToWindow)
  {
    _depthToWindow = depthToWindow;
  }

  /* The projection of the camera being rendered, to convert the depth pass (set by the render pass). */
  PXR_NS::GfMatrix4d GetProjectionMatrix() const
  {
    const std::lock_guard<std::mutex> lock(_projectionMutex);
    return _projectionMatrix;
  }

  void SetProjectionMatrix(const PXR_NS::GfMatrix4d &projectionMatrix)
  {
    const std::lock_guard<std::mutex> lock(_projectionMutex);
    _projectionMatrix = projectionMatrix;
  }

  void SetStageMetersPerUnit(const double stageMetersPerUnit)
  {
    _stageMetersPerUnit = stageMetersPerUnit;
  }

  PXR_NS::HdRenderPassAovBinding GetDisplayAovBinding() const
  {
    return _displayAovBinding;
  }

  void SetDisplayAovBinding(const PXR_NS::HdRenderPassAovBinding &aovBinding)
  {
    _displayAovBinding = aovBinding;
  }

  const PXR_NS::HdRenderPassAovBindingVector &GetAovBindings() const
  {
    return _aovBindings;
  }

  void SyncAovBindings(const PXR_NS::HdRenderPassAovBindingVector &aovBindings);

  /* `cycles:background:transparent` (Blender's Film › Transparent): -1 = not set, the default depends on
   * whether the stage has a dome light. */
  void SetTransparentBackground(const int transparent)
  {
    _backgroundDirty = _backgroundDirty || transparent != _transparentBackground;
    _transparentBackground = transparent;
  }

  /* Karma's `renderlightgeo` on the dome light: the sky seen by the camera (nullopt = the stage does not say). A
   * hidden dome leaves the background transparent, as in Karma. */
  void SetDomeCameraVisible(const std::optional<bool> visible)
  {
    _backgroundDirty = _backgroundDirty || visible != _domeCameraVisible;
    _domeCameraVisible = visible;
  }

  /* `cycles:background:camera_visible_default`: when the dome does not say. Karma's default (hidden) unless the host
   * asks for its own: the Blender add-on asks for Blender's (a World is seen by the camera). */
  void SetDomeCameraVisibleDefault(const bool visible)
  {
    _backgroundDirty = _backgroundDirty || visible != _domeCameraVisibleDefault;
    _domeCameraVisibleDefault = visible;
  }

  void RemoveAovBinding(PXR_NS::HdRenderBuffer *renderBuffer);

  /* Motion blur: the render camera's shutter (frames, relative to the current frame), set when the camera syncs
   * (before the rprims); `cycles:motion_blur` can turn it off. On and open: 3 motion steps, evenly over the shutter
   * (Cycles' layout), else none. */
  void SetShutter(const float open, const float close)
  {
    _shutterOpen = open;
    _shutterClose = close;
  }
  /* Set by a host that never renders while Hydra syncs (cycles_usd_render): each rprim then fills its own geometry
   * outside the scene lock, so Hydra's parallel sync is parallel. Interactive hosts (Blender's viewport) leave it
   * off: the session thread may update the scene meanwhile. */
  /* Hydra's `instanceId` AOV: an attribute on every instance object and an AOV output in every material. Hosts that
   * never read it (cycles_usd_render) turn it off: with millions of instances the per-object attributes cost
   * memory and an attribute repack on every change. */
  void SetInstanceIdAov(const bool enabled)
  {
    _instanceIdAov = enabled;
  }
  bool InstanceIdAov() const
  {
    return _instanceIdAov;
  }

  void SetExclusiveSync(const bool exclusive)
  {
    _exclusiveSync = exclusive;
  }
  bool ExclusiveSync() const
  {
    return _exclusiveSync;
  }
  void SetMotionBlurAllowed(const bool allowed)
  {
    _motionBlurAllowed = allowed;
  }
  int MotionSteps() const
  {
    return (_motionBlurAllowed && _shutterClose > _shutterOpen) ? 3 : 0;
  }
  float ShutterOpen() const
  {
    return _shutterOpen;
  }
  float ShutterClose() const
  {
    return _shutterClose;
  }
  /* Shutter offset (frames) of a time-ordered motion step. */
  float MotionTime(const int step) const
  {
    const int steps = MotionSteps();
    return steps > 1 ? _shutterOpen + (_shutterClose - _shutterOpen) * float(step) / float(steps - 1) : 0.0f;
  }

  /* Light and shadow linking (patch 0018): USD's collection:lightLink / shadowLink, a membership expression per light,
   * matched against every geometry prim's path. A link that reaches all geometry is no link. Every distinct set of
   * linked lights reaching a prim becomes a Cycles receiver (blocker) set; a linked light reaches its sets. The sets
   * are recomputed in UpdateScene when lights or geometry changed. ``objects``: the prim's instance objects (its own
   * vector, read at UpdateScene). */
  void SetLinkGeometry(const void *owner, const PXR_NS::SdfPath &id, const std::vector<CCL_NS::Object *> *objects);
  void RemoveLinkGeometry(const void *owner);
  /* ``prefix``: where the host put the stage in Hydra (Blender: /usd_scene); the expressions are in USD paths. */
  void SetLightLinks(CCL_NS::Object *light,
                     const PXR_NS::HdSceneIndexBaseRefPtr &sceneIndex,
                     const PXR_NS::SdfPath &prefix,
                     const PXR_NS::SdfPathExpression &lightLink,
                     const PXR_NS::SdfPathExpression &shadowLink);
  void RemoveLightLinks(CCL_NS::Object *light);

  /* Cycles never deletes a Shader (Scene::delete_node: "not supported", shader ids are baked into the kernel's
   * data). A host that re-creates its prims on every sync (Blender's Stage View) added a shader with its whole graph
   * per material and light each time: memory grew with every edit. A retired shader gets an empty graph and its
   * defaults back and is handed out again. Call both under the scene lock. */
  CCL_NS::Shader *TakeShader(CCL_NS::Scene *scene);
  void RetireShader(CCL_NS::Scene *scene, CCL_NS::Shader *shader);

  CCL_NS::Session *session;
  bool keep_nodes;

 private:
  std::vector<CCL_NS::Shader *> _retiredShaders;

  void UpdateLinks();

  struct LinkedGeometry {
    PXR_NS::SdfPath id;
    const std::vector<CCL_NS::Object *> *objects;
  };
  struct LightLinks {
    PXR_NS::SdfPathExpression light;
    PXR_NS::SdfPathExpression shadow;
  };
  std::unordered_map<const void *, LinkedGeometry> _linkedGeometry;
  std::unordered_map<CCL_NS::Object *, LightLinks> _lightLinks;
  PXR_NS::HdSceneIndexBaseRefPtr _linkSceneIndex;
  PXR_NS::SdfPath _linkPrefix = PXR_NS::SdfPath::AbsoluteRootPath();
  bool _linksDirty = false;
  bool _linksApplied = false; /* some object or light holds a non-default set */
  std::mutex _linksMutex; /* rprims sync in parallel (ExclusiveSync) */

  const bool _ownCyclesSession;
  double _stageMetersPerUnit = 0.01;
  bool _depthToWindow = false;
  PXR_NS::GfMatrix4d _projectionMatrix = PXR_NS::GfMatrix4d(1.0);
  mutable std::mutex _projectionMutex; /* written by the render pass, read by the output driver's thread */
  PXR_NS::HdRenderPassAovBindingVector _aovBindings;
  PXR_NS::HdRenderPassAovBinding _displayAovBinding;
  float _shutterOpen = 0.0f;
  float _shutterClose = 0.0f;
  bool _motionBlurAllowed = true;
  bool _exclusiveSync = false;
  bool _instanceIdAov = true;
  int _transparentBackground = -1;
  std::optional<bool> _domeCameraVisible;
  bool _domeCameraVisibleDefault = false;
  bool _backgroundDirty = false; /* the setting changed: update the background once */
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
