/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "hydra/resync_scene_index.h"

#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hgi/hgi.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

// clang-format off
/* stageMetersPerUnit and stageUpAxis are not part of the USD standard and are set by 
 * Cycles standalone app when loading USD files. */
#define HD_CYCLES_RENDER_SETTINGS_TOKENS \
    (stageMetersPerUnit) \
    (stageUpAxis) \
    ((device, "cycles:device")) \
    ((useCpuToo, "cycles:use_cpu_too")) \
    ((gpuDevices, "cycles:gpu_devices")) \
    ((threads, "cycles:threads")) \
    ((shadingSystem, "cycles:shading_system")) \
    ((useResolutionDivider, "cycles:use_resolution_divider")) \
    ((curveShape, "cycles:curve_shape")) \
    ((useTextureCache, "cycles:use_texture_cache")) \
    ((autoTextureCache, "cycles:auto_texture_cache")) \
    ((textureCachePath, "cycles:texture_cache_path")) \
    ((textureLimit, "cycles:texture_limit")) \
    ((tileSize, "cycles:tile_size")) \
    ((pixelSize, "cycles:pixel_size")) \
    ((useSpatialSplits, "cycles:use_spatial_splits")) \
    ((useCompactBvh, "cycles:use_compact_bvh")) \
    ((bvhType, "cycles:bvh_type")) \
    ((curveSubdivisions, "cycles:curve_subdivisions")) \
    ((timeLimit, "cycles:time_limit")) \
    ((samples, "cycles:samples")) \
    ((sampleOffset, "cycles:sample_offset")) \
    ((transparentBackground, "cycles:background:transparent")) \
    ((fastGi, "cycles:fast_gi")) \
    ((fastGiMethod, "cycles:fast_gi_method")) \
    ((aoBounces, "cycles:ao_bounces")) \
    ((aoFactor, "cycles:ao_factor")) \
    ((houdiniViewport, "houdini:viewport")) \
    ((windowDepth, "cycles:viewport:window_depth")) \
    ((sunIntensityScale, "cycles:light:sun_intensity_scale")) \
    ((domeCameraVisibleDefault, "cycles:background:camera_visible_default")) \
    (resolution) \
    (pixelAspectRatio) \
    (aspectRatioConformPolicy) \
    (renderMode) \
    ((houdiniFps, "houdini:fps")) \
    (disableMotionBlur) \
    (instantaneousShutter) \
    (disableDepthOfField) \
    ((motionSteps, "cycles:motion_steps"))     ((dicingRate, "cycles:dicing_rate"))     ((previewDicingRate, "cycles:preview_dicing_rate"))     ((maxSubdivisions, "cycles:max_subdivisions"))     ((offscreenDicingScale, "cycles:offscreen_dicing_scale"))     ((dicingCamera, "cycles:dicing_camera"))     ((bakeObject, "cycles:bake_object"))     ((bakeInstance, "cycles:bake_instance"))     ((bakeUVMap, "cycles:bake_uv_map"))     ((bakeSelectedToActive, "cycles:bake_use_selected_to_active"))     ((bakeSource, "cycles:bake_source"))     ((bakeCageObject, "cycles:bake_cage_object"))     ((bakeCageExtrusion, "cycles:bake_cage_extrusion"))     ((bakeMaxRayDistance, "cycles:bake_max_ray_distance"))     ((bakeMargin, "cycles:bake_margin"))     ((bakeNormalSpace, "cycles:bake_normal_space")) \
    ((abortNoGpu, "cycles:abort_no_gpu")) \
    ((abortMissingTexture, "cycles:abort_missing_texture")) \
    ((materialOverride, "cycles:material_override")) \
    ((shutterOpen, "shutter:open")) \
    ((shutterClose, "shutter:close"))
// clang-format on

TF_DECLARE_PUBLIC_TOKENS(HdCyclesRenderSettingsTokens, HD_CYCLES_RENDER_SETTINGS_TOKENS);

class HdCyclesDelegate final : public PXR_NS::HdRenderDelegate {
 public:
  HdCyclesDelegate(const PXR_NS::HdRenderSettingsMap &settingsMap,
                   CCL_NS::Session *session_ = nullptr,
                   const bool keep_nodes = false);
  ~HdCyclesDelegate() override;

  void SetDrivers(const PXR_NS::HdDriverVector &drivers) override;

  bool IsDisplaySupported() const;

  PXR_NS::Hgi *GetHgi() const
  {
    return _hgi;
  }

  const PXR_NS::TfTokenVector &GetSupportedRprimTypes() const override;
  const PXR_NS::TfTokenVector &GetSupportedSprimTypes() const override;
  const PXR_NS::TfTokenVector &GetSupportedBprimTypes() const override;

