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
#include <pxr/imaging/hd/instancer.h>

#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesInstancer final : public PXR_NS::HdInstancer {
 public:
  /* Per-instance primvar, `width` (1 to 4) floats per instance. */
  struct Primvar {
    PXR_NS::TfToken name;
    size_t width;
    std::vector<float> values;
  };

  HdCyclesInstancer(PXR_NS::HdSceneDelegate *delegate, const PXR_NS::SdfPath &instancerId);
  ~HdCyclesInstancer() override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  /* Transforms of the instances of a prototype, through all parent instancers. With `primvars`,
   * also the per-instance primvars of all these instancers, in the same instance order. */
  PXR_NS::VtMatrix4dArray ComputeInstanceTransforms(const PXR_NS::SdfPath &prototypeId,
                                                    std::vector<Primvar> *primvars = nullptr);

 private:
  void SyncPrimvars();

  PXR_NS::VtVec3fArray _translate;
  PXR_NS::VtVec4fArray _rotate;
  PXR_NS::VtVec3fArray _scale;
  PXR_NS::VtMatrix4dArray _instanceTransform;
  std::vector<Primvar> _primvars;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
