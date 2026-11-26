/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/bake.h"
#include "hydra/config.h"
#include "scene/pass.h"
#include "util/set.h"
#include "util/thread.h"
#include "util/transform.h"

#include <pxr/imaging/hd/renderDelegate.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesSession;

struct SceneLock {
  /* `holdRender`: the change deletes or rewrites geometry, see the constructor. */
  SceneLock(const PXR_NS::HdRenderParam *renderParam, bool holdRender = false);
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

  /* Queue node for deletion on the next UpdateScene(). Must be called with the scene locked. */
  void DeleteNode(CCL_NS::Object *node);
  void DeleteNode(CCL_NS::Geometry *node);
  void DeleteNode(CCL_NS::Shader *node);
  /* Cycles never deletes a Shader (Scene::delete_node: "not supported", shader ids are baked into the kernel's
   * data): a deleted one kept its whole graph, and hosts that make prims again (a LOP switch, Blender's Stage View)
   * added one per material and light each time. FlushDeletedNodes gives a deleted shader an empty graph and its
   * defaults back, and TakeShader hands it out before making a new one. Scene locked. From hdcycles-blender patch
   * 0022. */
  CCL_NS::Shader *TakeShader(CCL_NS::Scene *scene);

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
  /* Categories per instance object of a prim (an instancer's instanceCategories with the prim's own), which win
   * over the prim's; empty: none. Called with the scene locked. */
  void SetInstanceCategories(const std::string &object,
                             std::vector<std::pair<const CCL_NS::Object *, PXR_NS::VtArray<PXR_NS::TfToken>>>
                                 categories);
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

