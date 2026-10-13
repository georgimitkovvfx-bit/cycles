/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/resync_scene_index.h"

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdCyclesResyncSceneIndexRefPtr HdCyclesResyncSceneIndex::New(
    const HdSceneIndexBaseRefPtr &inputSceneIndex)
{
  return TfCreateRefPtr(new HdCyclesResyncSceneIndex(inputSceneIndex));
}

HdCyclesResyncSceneIndex::HdCyclesResyncSceneIndex(const HdSceneIndexBaseRefPtr &inputSceneIndex)
    : HdSingleInputFilteringSceneIndexBase(inputSceneIndex)
{
}

HdSceneIndexPrim HdCyclesResyncSceneIndex::GetPrim(const SdfPath &primPath) const
{
  return _GetInputSceneIndex()->GetPrim(primPath);
}

SdfPathVector HdCyclesResyncSceneIndex::GetChildPrimPaths(const SdfPath &primPath) const
{
  return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
}

void HdCyclesResyncSceneIndex::Resync(const HdSceneIndexObserver::DirtiedPrimEntries &entries)
{
  _SendPrimsDirtied(entries);
}

void HdCyclesResyncSceneIndex::_PrimsAdded(const HdSceneIndexBase & /*sender*/,
                                           const HdSceneIndexObserver::AddedPrimEntries &entries)
{
  _SendPrimsAdded(entries);
}

void HdCyclesResyncSceneIndex::_PrimsRemoved(
    const HdSceneIndexBase & /*sender*/, const HdSceneIndexObserver::RemovedPrimEntries &entries)
{
  _SendPrimsRemoved(entries);
}

void HdCyclesResyncSceneIndex::_PrimsDirtied(
    const HdSceneIndexBase & /*sender*/, const HdSceneIndexObserver::DirtiedPrimEntries &entries)
{
  _SendPrimsDirtied(entries);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
