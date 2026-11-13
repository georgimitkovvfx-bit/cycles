/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "scene/pass.h"
#include "util/thread.h"

#include <pxr/imaging/hd/renderDelegate.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesSession;

struct SceneLock {
  SceneLock(const PXR_NS::HdRenderParam *renderParam);
  ~SceneLock();

  CCL_NS::Scene *scene;

 private:
  HdCyclesSession *session;
  CCL_NS::thread_scoped_lock sceneLock;
};

class HdCyclesSession final : public PXR_NS::HdRenderParam {
 public:
  HdCyclesSession(CCL_NS::Session *session_, const bool keep_nodes);
  HdCyclesSession(const CCL_NS::SessionParams &params, const CCL_NS::SceneParams &scene_params);
  ~HdCyclesSession() override;

  void UpdateScene();

  // Cycles pass for a Hydra AOV or Cycles pass name, PASS_NONE if there is none
  static CCL_NS::PassType GetPassType(const PXR_NS::TfToken &aovName);

  // Override the automatic background transparency, which depends on presence of a dome light
  void SetTransparentBackground(const std::optional<bool> transparent);

  // Light and shadow linking: the link categories of each light and the categories each object
  // belongs to (from HdsiLightLinkingSceneIndex), by object name. UpdateScene turns them into
  // Cycles light and shadow sets. Called with the scene locked.
  // shortcut: per-instance categories of point instancers are ignored (instances use the
  // prototype's), read HdInstanceCategoriesSchema if instances need their own links.
  void SetLightLinks(const std::string &light,
                     const PXR_NS::TfToken &lightLink,
                     const PXR_NS::TfToken &shadowLink);
  void SetCategories(const std::string &object,
                     const PXR_NS::VtArray<PXR_NS::TfToken> &categories);
  void RemoveLinking(const std::string &name);
  // Objects were added: they need their sets
  void TagLightLinking()
  {
    _lightLinkingDirty = true;
  }

  double GetStageMetersPerUnit() const
  {
    return _stageMetersPerUnit;
  }

  void SetStageMetersPerUnit(const double stageMetersPerUnit)
  {
    _stageMetersPerUnit = stageMetersPerUnit;
  }

  // Projection of the camera being rendered, to convert the depth pass to Hydra's depth AOV
  PXR_NS::GfMatrix4d GetProjectionMatrix() const
  {
    const CCL_NS::thread_scoped_lock lock(_projectionMutex);
    return _projectionMatrix;
  }

  void SetProjectionMatrix(const PXR_NS::GfMatrix4d &projectionMatrix)
  {
    const CCL_NS::thread_scoped_lock lock(_projectionMutex);
    _projectionMatrix = projectionMatrix;
  }

  // Shutter interval in frames, relative to the current frame: from the shutter:open/close render
  // settings (the Houdini viewport and husk), else from the first camera with an open shutter,
  // since geometry is synced before the render pass knows its camera.
  void SetShutterSetting(const bool close, const float value)
  {
    (close ? _shutterClose : _shutterOpen) = value;
    _shutterFromSettings = true;
  }

  void SetCameraShutter(const float open, const float close)
  {
    if (!_shutterFromSettings && !(_shutterClose > _shutterOpen)) {
      _shutterOpen = open;
      _shutterClose = close;
    }
  }

  void SetMotionBlurDisabled(const bool disabled)
  {
    _motionBlurDisabled = disabled;
  }

  // Blender's motion steps of objects: 2^steps + 1 motion times over the shutter
  void SetMotionSteps(const int steps)
  {
    _motionSteps = std::clamp(steps, 1, 7);
  }

  // Frame rate from the application (houdini:fps), 0 if it is unknown
  void SetFramesPerSecond(const double fps)
  {
    _framesPerSecond = std::max(fps, 0.0);
  }

  double GetFramesPerSecond() const
  {
    return _framesPerSecond;
  }

  // Times in frames of the motion steps over the shutter when rendering with motion blur, else
  // empty. The steps are evenly spaced, as Cycles expects, the center one at mid-shutter. Without
  // motion blur, the motion pass needs the previous and next frame.
  std::vector<float> GetMotionTimes() const
  {
    return GetMotionTimes(_shutterOpen, _shutterClose);
  }
  std::vector<float> GetMotionTimes(float shutterOpen, float shutterClose) const;

  // Rendering for the Houdini viewport: it reads the depth AOV as window depth (0..1) and needs
  // integer IDs, final renders keep distance and float IDs
  bool IsViewport() const
  {
    return _viewport;
  }

  void SetViewport(const bool viewport)
  {
    _viewport = viewport;
  }

  /* The Cycles session thread writes the AOVs (output driver) while Hydra changes the bindings
   * and deletes render buffers: the output driver holds this while using the bound buffers. */
  std::unique_lock<std::recursive_mutex> LockAovBindings() const
  {
    return std::unique_lock(_aovBindingsMutex);
  }

  PXR_NS::HdRenderPassAovBinding GetDisplayAovBinding() const
  {
    const auto lock = LockAovBindings();
    return _displayAovBinding;
  }

  void SetDisplayAovBinding(const PXR_NS::HdRenderPassAovBinding &aovBinding)
  {
    const auto lock = LockAovBindings();
    _displayAovBinding = aovBinding;
  }

  PXR_NS::HdRenderPassAovBindingVector GetAovBindings() const
  {
    const auto lock = LockAovBindings();
    return _aovBindings;
  }