  PXR_NS::HdRenderParam *GetRenderParam() const override;

  PXR_NS::HdResourceRegistrySharedPtr GetResourceRegistry() const override;

  PXR_NS::HdRenderSettingDescriptorList GetRenderSettingDescriptors() const override;

  bool IsPauseSupported() const override;

  bool Pause() override;
  bool Resume() override;

  PXR_NS::HdRenderPassSharedPtr CreateRenderPass(
      PXR_NS::HdRenderIndex *index, const PXR_NS::HdRprimCollection &collection) override;

  PXR_NS::HdInstancer *CreateInstancer(PXR_NS::HdSceneDelegate *delegate,
                                       const PXR_NS::SdfPath &id) override;
  void DestroyInstancer(PXR_NS::HdInstancer *instancer) override;

  PXR_NS::HdRprim *CreateRprim(const PXR_NS::TfToken &typeId,
                               const PXR_NS::SdfPath &rprimId) override;
  void DestroyRprim(PXR_NS::HdRprim *rPrim) override;

  PXR_NS::HdSprim *CreateSprim(const PXR_NS::TfToken &typeId,
                               const PXR_NS::SdfPath &sprimId) override;
  PXR_NS::HdSprim *CreateFallbackSprim(const PXR_NS::TfToken &typeId) override;
  void DestroySprim(PXR_NS::HdSprim *sPrim) override;

  PXR_NS::HdBprim *CreateBprim(const PXR_NS::TfToken &typeId,
                               const PXR_NS::SdfPath &bprimId) override;
  PXR_NS::HdBprim *CreateFallbackBprim(const PXR_NS::TfToken &typeId) override;
  void DestroyBprim(PXR_NS::HdBprim *bPrim) override;

  void CommitResources(PXR_NS::HdChangeTracker *tracker) override;

  void Update() override;

  /* Mark the prims that HdCyclesSession::RequestResync asked for dirty, so that the next sync
   * syncs them. Scene indices must not send notices during a sync: called after setting changes,
   * after a sync (CommitResources) and after the render pass. */
  void SendResync();

  PXR_NS::TfToken GetMaterialBindingPurpose() const override;

  PXR_NS::TfTokenVector GetMaterialRenderContexts() const override;

  PXR_NS::VtDictionary GetRenderStats() const override;

  PXR_NS::HdAovDescriptor GetDefaultAovDescriptor(const PXR_NS::TfToken &name) const override;

  void SetRenderSetting(const PXR_NS::TfToken &key, const PXR_NS::VtValue &value) override;

  PXR_NS::VtValue GetRenderSetting(const PXR_NS::TfToken &key) const override;

  PXR_NS::HdCommandDescriptors GetCommandDescriptors() const override;
  bool InvokeCommand(const PXR_NS::TfToken &command, const PXR_NS::HdCommandArgs &args) override;

 private:
  PXR_NS::Hgi *_hgi = nullptr;
  std::unique_ptr<HdCyclesSession> _renderParam;
  /* The resync scene index of the render index, once the render pass is created. */
  HdCyclesResyncSceneIndexPtr GetResyncSceneIndex();

  PXR_NS::HdRenderIndex *_renderIndex = nullptr;
  HdCyclesResyncSceneIndexPtr _resyncSceneIndex;

  /* Blender's Fast GI settings, which set the integrator's AO sockets together. */
  bool _fastGi = false;
  std::string _fastGiMethod = "replace";
  int _aoBounces = 1;
  float _aoFactor = 1.0f;

  /* Set the integrator's denoising sockets from the settings, with what DLSS needs when it
   * denoises, or another denoiser when it cannot. */
  void UpdateDenoiser();

  /* Karma's defaults where Cycles' core ones differ, on a new scene. */
  void ApplyKarmaDefaults();
  bool _dlssWarned = false;

  /* Set the Cycles settings that have a Karma fallback (GetKarmaFallbacks) using the Karma or
   * Cycles setting `key`, from Karma's settings while the Cycles setting is not authored. */
  void ApplyKarmaFallbacks(const PXR_NS::TfToken &key);
  /* Cycles settings in the settings map that were set from Karma's. */
  PXR_NS::TfToken::Set _karmaFallbacks;

  /* Farm aborts of GetRenderStats: the texture files last checked, the first of them missing,
   * and whether the abort was reported. */
  mutable std::mutex _abortMutex;
  mutable std::vector<std::string> _checkedTextures;
  mutable std::string _missingTexture;
  mutable std::atomic<bool> _abortReported = false;

  /* houdini:cop_texture_changed arrived: the next CommitResources converts the op: images
   * again. */
  std::atomic<bool> _copTexturesChanged = false;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
