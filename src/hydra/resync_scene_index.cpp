/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/resync_scene_index.h"
#include "hydra/mesh_light.h"
#include "hydra/util.h"

#include <algorithm>
#include <optional>

#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/cameraSchema.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneGlobalsSchema.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/renderSettingsFilteringSceneIndex.h>
#include <pxr/imaging/hdsi/velocityMotionResolvingSceneIndex.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

/* Karma's Velocity Blur of geometry and of the instances of point instancers (Render Geometry
 * Settings LOP), as the velocity motion mode of HdsiVelocityMotionResolvingSceneIndex. */
const TfToken &VelocityBlurPrimvar(const TfToken &primType)
{
  static const TfToken vblur("karma:object:vblur");
  static const TfToken instanceVblur("karma:object:instance_vblur");
  return primType == HdPrimTypeTokens->instancer ? instanceVblur : vblur;
}

HdContainerDataSourceHandle VelocityMotionMode(const HdSceneIndexPrim &prim)
{
  const HdSampledDataSourceHandle ds = HdPrimvarsSchema::GetFromParent(prim.dataSource)
                                           .GetPrimvar(VelocityBlurPrimvar(prim.primType))
                                           .GetPrimvarValue();
  const int mode = ds ? VelocityBlurMode(ds->GetValue(0.0f)) : -1;
  if (mode < 0) {
    return nullptr;
  }
  /* Karma without velocity blur interpolates the time samples, Velocity Blur ignores the
   * accelerations. */
  const TfToken &token = mode == 0 ? HdsiVelocityMotionResolvingSceneIndexTokens->ignore :
                         mode == 1 ? HdsiVelocityMotionResolvingSceneIndexTokens->noAcceleration :
                                     HdsiVelocityMotionResolvingSceneIndexTokens->enable;
  const HdTokenDataSourceHandle tokenDs = HdRetainedTypedSampledDataSource<TfToken>::New(token);
  const TfToken &name = HdsiVelocityMotionResolvingSceneIndexTokens->velocityMotionMode;
  /* shortcut: authored both as a constant primvar and as a prim-level data source, the header
   * does not say which one the scene index reads; drop the unused one once tested. */
  return HdRetainedContainerDataSource::New(
      HdPrimvarsSchema::GetSchemaToken(),
      HdRetainedContainerDataSource::New(
          name,
          HdPrimvarSchema::Builder()
              .SetPrimvarValue(tokenDs)
              .SetInterpolation(
                  HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->constant))
              .Build()),
      name,
      tokenDs);
}

/* Sets the frame rate in the scene globals of the root prim to the render delegate's, and the
 * velocity motion mode of prims with Karma's Velocity Blur. */
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
    if (const HdContainerDataSourceHandle mode = VelocityMotionMode(prim)) {
      prim.dataSource = HdOverlayContainerDataSource::OverlayedContainerDataSources(
          mode, prim.dataSource);
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
    /* A changed Velocity Blur changes the velocity motion mode. */
    static const HdDataSourceLocator modeLocator = HdPrimvarsSchema::GetDefaultLocator().Append(
        HdsiVelocityMotionResolvingSceneIndexTokens->velocityMotionMode);
    std::optional<HdSceneIndexObserver::DirtiedPrimEntries> mapped;
    for (size_t i = 0; i < entries.size(); ++i) {
      const HdDataSourceLocatorSet &locators = entries[i].dirtyLocators;
      for (const TfToken &primType : {HdPrimTypeTokens->mesh, HdPrimTypeTokens->instancer}) {
        if (locators.Intersects(
                HdPrimvarsSchema::GetDefaultLocator().Append(VelocityBlurPrimvar(primType))))
        {
          if (!mapped) {
            mapped = entries;
          }
          (*mapped)[i].dirtyLocators.insert(modeLocator);
          (*mapped)[i].dirtyLocators.insert(HdDataSourceLocator(
              HdsiVelocityMotionResolvingSceneIndexTokens->velocityMotionMode));
        }
      }
    }
    _SendPrimsDirtied(mapped ? *mapped : entries);
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
    /* Nor the light of geometry lights: dirty the material of the gprim for a change of its
     * light or of its light links (HdCyclesGeometryLight). */
    if (entries[i].primPath.GetNameToken() == GeometryLightChildName() &&
        locators.Intersects(HdLightSchema::GetDefaultLocator()))
    {
      if (!mapped) {
        mapped = entries;
      }
      mapped->emplace_back(entries[i].primPath.GetParentPath(),
                           HdMaterialBindingsSchema::GetDefaultLocator());
    }
    /* Nor material bindings of lights: dirty a dome light's parameters for a material bound to
     * it as the world (HdCyclesLight::Sync). */
    if (locators.Intersects(HdMaterialBindingsSchema::GetDefaultLocator()) &&
        _GetInputSceneIndex()->GetPrim(entries[i].primPath).primType ==
            HdPrimTypeTokens->domeLight)
    {
      if (!mapped) {
        mapped = entries;
      }
      (*mapped)[i].dirtyLocators.insert(HdLightSchema::GetDefaultLocator());
    }
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
