/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/link_prefix_scene_index.h"

#include <pxr/imaging/hd/collectionSchema.h>
#include <pxr/imaging/hd/collectionsSchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primOriginSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

/* Where the host put the stage: the prim's path without its prim origin (/usd_scene for
 * /usd_scene/world/lamp from /world/lamp), empty when the prim is at its USD path or has no
 * absolute origin (prototypes of native instances have paths relative to the prototype). */
SdfPath HostPrefix(const SdfPath &primPath, const HdContainerDataSourceHandle &primDs)
{
  const SdfPath origin = HdPrimOriginSchema::GetFromParent(primDs).GetOriginPath(
      HdPrimOriginSchemaTokens->scenePath);
  if (origin.IsEmpty() || !origin.IsAbsolutePath() || origin == primPath ||
      primPath.GetPathElementCount() <= origin.GetPathElementCount())
  {
    return SdfPath();
  }
  SdfPath prefix = primPath;
  for (size_t i = 0; i < origin.GetPathElementCount(); ++i) {
    prefix = prefix.GetParentPath();
  }
  return origin.ReplacePrefix(SdfPath::AbsoluteRootPath(), prefix) == primPath ? prefix : SdfPath();
}

/* One collection, its membership expression below the prefix. */
class CollectionDataSource final : public HdContainerDataSource {
 public:
  HD_DECLARE_DATASOURCE(CollectionDataSource);

  TfTokenVector GetNames() override
  {
    return _input->GetNames();
  }

  HdDataSourceBaseHandle Get(const TfToken &name) override
  {
    HdDataSourceBaseHandle ds = _input->Get(name);
    if (name != HdCollectionSchemaTokens->membershipExpression) {
      return ds;
    }
    const HdPathExpressionDataSourceHandle expression = HdPathExpressionDataSource::Cast(ds);
    if (!expression) {
      return ds;
    }
    return HdRetainedTypedSampledDataSource<SdfPathExpression>::New(
        expression->GetTypedValue(0.0f).ReplacePrefix(SdfPath::AbsoluteRootPath(), _prefix));
  }

 private:
  CollectionDataSource(const HdContainerDataSourceHandle &input, const SdfPath &prefix)
      : _input(input), _prefix(prefix)
  {
  }

  HdContainerDataSourceHandle _input;
  SdfPath _prefix;
};

/* The prim's collections (lightLink, shadowLink, filterLink, ...), each below the prefix. */
class CollectionsDataSource final : public HdContainerDataSource {
 public:
  HD_DECLARE_DATASOURCE(CollectionsDataSource);

  TfTokenVector GetNames() override
  {
    return _input->GetNames();
  }

  HdDataSourceBaseHandle Get(const TfToken &name) override
  {
    HdDataSourceBaseHandle ds = _input->Get(name);
    if (const HdContainerDataSourceHandle collection = HdContainerDataSource::Cast(ds)) {
      return CollectionDataSource::New(collection, _prefix);
    }
    return ds;
  }

 private:
  CollectionsDataSource(const HdContainerDataSourceHandle &input, const SdfPath &prefix)
      : _input(input), _prefix(prefix)
  {
  }

  HdContainerDataSourceHandle _input;
  SdfPath _prefix;
};

}  // namespace

HdCyclesLinkPrefixSceneIndexRefPtr HdCyclesLinkPrefixSceneIndex::New(
    const HdSceneIndexBaseRefPtr &inputSceneIndex)
{
  return TfCreateRefPtr(new HdCyclesLinkPrefixSceneIndex(inputSceneIndex));
}

HdCyclesLinkPrefixSceneIndex::HdCyclesLinkPrefixSceneIndex(
    const HdSceneIndexBaseRefPtr &inputSceneIndex)
    : HdSingleInputFilteringSceneIndexBase(inputSceneIndex)
{
}

HdSceneIndexPrim HdCyclesLinkPrefixSceneIndex::GetPrim(const SdfPath &primPath) const
{
  HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
  if (!prim.dataSource) {
    return prim;
  }
  const HdContainerDataSourceHandle collections = HdCollectionsSchema::GetFromParent(prim.dataSource)
                                                      .GetContainer();
  if (!collections) {
    return prim;
  }
  const SdfPath prefix = HostPrefix(primPath, prim.dataSource);
  if (prefix.IsEmpty()) {
    return prim;
  }
  prim.dataSource = HdOverlayContainerDataSource::New(
      HdRetainedContainerDataSource::New(HdCollectionsSchema::GetSchemaToken(),
                                         CollectionsDataSource::New(collections, prefix)),
      prim.dataSource);
  return prim;
}

SdfPathVector HdCyclesLinkPrefixSceneIndex::GetChildPrimPaths(const SdfPath &primPath) const
{
  return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
}

void HdCyclesLinkPrefixSceneIndex::_PrimsAdded(const HdSceneIndexBase & /*sender*/,
                                               const HdSceneIndexObserver::AddedPrimEntries &entries)
{
  _SendPrimsAdded(entries);
}

void HdCyclesLinkPrefixSceneIndex::_PrimsRemoved(
    const HdSceneIndexBase & /*sender*/, const HdSceneIndexObserver::RemovedPrimEntries &entries)
{
  _SendPrimsRemoved(entries);
}

void HdCyclesLinkPrefixSceneIndex::_PrimsDirtied(
    const HdSceneIndexBase & /*sender*/, const HdSceneIndexObserver::DirtiedPrimEntries &entries)
{
  _SendPrimsDirtied(entries);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
