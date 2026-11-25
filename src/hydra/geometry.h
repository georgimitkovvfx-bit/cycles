/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "hydra/instancer.h"
#include "hydra/mesh_light.h"

#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/tokens.h>

#include <functional>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Hydra syncs only the prims of the render tags a render pass draws: the render pass hides the
 * Cycles objects of the others (a mesh that became a guide, the viewer's purpose toggles). */
class HdCyclesRenderTagVisibility {
 public:
  virtual ~HdCyclesRenderTagVisibility() = default;
  /* With the scene locked. */
  virtual void SetRenderTagVisible(CCL_NS::Scene *scene, bool visible) = 0;
};

template<typename Base, typename CyclesBase>
class HdCyclesGeometry : public Base, public HdCyclesRenderTagVisibility {
 public:
  HdCyclesGeometry(const PXR_NS::SdfPath &rprimId);

  void SetRenderTagVisible(CCL_NS::Scene *scene, bool visible) override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits,
            const PXR_NS::TfToken &reprToken) override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

 protected:
  /* Shader of the geometry without a material. */
  virtual CCL_NS::Shader *GetDefaultShader(CCL_NS::Scene *scene) const;

  void _InitRepr(const PXR_NS::TfToken &reprToken, PXR_NS::HdDirtyBits *dirtyBits) override;

  PXR_NS::HdDirtyBits _PropagateDirtyBits(PXR_NS::HdDirtyBits bits) const override;

  virtual void Populate(PXR_NS::HdSceneDelegate *sceneDelegate,
                        PXR_NS::HdDirtyBits dirtyBits,
                        bool &rebuild) = 0;

  /* Object color of every instance, from a constant displayColor value. */
  void SetInstanceColor(const PXR_NS::VtValue &value);

  /* The first numPoints of the points primvar, at the motion steps when they move over the
   * shutter. `points` is its value at the current frame. `remap`, if set, converts the points
   * of every step (e.g. curve control points to Cycles curve keys) before that. */
  void SetPositions(
      CCL_NS::Attribute *attr_P,
      const PXR_NS::HdPrimvarsSchema &primvars,
      const PXR_NS::VtVec3fArray &points,
      size_t numPoints,
      const std::function<PXR_NS::VtVec3fArray(const PXR_NS::VtVec3fArray &)> &remap = {},
      const PXR_NS::TfToken &pointsPrimvar = PXR_NS::HdTokens->points);

  /* Times of the motion steps of this object: none when its motion blur is off, its own number
   * of steps or of transform (`deform` false) or deformation samples when set. `ds` is the
   * animated value, for samples from the time samples of the stage. */
  std::vector<float> GetObjectMotionTimes(const PXR_NS::HdSampledDataSourceHandle &ds = nullptr,
                                          bool deform = false) const;

  /* Constant "cycles:object:<setting>" primvar, empty if not authored. */
  static PXR_NS::VtValue GetObjectSetting(const PXR_NS::HdPrimvarsSchema &primvars,
                                          const std::string &setting);

  CyclesBase *_geom = nullptr;
  std::vector<CCL_NS::Object *> _instances;
  // Ray visibility of the instances from the prim, and whether its render tag is drawn
  unsigned int _visibility = ~0u;
  // Per instance when the instancer has per-instance ray visibility settings, else empty
  std::vector<unsigned int> _instanceVisibility;
  bool _renderTagVisible = true;
  /* Set by Populate for an attribute-only update, which Sync tags under the scene lock: Populate may run outside
   * it (HdCyclesSession::ExclusiveSync). */
  bool _attributesChanged = false;
  const HdCyclesSession *_session = nullptr;

 private:
  void Initialize(PXR_NS::HdRenderParam *renderParam);

  void InitializeInstance(const int index);
  void SetPrimIdAttribute(CCL_NS::Object *instance) const;
  void UpdateObjectSettings(const PXR_NS::HdContainerDataSourceHandle &primDs);
  void SetInstancePrimvars(CCL_NS::Scene *scene,
                           const PXR_NS::HdContainerDataSourceHandle &primDs,
                           const std::vector<HdCyclesInstancer::Primvar> &primvars);

  // Per-instance "cycles:object:<setting>" primvars of the instancer (Primvar::elements)
  std::vector<HdCyclesInstancer::Primvar> _instanceSettings;

  // UsdLux LightAPI / MeshLightAPI on the prim (Houdini's Geometry Light LOP)
  HdCyclesGeometryLight _light;

  // Per-object motion blur settings: 0 steps or samples use the Motion Steps render setting
  struct MotionSettings {
    int steps = 0;
    int transformSamples = 0;
    int deformSamples = 0;
    // Instance Motion Samples of the instancer, for the transforms
    int instanceSamples = 0;
    bool fromStage = false;
    // Karma's Velocity Blur: -1 not authored, 0 none, 1 velocity, 2 acceleration
    int velocityBlur = -1;
    bool operator!=(const MotionSettings &other) const
    {
      return steps != other.steps || transformSamples != other.transformSamples ||
             deformSamples != other.deformSamples || instanceSamples != other.instanceSamples ||
             fromStage != other.fromStage || velocityBlur != other.velocityBlur;
    }
  };
  bool _useMotionBlur = true;
  MotionSettings _motion;

  PXR_NS::GfMatrix4d _geomTransform;
  // Transforms at the motion steps when moving over the shutter, else empty
  std::vector<PXR_NS::GfMatrix4d> _geomTransformMotion;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
