/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Ported from hdCycles-h22 (https://projects.blender.org/devanshutak25/hdCycles-h22, branch hdCycles-H22, commit
 * 3e0bcd4, src/hydra/cylinder_light.h, Apache-2.0) as hdcycles-blender patch 0029. */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/light.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* USD cylinder light as a Cycles mesh light: Cycles has no cylinder light, so its side (USD's cylinder lights emit no
 * light from their caps) is an emissive tube along the X axis. A point light stood in before: 5 times Karma's
 * brightness without normalize, 0.79 times with it (hdcycles-blender light_units_test.sh). */
class HdCyclesCylinderLight final : public PXR_NS::HdLight {
 public:
  HdCyclesCylinderLight(const PXR_NS::SdfPath &sprimId);
  ~HdCyclesCylinderLight() override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

 private:
  CCL_NS::Object *_object = nullptr;
  CCL_NS::Mesh *_mesh = nullptr;
  CCL_NS::Shader *_shader = nullptr;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
