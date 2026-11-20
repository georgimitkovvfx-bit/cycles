/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/collectionExpressionEvaluator.h>
#include <pxr/imaging/hd/filteringSceneIndex.h>

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

HDCYCLES_NAMESPACE_OPEN_SCOPE

TF_DECLARE_WEAK_AND_REF_PTRS(HdCyclesRenderPassSceneIndex);

/* Collections of the active UsdRenderPass (husk --pass, the scene globals' activeRenderPassPrim).
 * Geometry and lights not in renderVisibility become invisible, geometry not in cameraVisibility
 * gets cycles:object:visible_camera off, and geometry in matte cycles:object:use_holdout on.
 * The prune collection is HdsiRenderPassPruneSceneIndex's. */
class HdCyclesRenderPassSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
 public:
  static HdCyclesRenderPassSceneIndexRefPtr New(const HdSceneIndexBaseRefPtr &inputSceneIndex);

  HdSceneIndexPrim GetPrim(const SdfPath &primPath) const override;
  SdfPathVector GetChildPrimPaths(const SdfPath &primPath) const override;

 protected:
  HdCyclesRenderPassSceneIndex(const HdSceneIndexBaseRefPtr &inputSceneIndex);

  void _PrimsAdded(const HdSceneIndexBase &sender,
                   const HdSceneIndexObserver::AddedPrimEntries &entries) override;
  void _PrimsRemoved(const HdSceneIndexBase &sender,
                     const HdSceneIndexObserver::RemovedPrimEntries &entries) override;
  void _PrimsDirtied(const HdSceneIndexBase &sender,
                     const HdSceneIndexObserver::DirtiedPrimEntries &entries) override;

 private:
  struct Collection {
    std::optional<SdfPathExpression> expression; /* Unset: the pass has no such collection. */
    std::optional<HdCollectionExpressionEvaluator> evaluator;
  };

  /* Reads the active pass and its collections again, and dirties all geometry and lights when
   * they changed. */
  void UpdatePass();

  /* Collection membership bits of a prim, evaluated once per path until the scene or the pass
   * changes (GetPrim is called many times per prim and sync, from several threads). */
  uint8_t GetMembership(const SdfPath &primPath) const;
  void ClearMembership();
  mutable std::shared_mutex _membershipMutex;
  mutable std::unordered_map<SdfPath, uint8_t, SdfPath::Hash> _membership;

  SdfPath _passPath;
  Collection _renderVisibility;
  Collection _cameraVisibility;
  Collection _matte;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
