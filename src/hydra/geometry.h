/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "hydra/instancer.h"

#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/rprim.h>

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
  void _InitRepr(const PXR_NS::TfToken &reprToken, PXR_NS::HdDirtyBits *dirtyBits) override;

  PXR_NS::HdDirtyBits _PropagateDirtyBits(PXR_NS::HdDirtyBits bits) const override;

  virtual void Populate(PXR_NS::HdSceneDelegate *sceneDelegate,
                        PXR_NS::HdDirtyBits dirtyBits,
                        bool &rebuild) = 0;

  /* Object color of every instance, from a constant displayColor value. */
  void SetInstanceColor(const PXR_NS::VtValue &value);

  /* The first numPoints of the points primvar, at the motion steps when they move over the
   * shutter. `points` is its value at the current frame. */
  void SetPositions(CCL_NS::Attribute *attr_P,
                    const PXR_NS::HdPrimvarsSchema &primvars,
                    const PXR_NS::VtVec3fArray &points,
                    size_t numPoints);

  CyclesBase *_geom = nullptr;
  std::vector<CCL_NS::Object *> _instances;
  // Ray visibility of the instances from the prim, and whether its render tag is drawn
  unsigned int _visibility = ~0u;
  bool _renderTagVisible = true;
  const HdCyclesSession *_session = nullptr;

 private:
  void Initialize(PXR_NS::HdRenderParam *renderParam);

  void InitializeInstance(const int index);
  void UpdateObjectSettings(const PXR_NS::HdContainerDataSourceHandle &primDs);
  void SetInstancePrimvars(CCL_NS::Scene *scene,
                           const PXR_NS::HdContainerDataSourceHandle &primDs,
                           const std::vector<HdCyclesInstancer::Primvar> &primvars);

  PXR_NS::GfMatrix4d _geomTransform;
  // Transforms at the motion steps when moving over the shutter, else empty
  std::vector<PXR_NS::GfMatrix4d> _geomTransformMotion;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