  // Whether the shutter (render settings, else the camera's) is open
  bool HasOpenShutter() const
  {
    return _shutterClose > _shutterOpen;
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
  // With motion blur, objects can have their own motion steps (0 is the Motion Steps setting),
  // or a number of motion samples (Karma's time samples, 0 uses the steps; 1 is no motion blur),
  // rounded up to an odd number so that one sample is at mid-shutter
  std::vector<float> GetMotionTimes(const int steps, const int samples = 0) const
  {
    return GetMotionTimes(_shutterOpen, _shutterClose, steps, samples);
  }
  std::vector<float> GetMotionTimes(float shutterOpen,
                                    float shutterClose,
                                    int steps = 0,
                                    int samples = 0) const;

  // Blender's Subdivision render settings: dicing rate in pixels (viewport and final renders),
  // max subdivision level of adaptive subdivision, offscreen dicing scale and the dicing camera
  // (camera prim path, empty for the render camera)
  struct DicingSettings {
    float rate = 1.0f;
    float previewRate = 8.0f;
    int maxLevel = 12;
    float offscreenScale = 4.0f;
    std::string camera;
    bool operator==(const DicingSettings &other) const = default;
  };
  const DicingSettings &GetDicing() const
  {
    return _dicing;
  }
  /* Texture baking (bake.h): the bake settings, the prim path of the bake target empty when not
   * baking. Changing them rebuilds the passes (HdCyclesRenderPass) and resyncs the geometry for
   * the UV map. */
  std::string GetBakeObject() const
  {
    const std::lock_guard<std::mutex> lock(_bakeMutex);
    return _bake.object;
  }
  BakeSettings GetBake() const
  {
    const std::lock_guard<std::mutex> lock(_bakeMutex);
    return _bake;
  }
  void SetBake(const BakeSettings &bake)
  {
    {
      const std::lock_guard<std::mutex> lock(_bakeMutex);
      if (bake == _bake) {
        return;
      }
      _bake = bake;
    }
    bakeChanged = true;
    RequestResync(false, true);
  }
  /* Selected to Active: the rprims the bake sources match, resolved by the render pass, and the
   * scene state version (HdChangeTracker) they were resolved at. */
  std::vector<std::string> GetBakeSourcePrims(unsigned int &version) const
  {
    const std::lock_guard<std::mutex> lock(_bakeMutex);
    version = _bakeSourcesVersion;
    return _bakeSourcePrims;
  }
  void SetBakeSourcePrims(std::vector<std::string> prims, const unsigned int version)
  {
    const std::lock_guard<std::mutex> lock(_bakeMutex);
    _bakeSourcePrims = std::move(prims);
    _bakeSourcesVersion = version;
  }
  std::atomic<bool> bakeChanged = false;
  /* Bake margin in pixels (Blender's Extend margin). */
  std::atomic<int> bakeMargin = 16;
  /* Space of baked normals (BakeNormalSpace: 0 world, 1 object, 2 tangent). */
  std::atomic<int> bakeNormalSpace = 2;

  // The cycles:material_override render setting: empty, "clay" or the path of the material that
  // every material renders as. Set outside of syncs, read by material syncs.
  void SetMaterialOverride(const std::string &materialOverride)
  {
    {
      const std::lock_guard<std::mutex> lock(_materialOverrideMutex);
      if (materialOverride == _materialOverride) {
        return;
      }
      _materialOverride = materialOverride;
    }
    {
      /* Prims without a material render with the default surface (the override material's sync
       * sets its graph there). */
      const SceneLock lock(this);
      UpdateDefaultSurface();
    }
    RequestResync(true, false);
  }
  std::string GetMaterialOverride() const
  {
    const std::lock_guard<std::mutex> lock(_materialOverrideMutex);
    return _materialOverride;
  }

  void SetDicing(const DicingSettings &dicing)
  {
    if (!(dicing == _dicing)) {
      _dicing = dicing;
      // Subdivision meshes dice again when their primvars are read again
      RequestResync(false, true);
    }
  }

  // Film exposure from the render settings, multiplied by the exposure of the camera rendered
  float GetFilmExposure() const
  {
    return _filmExposure;
  }
  void SetFilmExposure(const float exposure)
  {
    _filmExposure = exposure;
  }

  // Rendering for the Houdini viewport: it reads the depth AOV as window depth (0..1) and needs
  // integer IDs, final renders keep distance and float IDs
  /* Whether rprims may fill their geometry outside the scene lock, in parallel: while the session thread is not
   * rendering. Only the render pass starts it, after Hydra's sync: husk's frames, any first sync. Resyncs while
   * rendering (and viewports) keep the lock. */
  bool ExclusiveSync() const;
  /* Set by a host that never renders while Hydra syncs (cycles_usd_render): rprims fill their
   * geometry outside the scene lock in every sync, not only the ones before a render starts. */
  void SetExclusiveSync(const bool exclusive)
  {
    _exclusiveSync = exclusive;
  }
  bool IsViewport() const
  {
    return _viewport;
  }

  void SetViewport(const bool viewport)
  {
    _viewport = viewport;
  }

  /* Settings for hosts that differ from Karma (ADR 0076: Karma's behaviour is the default). The
   * Blender add-on sets Blender's. */

  /* cycles:viewport:window_depth: the depth AOV as window depth (0..1) outside Houdini's viewport
   * too, for viewports that composite it (USD Nodes' Live view in Blender). */
  bool GetDepthToWindow() const
  {
    return _depthToWindow;
  }
  void SetDepthToWindow(const bool depthToWindow)
  {
    _depthToWindow = depthToWindow;
  }

  /* The final-render depth AOV in metres, not in stage units (Karma): Blender's Z pass
   * (cycles_usd_render). */
  bool GetDepthInMetres() const
  {
    return _depthInMetres;
  }
  void SetDepthInMetres(const bool metres)
  {
    _depthInMetres = metres;
  }

  /* cycles:light:sun_intensity_scale: 1 = USD's units, as Karma. Blender's USD exporter and Hydra
   * export divide a sun's strength by 4 (its importer multiplies it back): Blender asks for 4. */
  float GetSunIntensityScale() const
  {
    return _sunIntensityScale;
  }
  /* Whether the scale changed. */
  bool SetSunIntensityScale(const float scale)
  {
    const bool changed = scale != _sunIntensityScale;
    _sunIntensityScale = scale;
    return changed;
  }

  /* cycles:background:camera_visible_default: whether the camera sees a dome light whose Cycles,
   * USD and Karma attributes do not say. Karma hides it; Blender shows its world. */
  bool GetDomeCameraVisibleDefault() const
  {
    return _domeCameraVisibleDefault;
  }
  /* Whether the default changed. */
  bool SetDomeCameraVisibleDefault(const bool visible)
  {
    const bool changed = visible != _domeCameraVisibleDefault;
    _domeCameraVisibleDefault = visible;
    return changed;
  }

  /* The Cycles session thread writes the AOVs (output driver) while Hydra changes the bindings
   * and deletes render buffers: the output driver holds this while using the bound buffers. */
  std::unique_lock<std::recursive_mutex> LockAovBindings() const
  {
    return std::unique_lock(_aovBindingsMutex);
  }

  PXR_NS::TfToken GetStageUpAxis() const
  {
    return _stageUpAxis;
  }

  void SetStageUpAxis(const PXR_NS::TfToken stageUpAxis)
  {
    _stageUpAxis = stageUpAxis;
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

  // Rebuild the default surface shader with the primvar AOVs, with the scene locked: the graph
  // of the material override if given, else clay for the "clay" override, else displayColor
  void UpdateDefaultSurface();
  void UpdateDefaultSurface(std::unique_ptr<CCL_NS::ShaderGraph> graph);

  // Whether a render settings prim has a motion pass render var, synced before geometry so that
  // it is sampled at the motion pass times, with the scene locked. Like the primvar AOVs, only the
  // active render settings count, or all of them when none is active.
  void SetRenderSettingsMotionPass(const PXR_NS::SdfPath &id, bool active, bool motionPass);

  // Primvar a render var reads: source type "primvar" or a "primvars:<name>" source name, unless
  // it is a Cycles pass (Karma's "st" is the UV pass). Empty if none.
  static std::string GetPrimvar(const std::string &sourceName, const PXR_NS::TfToken &sourceType);

  // Name of the Cycles pass holding an AOV, which can be shared with other AOVs
  std::string GetPassName(const PXR_NS::TfToken &aovName) const;

  // World material of a dome light ("inputs:cycles:light:world_material", empty for none), the
  // light's own shader and its rotation. UpdateScene() uses the material's shader as background
  // and light shader, like a Blender world: Cycles builds the world volume from the shaders of
  // the background light. Called with the scene locked.
  // A dome light's world: its material ("" for the light's own shader), the light's rotation
  // and the quick adjustments of its Cycles tab (exposure in stops, saturation factor)
  struct WorldMaterial {
    std::string material;
    CCL_NS::Transform rotation;
    CCL_NS::Shader *lightShader;
    float exposure = 0.0f;
    float saturation = 1.0f;
  };
  void SetWorldMaterial(CCL_NS::Light *light, const WorldMaterial &world);
  void RemoveWorldMaterial(CCL_NS::Light *light);
  // Stop using the shader of a material as world, before it is deleted
  void ReleaseWorldShader(CCL_NS::Shader *shader);
  // The world of the dome light that uses the material as world, nullptr when no light does
  const WorldMaterial *GetWorldOf(const std::string &material) const;
  // Scales the strength and the saturation of the Background nodes of a world shader graph
  static void AdjustWorld(CCL_NS::ShaderGraph *graph, float strength, float saturation);
  // Replaces the Dome Texture nodes of a world material's graph by a copy of its dome light's
  // color (texture times color and intensity, without the quick adjustments)
  void ExpandDomeTextures(CCL_NS::ShaderGraph *graph, const WorldMaterial &world);
  // Copy of a world shader's graph (a dome light's own shader or a world material) before
  // Cycles finalizes it, to add several dome lights into one world. Called with the scene locked.
  void SetWorldSource(CCL_NS::Shader *shader, CCL_NS::ShaderGraph &graph);
  void RemoveWorldSource(CCL_NS::Shader *shader);
  // The dome light's Multiple Importance setting. Only the first dome light samples the world,
  // UpdateScene() applies it.
  void SetDomeUseMis(CCL_NS::Light *light, bool useMis);

  void RemoveAovBinding(PXR_NS::HdRenderBuffer *renderBuffer);

  // Mark the render buffers as not converged, so that the render pass is executed again
  void ResetConverged();

  // With scene indices, Hydra only syncs a prim again when the scene changes it, not when the
  // render delegate marks it dirty. Changes only the delegate knows about request a sync here,
  // sent by HdCyclesDelegate::SendResync() outside of syncs, for the next sync: materials are
  // rebuilt, rprims and cameras read their transforms and primvars again.
  void RequestResync(bool materials, bool rprims);
  // The same for one material (a world material), by its path
  void RequestMaterialResync(const std::string &material);
  // The materials RequestMaterialResync() asked for since the last call, for SendResync()
  std::set<std::string> TakeMaterialResyncs();

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
  // Suns and dome lights, after a setting that changes them (sun scale, dome camera visibility)
  std::atomic<bool> resyncLights = false;
  // Set from HdCyclesDelegate::SendResync() to CommitResources() when the next sync syncs every
  // rprim again: the materials and cameras synced before them need no other rprim sync
  std::atomic<bool> rprimsResyncing = false;
  // The same for every material
  std::atomic<bool> materialsResyncing = false;
  // Prims were marked dirty for the next sync, which restarts the render
  std::atomic<bool> resyncPending = false;
  // Scene changes the render was not reset for yet, set by SceneLock with the scene locked. The
  // session thread updates the device and clears the update tags whenever it gets the lock, also
  // between a sync and the render pass that resets the render for it.
  std::atomic<bool> sceneChanged = false;
  // A light was added or removed since the last UpdateScene(), set with the scene locked
  bool lightsChanged = false;
  // Lights made of mesh objects (cylinder lights): they count as lights for the default world
  int meshLights = 0;
  // disableDepthOfField render setting: cameras render without depth of field
  std::atomic<bool> depthOfFieldDisabled = false;

  // Whether a requested re-sync has not reached the render yet
  bool IsResyncing() const
  {
    const std::lock_guard<CCL_NS::thread_mutex> lock(_resyncMaterialIdsMutex);
    return resyncMaterials || resyncRprims || resyncLights || resyncPending ||
           !_resyncMaterialIds.empty();
  }

 private:
  void CreateSession(const CCL_NS::SessionParams &params, const CCL_NS::SceneParams &scene_params);
  void UpdateLightLinking();

  std::unordered_map<std::string, std::pair<PXR_NS::TfToken, PXR_NS::TfToken>> _lightLinks;
  std::unordered_map<std::string, PXR_NS::VtArray<PXR_NS::TfToken>> _categories;
  std::unordered_map<std::string,
                     std::vector<std::pair<const CCL_NS::Object *, PXR_NS::VtArray<PXR_NS::TfToken>>>>
      _instanceCategories;
  bool _lightLinkingDirty = false;
  std::map<CCL_NS::Light *, WorldMaterial> _worldMaterials;
  void SetLightShader(CCL_NS::Light *light, CCL_NS::Shader *shader);
  CCL_NS::Shader *GetWorldShader(CCL_NS::Light *light) const;
  CCL_NS::Shader *UpdateCombinedWorld(const std::vector<CCL_NS::Light *> &domes);

  struct WorldSource {
    std::shared_ptr<CCL_NS::ShaderGraph> graph;
    uint64_t version;
  };
  std::map<const CCL_NS::Shader *, WorldSource> _worldSources;
  // World materials with Dome Texture nodes: rebuilt when their dome light's shader changes
  std::set<std::string> _domeTextureMaterials;
  uint64_t _worldSourceVersion = 0;
  std::map<const CCL_NS::Light *, bool> _domeUseMis;
  // Background shader adding the shaders of several dome lights, and what it was built from
  CCL_NS::Shader *_combinedWorld = nullptr;
  std::vector<uint64_t> _combinedWorldKey;

  size_t _numDefaultShaders = 0;
  bool _resyncEnabled = false;
  std::set<std::string> _resyncMaterialIds;
  mutable CCL_NS::thread_mutex _resyncMaterialIdsMutex;
  const bool _ownCyclesSession;
  double _stageMetersPerUnit = 0.01;
  PXR_NS::GfMatrix4d _projectionMatrix = PXR_NS::GfMatrix4d(1.0);
  mutable CCL_NS::thread_mutex _projectionMutex;
  bool _viewport = false;
  bool _exclusiveSync = false;
  bool _depthToWindow = false;
  bool _depthInMetres = false;
  float _sunIntensityScale = 1.0f;
  bool _domeCameraVisibleDefault = false;
  float _shutterOpen = 0.0f;
  float _shutterClose = 0.0f;
  bool _shutterFromSettings = false;
  bool _motionBlurDisabled = false;
  int _motionSteps = 1;
  DicingSettings _dicing;
  mutable std::mutex _bakeMutex;
  BakeSettings _bake;
  std::vector<std::string> _bakeSourcePrims;
  unsigned int _bakeSourcesVersion = 0;
  mutable std::mutex _materialOverrideMutex;
  std::string _materialOverride;
  float _filmExposure = 1.0f;
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
  PXR_NS::TfToken _stageUpAxis = PXR_NS::TfToken("Y");
  CCL_NS::set<CCL_NS::Object *> _deletedObjects;
  CCL_NS::set<CCL_NS::Geometry *> _deletedGeometry;
  CCL_NS::set<CCL_NS::Shader *> _deletedShaders;
  std::vector<CCL_NS::Shader *> _retiredShaders; /* deleted, emptied, handed out again */

  void FlushDeletedNodes();
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
