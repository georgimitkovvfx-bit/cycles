/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/render_pass_scene_index.h"

#include <pxr/imaging/hd/collectionSchema.h>
#include <pxr/imaging/hd/collectionsSchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneGlobalsSchema.h>
#include <pxr/imaging/hd/sceneIndexPrimView.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/visibilitySchema.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

const TfToken renderVisibilityToken("renderVisibility");
const TfToken cameraVisibilityToken("cameraVisibility");
const TfToken matteToken("matte");
const TfToken visibleCameraPrimvar("cycles:object:visible_camera");
const TfToken holdoutPrimvar("cycles:object:use_holdout");

bool IsGeometry(const TfToken &primType)
{
  return HdPrimTypeIsGprim(primType) || primType == HdPrimTypeTokens->particleField;
}

HdDataSourceBaseHandle ConstantBoolPrimvar(const bool value)
{
  return HdPrimvarSchema::Builder()
      .SetPrimvarValue(HdRetainedTypedSampledDataSource<bool>::New(value))
      .SetInterpolation(
          HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->constant))
      .Build();
}

}  // namespace

HdCyclesRenderPassSceneIndexRefPtr HdCyclesRenderPassSceneIndex::New(
    const HdSceneIndexBaseRefPtr &inputSceneIndex)
{
  return TfCreateRefPtr(new HdCyclesRenderPassSceneIndex(inputSceneIndex));
}

HdCyclesRenderPassSceneIndex::HdCyclesRenderPassSceneIndex(
    const HdSceneIndexBaseRefPtr &inputSceneIndex)
    : HdSingleInputFilteringSceneIndexBase(inputSceneIndex)
{
  UpdatePass();
}

HdSceneIndexPrim HdCyclesRenderPassSceneIndex::GetPrim(const SdfPath &primPath) const
{
  HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
  if (_passPath.IsEmpty() || !prim.dataSource) {
    return prim;
  }
  const bool geometry = IsGeometry(prim.primType);
  if (!geometry && !HdPrimTypeIsLight(prim.primType)) {
    return prim;
  }
  const auto matches = [&](const Collection &collection) {
    return collection.evaluator->Match(primPath).GetValue();
  };

  TfToken names[2];
  HdDataSourceBaseHandle values[2];
  size_t count = 0;
  if (_renderVisibility.evaluator && !matches(_renderVisibility)) {
    names[count] = HdVisibilitySchema::GetSchemaToken();
    values[count++] = HdVisibilitySchema::Builder()
                          .SetVisibility(HdRetainedTypedSampledDataSource<bool>::New(false))
                          .Build();
  }
  if (geometry) {
    TfToken primvarNames[2];
    HdDataSourceBaseHandle primvarValues[2];
    size_t primvarCount = 0;
    if (_cameraVisibility.evaluator && !matches(_cameraVisibility)) {
      primvarNames[primvarCount] = visibleCameraPrimvar;
      primvarValues[primvarCount++] = ConstantBoolPrimvar(false);
    }
    if (_matte.evaluator && matches(_matte)) {
      primvarNames[primvarCount] = holdoutPrimvar;
      primvarValues[primvarCount++] = ConstantBoolPrimvar(true);
    }
    if (primvarCount) {
      names[count] = HdPrimvarsSchema::GetSchemaToken();
      values[count++] = HdRetainedContainerDataSource::New(
          primvarCount, primvarNames, primvarValues);
    }
  }
  if (count) {
    /* The pass wins over the prim's own settings. */
    prim.dataSource = HdOverlayContainerDataSource::New(
        HdRetainedContainerDataSource::New(count, names, values), prim.dataSource);
  }
  return prim;
}

SdfPathVector HdCyclesRenderPassSceneIndex::GetChildPrimPaths(const SdfPath &primPath) const
{
  return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
}

