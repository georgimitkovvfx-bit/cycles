/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/hd/instancer.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesInstancer final : public PXR_NS::HdInstancer {
 public:
  HdCyclesInstancer(PXR_NS::HdSceneDelegate *delegate, const PXR_NS::SdfPath &instancerId);
  ~HdCyclesInstancer() override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  PXR_NS::VtMatrix4dArray ComputeInstanceTransforms(const PXR_NS::SdfPath &prototypeId);
  /* At a shutter offset (frames): motion blur. Offset 0 uses the primvars read at sync. */
  PXR_NS::VtMatrix4dArray ComputeInstanceTransforms(const PXR_NS::SdfPath &prototypeId, float time);
  /* Whether this instancer or a parent instancer moves its instances within [open, close]. */
  bool IsMoving(float open, float close);

 private:
  void SyncPrimvars();
  struct Arrays {
    PXR_NS::VtVec3fArray translate;
    PXR_NS::VtVec4fArray rotate;
    PXR_NS::VtVec3fArray scale;
    PXR_NS::VtMatrix4dArray instanceTransform;
  };
  /* The instance primvars at a shutter offset. */
  void ReadPrimvars(float time, Arrays &out);

  PXR_NS::VtVec3fArray _translate;
  PXR_NS::VtVec4fArray _rotate;
  PXR_NS::VtVec3fArray _scale;
  PXR_NS::VtMatrix4dArray _instanceTransform;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
