/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "hydra/geometry.h"

#include <pxr/imaging/hd/basisCurves.h>

#include <utility>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Cycles curve keys as weighted sums of USD values (control points or varying values): Cycles
 * interpolates its keys as Catmull-Rom (or linearly), so other bases and periodic or pinned
 * curves are resampled into keys. Empty when the USD values are the keys. */
struct HdCyclesCurveKeyMap {
  std::vector<int> start; /* First term of each key, plus the end. */
  std::vector<std::pair<int, float>> terms;
  size_t sourceSize = 0; /* Number of USD values. */

  bool empty() const
  {
    return start.empty();
  }
  size_t size() const
  {
    return start.empty() ? 0 : start.size() - 1;
  }
  /* Keys from a float/vector array with sourceSize values, the value unchanged otherwise. */
  PXR_NS::VtValue Apply(const PXR_NS::VtValue &value) const;
};

class HdCyclesCurves final : public HdCyclesGeometry<PXR_NS::HdBasisCurves, CCL_NS::Hair> {
 public:
  HdCyclesCurves(const PXR_NS::SdfPath &rprimId);
  ~HdCyclesCurves() override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

 private:
  PXR_NS::HdDirtyBits _PropagateDirtyBits(PXR_NS::HdDirtyBits bits) const override;

  void Populate(PXR_NS::HdSceneDelegate *sceneDelegate,
                PXR_NS::HdDirtyBits dirtyBits,
                bool &rebuild) override;

  void PopulatePoints(PXR_NS::HdSceneDelegate *sceneDelegate);
  void PopulateWidths(PXR_NS::HdSceneDelegate *sceneDelegate);

  void PopulatePrimvars(PXR_NS::HdSceneDelegate *sceneDelegate);
  void PopulateCurveAttributes();

  void PopulateTopology(PXR_NS::HdSceneDelegate *sceneDelegate);

  HdCyclesCurveKeyMap _vertexMap;
  HdCyclesCurveKeyMap _varyingMap;
  size_t _numControlPoints = 0;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