void HdCyclesRenderPassSceneIndex::UpdatePass()
{
  const HdSceneIndexBaseRefPtr &input = _GetInputSceneIndex();
  SdfPath passPath;
  if (HdPathDataSourceHandle ds =
          HdSceneGlobalsSchema::GetFromSceneIndex(input).GetActiveRenderPassPrim())
  {
    passPath = ds->GetTypedValue(0.0f);
  }
  const HdCollectionsSchema collections = HdCollectionsSchema::GetFromParent(
      passPath.IsEmpty() ? nullptr : input->GetPrim(passPath).dataSource);
  const auto expression = [&](const TfToken &name) -> std::optional<SdfPathExpression> {
    if (HdPathExpressionDataSourceHandle ds =
            collections.GetCollection(name).GetMembershipExpression())
    {
      return ds->GetTypedValue(0.0f);
    }
    return std::nullopt;
  };
  const std::optional<SdfPathExpression> renderVisibility = expression(renderVisibilityToken);
  const std::optional<SdfPathExpression> cameraVisibility = expression(cameraVisibilityToken);
  const std::optional<SdfPathExpression> matte = expression(matteToken);

  if (passPath == _passPath && renderVisibility == _renderVisibility.expression &&
      cameraVisibility == _cameraVisibility.expression && matte == _matte.expression)
  {
    return;
  }
  const bool hadPass = !_passPath.IsEmpty();
  _passPath = passPath;

  /* Visibility collections hide what they leave out, an empty one everything; a missing or
   * everything collection needs no evaluation, nor a missing or empty matte collection. */
  const auto update = [&](Collection &collection,
                          const std::optional<SdfPathExpression> &expr,
                          const bool needed) {
    collection.expression = expr;
    collection.evaluator.reset();
    if (needed) {
      collection.evaluator.emplace(input, *expr);
    }
  };
  update(_renderVisibility,
         renderVisibility,
         renderVisibility && *renderVisibility != SdfPathExpression::Everything());
  update(_cameraVisibility,
         cameraVisibility,
         cameraVisibility && *cameraVisibility != SdfPathExpression::Everything());
  update(_matte, matte, matte && !matte->IsEmpty());

  if (!hadPass && _passPath.IsEmpty()) {
    return;
  }
  /* shortcut: dirties all geometry and lights on a pass change, not only those whose membership
   * changed; fine for husk, where the pass is set once. */
  static const HdDataSourceLocatorSet locators = {
      HdVisibilitySchema::GetDefaultLocator(),
      HdPrimvarsSchema::GetDefaultLocator().Append(visibleCameraPrimvar),
      HdPrimvarsSchema::GetDefaultLocator().Append(holdoutPrimvar)};
  HdSceneIndexObserver::DirtiedPrimEntries dirtied;
  for (const SdfPath &path : HdSceneIndexPrimView(input)) {
    const TfToken primType = input->GetPrim(path).primType;
    if (IsGeometry(primType) || HdPrimTypeIsLight(primType)) {
      dirtied.emplace_back(path, locators);
    }
  }
  if (!dirtied.empty()) {
    _SendPrimsDirtied(dirtied);
  }
}

void HdCyclesRenderPassSceneIndex::_PrimsAdded(
    const HdSceneIndexBase & /*sender*/, const HdSceneIndexObserver::AddedPrimEntries &entries)
{
  _SendPrimsAdded(entries);
  for (const HdSceneIndexObserver::AddedPrimEntry &entry : entries) {
    if (entry.primPath.IsAbsoluteRootPath() || entry.primPath == _passPath) {
      UpdatePass();
      break;
    }
  }
}

void HdCyclesRenderPassSceneIndex::_PrimsRemoved(
    const HdSceneIndexBase & /*sender*/, const HdSceneIndexObserver::RemovedPrimEntries &entries)
{
  _SendPrimsRemoved(entries);
  for (const HdSceneIndexObserver::RemovedPrimEntry &entry : entries) {
    if (!_passPath.IsEmpty() && _passPath.HasPrefix(entry.primPath)) {
      UpdatePass();
      break;
    }
  }
}

void HdCyclesRenderPassSceneIndex::_PrimsDirtied(
    const HdSceneIndexBase & /*sender*/, const HdSceneIndexObserver::DirtiedPrimEntries &entries)
{
  _SendPrimsDirtied(entries);
  for (const HdSceneIndexObserver::DirtiedPrimEntry &entry : entries) {
    if ((entry.primPath.IsAbsoluteRootPath() &&
         entry.dirtyLocators.Intersects(HdSceneGlobalsSchema::GetActiveRenderPassPrimLocator())) ||
        (entry.primPath == _passPath &&
         entry.dirtyLocators.Intersects(HdCollectionsSchema::GetDefaultLocator())))
    {
      UpdatePass();
      break;
    }
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
