/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/instancer.h>

#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesInstancer final : public PXR_NS::HdInstancer {
 public:
  /* Per-instance primvar, `width` (1 to 4) floats per instance (0 when not numeric). Object
   * settings ("cycles:object:*") also keep their value of each instance, of any type, in
   * `elements` (empty where an instance has none). */
  struct Primvar {
    PXR_NS::TfToken name;
    size_t width;
    std::vector<float> values;
    std::vector<PXR_NS::VtValue> elements;
  };

  HdCyclesInstancer(PXR_NS::HdSceneDelegate *delegate, const PXR_NS::SdfPath &instancerId);
  ~HdCyclesInstancer() override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  /* Transforms of the instances of a prototype, through all parent instancers: at each of the
   * motion `times` when the instances move over the shutter, else only at the current frame.
   * With `primvars`, also the per-instance primvars of all these instancers, in the same instance
   * order. */
  std::vector<PXR_NS::VtMatrix4dArray> ComputeInstanceTransforms(
      const PXR_NS::SdfPath &prototypeId,
      const std::vector<float> &times,
      std::vector<Primvar> *primvars = nullptr);

 private:
  /* Instancer transform and instance transform primvars at one time. */
  struct Sample {
    PXR_NS::GfMatrix4d xform = PXR_NS::GfMatrix4d(1.0);
    PXR_NS::VtVec3fArray translate;
    PXR_NS::VtVec4fArray rotate;
    PXR_NS::VtVec3fArray scale;
    PXR_NS::VtMatrix4dArray instanceTransform;
  };

  void SyncPrimvars();

  /* Samples at the motion times when anything moves over the shutter, else at the current
   * frame. Sampled when first needed after a sync, or for other motion times or frame rate. */
  const std::vector<Sample> &GetSamples(const std::vector<float> &times);

  std::vector<float> _sampleTimes;
  double _sampleFps = 0.0;
  std::vector<Sample> _samples;
  std::vector<Primvar> _primvars;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
