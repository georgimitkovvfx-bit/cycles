/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/filteringSceneIndex.h>

#include <atomic>
#include <memory>

HDCYCLES_NAMESPACE_OPEN_SCOPE

TF_DECLARE_WEAK_AND_REF_PTRS(HdCyclesResyncSceneIndex);

/* Pass-through scene index at the end of the Cycles scene index chain. With scene indices, the
 * render index only syncs prims again when a dirty notice reaches it through the chain, and
 * HdChangeTracker::Mark*Dirty from the render delegate is dropped. Resync() sends such notices for
 * prims that need a sync because of a change only the render delegate knows about.
 *
 * New() also puts HdsiVelocityMotionResolvingSceneIndex before it: velocities, accelerations and
 * angular velocities become time samples of points and instance transforms (UsdGeom rules), so
 * prims only sample those at the motion times. That scene index takes the frame rate from the
 * scene globals, where husk has 24 whatever the stage's frame rate, so the render delegate sets
 * the one it renders with here (houdini:fps). Before that, HdsiRenderSettingsFilteringSceneIndex
 * marks the render settings prim of the scene globals active (HdRenderSettings::IsActive), which
 * nothing else does for Cycles. */
class HdCyclesResyncSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
 public:
  static HdCyclesResyncSceneIndexRefPtr New(const HdSceneIndexBaseRefPtr &inputSceneIndex);

  HdSceneIndexPrim GetPrim(const SdfPath &primPath) const override;
  SdfPathVector GetChildPrimPaths(const SdfPath &primPath) const override;

  void Resync(const HdSceneIndexObserver::DirtiedPrimEntries &entries);

  /* Frame rate of velocities, 0 to use the one of the scene globals. */
  void SetTimeCodesPerSecond(double timeCodesPerSecond);

 protected:
  HdCyclesResyncSceneIndex(const HdSceneIndexBaseRefPtr &inputSceneIndex,
                           std::shared_ptr<std::atomic<double>> timeCodesPerSecond);

  void _PrimsAdded(const HdSceneIndexBase &sender,
                   const HdSceneIndexObserver::AddedPrimEntries &entries) override;
  void _PrimsRemoved(const HdSceneIndexBase &sender,
                     const HdSceneIndexObserver::RemovedPrimEntries &entries) override;
  void _PrimsDirtied(const HdSceneIndexBase &sender,
                     const HdSceneIndexObserver::DirtiedPrimEntries &entries) override;

 private:
  std::shared_ptr<std::atomic<double>> _timeCodesPerSecond;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