  // Returns true when the primvar AOVs changed: materials then need to be rebuilt, and geometry
  // to export the primvars.
  bool SyncAovBindings(const PXR_NS::HdRenderPassAovBindingVector &aovBindings);

  // Primvar AOVs as (Cycles pass name, primvar name): every shader writes the primvar to the pass
  const std::vector<std::pair<std::string, std::string>> &GetPrimvarAovs() const
  {
    return _primvarAovs;
  }

  // Primvar AOVs of a render settings prim, synced before shaders are built, with the scene
  // locked. The active render settings are used, or all of them when none is active (husk).
  void SetRenderSettingsPrimvarAovs(
      const PXR_NS::SdfPath &id,
      bool active,
      std::vector<std::pair<std::string, std::string>> &&primvarAovs);

  // Rebuild the default surface shader with the primvar AOVs, with the scene locked
  void UpdateDefaultSurface();

  // Whether a render settings prim has a motion pass render var, synced before geometry so that
  // it is sampled at the motion pass times, with the scene locked. Like the primvar AOVs, only the
  // active render settings count, or all of them when none is active.
  void SetRenderSettingsMotionPass(const PXR_NS::SdfPath &id, bool active, bool motionPass);

  // Primvar a render var reads: source type "primvar" or a "primvars:<name>" source name, unless
  // it is a Cycles pass (Karma's "st" is the UV pass). Empty if none.
  static std::string GetPrimvar(const std::string &sourceName, const PXR_NS::TfToken &sourceType);

  // Name of the Cycles pass holding an AOV, which can be shared with other AOVs
  std::string GetPassName(const PXR_NS::TfToken &aovName) const;

  void RemoveAovBinding(PXR_NS::HdRenderBuffer *renderBuffer);

  // Mark the render buffers as not converged, so that the render pass is executed again
  void ResetConverged();

  // With scene indices, Hydra only syncs a prim again when the scene changes it, not when the
  // render delegate marks it dirty. Changes only the delegate knows about request a sync here,
  // sent by HdCyclesDelegate::SendResync() outside of syncs, for the next sync: materials are
  // rebuilt, rprims and cameras read their transforms and primvars again.
  void RequestResync(bool materials, bool rprims);

  // Requests before the first render pass come from the first sync, which syncs the prims they
  // concern after them (render settings, cameras and materials before geometry)
  void EnableResync()
  {
    _resyncEnabled = true;
  }

  // Settings fixed when the Cycles session is created can arrive after the render delegate was
  // created (Houdini's viewport). Replace the session for them while nothing uses it yet.
  bool IsSessionUnused() const;
  void RecreateSession(const CCL_NS::SessionParams &params,
                       const CCL_NS::SceneParams &scene_params);

  CCL_NS::Session *session;
  bool keep_nodes;

  std::atomic<bool> resyncMaterials = false;
  std::atomic<bool> resyncRprims = false;
  // Set from HdCyclesDelegate::SendResync() to CommitResources() when the next sync syncs every
  // rprim again: the materials and cameras synced before them need no other rprim sync
  std::atomic<bool> rprimsResyncing = false;
  // Prims were marked dirty for the next sync, which restarts the render
  std::atomic<bool> resyncPending = false;
  // Scene changes the render was not reset for yet, set by SceneLock with the scene locked. The
  // session thread updates the device and clears the update tags whenever it gets the lock, also
  // between a sync and the render pass that resets the render for it.
  std::atomic<bool> sceneChanged = false;

  // Whether a requested re-sync has not reached the render yet
  bool IsResyncing() const
  {
    return resyncMaterials || resyncRprims || resyncPending;
  }

 private:
  void CreateSession(const CCL_NS::SessionParams &params, const CCL_NS::SceneParams &scene_params);
  void UpdateLightLinking();

  std::unordered_map<std::string, std::pair<PXR_NS::TfToken, PXR_NS::TfToken>> _lightLinks;
  std::unordered_map<std::string, PXR_NS::VtArray<PXR_NS::TfToken>> _categories;
  bool _lightLinkingDirty = false;

  size_t _numDefaultShaders = 0;
  bool _resyncEnabled = false;
  const bool _ownCyclesSession;
  double _stageMetersPerUnit = 0.01;
  PXR_NS::GfMatrix4d _projectionMatrix = PXR_NS::GfMatrix4d(1.0);
  mutable CCL_NS::thread_mutex _projectionMutex;
  bool _viewport = false;
  float _shutterOpen = 0.0f;
  float _shutterClose = 0.0f;
  bool _shutterFromSettings = false;
  bool _motionBlurDisabled = false;
  int _motionSteps = 1;
  double _framesPerSecond = 0.0;
  std::optional<bool> _transparentBackground;
  mutable std::recursive_mutex _aovBindingsMutex;
  PXR_NS::HdRenderPassAovBindingVector _aovBindings;
  std::unordered_map<PXR_NS::TfToken, std::string, PXR_NS::TfToken::HashFunctor> _passNames;
  std::vector<std::pair<std::string, std::string>> _primvarAovs;
  std::map<PXR_NS::SdfPath, std::pair<bool, std::vector<std::pair<std::string, std::string>>>>
      _renderSettingsPrimvarAovs;
  // Render settings prim: (active, has a motion pass)
  std::map<PXR_NS::SdfPath, std::pair<bool, bool>> _motionPassSettings;
  bool _motionPassBound = false;
  PXR_NS::HdRenderPassAovBinding _displayAovBinding;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
