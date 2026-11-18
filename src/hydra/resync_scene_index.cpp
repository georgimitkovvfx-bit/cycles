/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/resync_scene_index.h"

#include <algorithm>
#include <optional>

#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/cameraSchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneGlobalsSchema.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/renderSettingsFilteringSceneIndex.h>
#include <pxr/imaging/hdsi/velocityMotionResolvingSceneIndex.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

/* Sets the frame rate in the scene globals of the root prim to the render delegate's. */
class SceneGlobalsSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
 public:
  static TfRefPtr<SceneGlobalsSceneIndex> New(
      const HdSceneIndexBaseRefPtr &inputSceneIndex,
      std::shared_ptr<const std::atomic<double>> timeCodesPerSecond)
  {
    return TfCreateRefPtr(
        new SceneGlobalsSceneIndex(inputSceneIndex, std::move(timeCodesPerSecond)));
  }

  HdSceneIndexPrim GetPrim(const SdfPath &primPath) const override
  {
    HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
    const double timeCodesPerSecond = *_timeCodesPerSecond;
    if (primPath.IsAbsoluteRootPath() && timeCodesPerSecond > 0.0) {
      prim.dataSource = HdOverlayContainerDataSource::OverlayedContainerDataSources(
          HdRetainedContainerDataSource::New(
              HdSceneGlobalsSchema::GetSchemaToken(),
              HdSceneGlobalsSchema::Builder()
                  .SetTimeCodesPerSecond(
                      HdRetainedTypedSampledDataSource<double>::New(timeCodesPerSecond))
                  .Build()),
          prim.dataSource);
    }
    return prim;
  }

  SdfPathVector GetChildPrimPaths(const SdfPath &primPath) const override
  {
    return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
  }

 protected:
  SceneGlobalsSceneIndex(const HdSceneIndexBaseRefPtr &inputSceneIndex,
                         std::shared_ptr<const std::atomic<double>> timeCodesPerSecond)
      : HdSingleInputFilteringSceneIndexBase(inputSceneIndex),
        _timeCodesPerSecond(std::move(timeCodesPerSecond))
  {
  }

  void _PrimsAdded(const HdSceneIndexBase & /*sender*/,
                   const HdSceneIndexObserver::AddedPrimEntries &entries) override
  {
    _SendPrimsAdded(entries);
  }

  void _PrimsRemoved(const HdSceneIndexBase & /*sender*/,
                     const HdSceneIndexObserver::RemovedPrimEntries &entries) override
  {
    _SendPrimsRemoved(entries);
  }

  void _PrimsDirtied(const HdSceneIndexBase & /*sender*/,
                     const HdSceneIndexObserver::DirtiedPrimEntries &entries) override
  {
    _SendPrimsDirtied(entries);
  }

 private:
  std::shared_ptr<const std::atomic<double>> _timeCodesPerSecond;
};

}  // namespace

HdCyclesResyncSceneIndexRefPtr HdCyclesResyncSceneIndex::New(
    const HdSceneIndexBaseRefPtr &inputSceneIndex)
{
  auto timeCodesPerSecond = std::make_shared<std::atomic<double>>(0.0);
  return TfCreateRefPtr(new HdCyclesResyncSceneIndex(
      HdsiVelocityMotionResolvingSceneIndex::New(HdsiRenderSettingsFilteringSceneIndex::New(
          SceneGlobalsSceneIndex::New(inputSceneIndex, timeCodesPerSecond), nullptr)),
      timeCodesPerSecond));
}

HdCyclesResyncSceneIndex::HdCyclesResyncSceneIndex(
    const HdSceneIndexBaseRefPtr &inputSceneIndex,
    std::shared_ptr<std::atomic<double>> timeCodesPerSecond)
    : HdSingleInputFilteringSceneIndexBase(inputSceneIndex),
      _timeCodesPerSecond(std::move(timeCodesPerSecond))
{
}

void HdCyclesResyncSceneIndex::SetTimeCodesPerSecond(const double timeCodesPerSecond)
{
  *_timeCodesPerSecond = timeCodesPerSecond;
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

/* True for a locator of a Cycles camera setting ("primvars:cycles:camera:<setting>"). */
static bool IsCameraSettingLocator(const HdDataSourceLocator &locator)
{
  return locator.GetElementCount() >= 2 &&
         locator.GetFirstElement() == HdPrimvarsSchema::GetSchemaToken() &&
         TfStringStartsWith(locator.GetElement(1).GetString(), "cycles:camera:");
}

void HdCyclesResyncSceneIndex::_PrimsDirtied(
    const HdSceneIndexBase & /*sender*/, const HdSceneIndexObserver::DirtiedPrimEntries &entries)
{
  /* Hydra maps no primvar of a camera to its dirty bits: dirty its parameters for the Cycles
   * camera settings, so HdCyclesCamera::Sync reads them again. */
  std::optional<HdSceneIndexObserver::DirtiedPrimEntries> mapped;
  for (size_t i = 0; i < entries.size(); ++i) {
    const HdDataSourceLocatorSet &locators = entries[i].dirtyLocators;
    if (!locators.Intersects(HdPrimvarsSchema::GetDefaultLocator())) {
      continue;
    }
    /* All primvars (added or removed ones): only cameras, which this asks the scene for. */
    const bool allPrimvars = locators.Contains(HdPrimvarsSchema::GetDefaultLocator());
    if (allPrimvars ? _GetInputSceneIndex()->GetPrim(entries[i].primPath).primType !=
                          HdPrimTypeTokens->camera :
                      std::none_of(locators.begin(), locators.end(), IsCameraSettingLocator))
    {
      continue;
    }
    if (!mapped) {
      mapped = entries;
    }
    (*mapped)[i].dirtyLocators.insert(HdCameraSchema::GetDefaultLocator());
  }
  _SendPrimsDirtied(mapped ? *mapped : entries);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
