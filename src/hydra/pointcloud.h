/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "hydra/geometry.h"

#include <pxr/imaging/hd/points.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesPoints final : public HdCyclesGeometry<PXR_NS::HdPoints, CCL_NS::PointCloud> {
 public:
  /* gaussianSplats: a Hydra particleField (USD ParticleField3DGaussianSplat), rendered as
   * Cycles Gaussian splats. */
  HdCyclesPoints(const PXR_NS::SdfPath &rprimId, bool gaussianSplats = false);
  ~HdCyclesPoints() override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

 private:
  PXR_NS::HdDirtyBits _PropagateDirtyBits(PXR_NS::HdDirtyBits bits) const override;

  void Populate(PXR_NS::HdSceneDelegate *sceneDelegate,
                PXR_NS::HdDirtyBits dirtyBits,
                bool &rebuild) override;

  void PopulatePoints(PXR_NS::HdSceneDelegate *sceneDelegate);
  void PopulateWidths(PXR_NS::HdSceneDelegate *sceneDelegate);

  void PopulatePrimvars(PXR_NS::HdSceneDelegate *sceneDelegate);
  void PopulatePointRandom(PXR_NS::HdSceneDelegate *sceneDelegate);
  void PopulateGaussianSplats(PXR_NS::HdSceneDelegate *sceneDelegate);

  /* Splats without a material show their radiance (Cycles' default splat shader). */
  CCL_NS::Shader *GetDefaultShader(CCL_NS::Scene *scene) const override;

  bool _gaussianSplats = false;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
