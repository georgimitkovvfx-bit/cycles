/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/filteringSceneIndex.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

TF_DECLARE_WEAK_AND_REF_PTRS(HdCyclesResyncSceneIndex);

/* Pass-through scene index at the end of the Cycles scene index chain. With scene indices, the
 * render index only syncs prims again when a dirty notice reaches it through the chain, and
 * HdChangeTracker::Mark*Dirty from the render delegate is dropped. Resync() sends such notices for
 * prims that need a sync because of a change only the render delegate knows about. */
class HdCyclesResyncSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
 public:
  static HdCyclesResyncSceneIndexRefPtr New(const HdSceneIndexBaseRefPtr &inputSceneIndex);

  HdSceneIndexPrim GetPrim(const SdfPath &primPath) const override;
  SdfPathVector GetChildPrimPaths(const SdfPath &primPath) const override;

  void Resync(const HdSceneIndexObserver::DirtiedPrimEntries &entries);

 protected:
  HdCyclesResyncSceneIndex(const HdSceneIndexBaseRefPtr &inputSceneIndex);

  void _PrimsAdded(const HdSceneIndexBase &sender,
                   const HdSceneIndexObserver::AddedPrimEntries &entries) override;
  void _PrimsRemoved(const HdSceneIndexBase &sender,
                     const HdSceneIndexObserver::RemovedPrimEntries &entries) override;
  void _PrimsDirtied(const HdSceneIndexBase &sender,
                     const HdSceneIndexObserver::DirtiedPrimEntries &entries) override;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
