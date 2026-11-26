/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/filteringSceneIndex.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

TF_DECLARE_WEAK_AND_REF_PTRS(HdCyclesLinkPrefixSceneIndex);

/* Collections of a stage that a host put below a prefix in Hydra (Blender's Stage View: the stage
 * at /usd_scene, through HdPrefixingSceneIndex). Their membership expressions name USD paths
 * (/world/left), which match nothing below the prefix, so HdsiLightLinkingSceneIndex linked a
 * light to no geometry at all. A prim whose path is its prim origin (HdPrimOriginSchema) below a
 * prefix gets its collections' expressions with that prefix. Other prims pass unchanged: hosts
 * without a prefix (Houdini, usdview, husk, cycles_usd_render) see no difference. Before
 * HdsiLightLinkingSceneIndex in the chain. */
class HdCyclesLinkPrefixSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
 public:
  static HdCyclesLinkPrefixSceneIndexRefPtr New(const HdSceneIndexBaseRefPtr &inputSceneIndex);

  HdSceneIndexPrim GetPrim(const SdfPath &primPath) const override;
  SdfPathVector GetChildPrimPaths(const SdfPath &primPath) const override;

 protected:
  HdCyclesLinkPrefixSceneIndex(const HdSceneIndexBaseRefPtr &inputSceneIndex);

  void _PrimsAdded(const HdSceneIndexBase &sender,
                   const HdSceneIndexObserver::AddedPrimEntries &entries) override;
  void _PrimsRemoved(const HdSceneIndexBase &sender,
                     const HdSceneIndexObserver::RemovedPrimEntries &entries) override;
  void _PrimsDirtied(const HdSceneIndexBase &sender,
                     const HdSceneIndexObserver::DirtiedPrimEntries &entries) override;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
