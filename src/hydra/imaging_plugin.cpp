/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* UsdImaging adapters of the Cycles render delegate, in their own small library (hdCyclesImaging)
 * that only links USD: UsdImaging loads keyless adapters in every session, whatever the renderer,
 * so keeping them out of hdCycles keeps Cycles, its kernels and libraries out of Karma, Storm and
 * other renderers' processes. */

#include <pxr/pxr.h>

#if PXR_VERSION >= 2408
#  include <pxr/base/tf/type.h>
#  include <pxr/imaging/hd/primvarSchema.h>
#  include <pxr/imaging/hd/primvarsSchema.h>
#  include <pxr/imaging/hd/retainedDataSource.h>
#  include <pxr/usd/usdGeom/points.h>
#  include <pxr/usdImaging/usdImaging/apiSchemaAdapter.h>
#  include <pxr/usdImaging/usdImaging/dataSourceAttribute.h>

#  include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

/* Keyless API schema adapter (runs for every prim): the ids of UsdGeomPoints as the vertex
 * primvar "ids", which UsdImaging does not pass to Hydra. */
class HdCycles_PointsIdsAdapter final : public UsdImagingAPISchemaAdapter {
 public:
  using BaseAdapter = UsdImagingAPISchemaAdapter;

  HdContainerDataSourceHandle GetImagingSubprimData(
      const UsdPrim &prim,
      const TfToken &subprim,
      const TfToken & /*appliedInstanceName*/,
      const UsdImagingDataSourceStageGlobals &stageGlobals) override
  {
    if (!subprim.IsEmpty() || !prim.IsA<UsdGeomPoints>()) {
      return nullptr;
    }
    const UsdAttribute ids = UsdGeomPoints(prim).GetIdsAttr();
    if (!ids.HasAuthoredValue()) {
      return nullptr;
    }
    static const HdDataSourceLocator valueLocator = HdPrimvarsSchema::GetDefaultLocator()
                                                        .Append(UsdGeomTokens->ids)
                                                        .Append(HdPrimvarSchemaTokens->primvarValue);
    return HdRetainedContainerDataSource::New(
        HdPrimvarsSchema::GetSchemaToken(),
        HdRetainedContainerDataSource::New(
            UsdGeomTokens->ids,
            HdPrimvarSchema::Builder()
                .SetPrimvarValue(UsdImagingDataSourceAttributeNew(
                    ids, stageGlobals, prim.GetPath(), valueLocator))
                .SetInterpolation(
                    HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->vertex))
                .Build()));
  }

  HdDataSourceLocatorSet InvalidateImagingSubprim(
      const UsdPrim & /*prim*/,
      const TfToken &subprim,
      const TfToken & /*appliedInstanceName*/,
      const TfTokenVector &properties,
      UsdImagingPropertyInvalidationType /*invalidationType*/) override
  {
    if (subprim.IsEmpty() &&
        std::find(properties.begin(), properties.end(), UsdGeomTokens->ids) != properties.end())
    {
      return HdDataSourceLocatorSet(
          HdPrimvarsSchema::GetDefaultLocator().Append(UsdGeomTokens->ids));
    }
    return HdDataSourceLocatorSet();
  }
};

TF_REGISTRY_FUNCTION(TfType)
{
  TfType::Define<HdCycles_PointsIdsAdapter, TfType::Bases<HdCycles_PointsIdsAdapter::BaseAdapter>>()
      .SetFactory<UsdImagingAPISchemaAdapterFactory<HdCycles_PointsIdsAdapter>>();
}

PXR_NAMESPACE_CLOSE_SCOPE
#endif
