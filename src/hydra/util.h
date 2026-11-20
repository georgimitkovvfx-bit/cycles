/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "scene/attribute.h"

#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/enums.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/timeSampleArray.h>
#include <pxr/imaging/hd/types.h>

#include <functional>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
class HdSceneDelegate;
class SdfPath;
class TfToken;
PXR_NAMESPACE_CLOSE_SCOPE

CCL_NAMESPACE_BEGIN
class Geometry;
class Object;
class Scene;
CCL_NAMESPACE_END

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Prim data source from the scene index, or empty if none is reachable. */
PXR_NS::HdSceneIndexPrim GetPrim(PXR_NS::HdSceneDelegate *delegate, const PXR_NS::SdfPath &id);

/* Typed child data source, or null if missing or of a different type. */
template<typename T>
typename PXR_NS::HdTypedSampledDataSource<T>::Handle GetTypedDataSource(
    const PXR_NS::HdContainerDataSourceHandle &container, const PXR_NS::TfToken &name)
{
  if (!container) {
    return nullptr;
  }
  return PXR_NS::HdTypedSampledDataSource<T>::Cast(container->Get(name));
}

/* Typed value at shutter offset 0, or `fallback` if missing or of a different type. */
template<typename T>
T GetTypedValue(const PXR_NS::HdContainerDataSourceHandle &container,
                const PXR_NS::TfToken &name,
                const T &fallback = T())
{
  if (!container) {
    return fallback;
  }
  const auto base = container->Get(name);
  if (const auto typed = PXR_NS::HdTypedSampledDataSource<T>::Cast(base)) {
    return typed->GetTypedValue(0.0f);
  }
  if (const auto sampled = PXR_NS::HdSampledDataSource::Cast(base)) {
    PXR_NS::VtValue v = sampled->GetValue(0.0f);
    if (v.IsHolding<T>()) {
      return v.UncheckedGet<T>();
    }
  }
  return fallback;
}

/* Sample a typed data source over the shutter into `out`. */
template<typename T, unsigned int Capacity>
void SampleTyped(const typename PXR_NS::HdTypedSampledDataSource<T>::Handle &ds,
                 float shutterOpen,
                 float shutterClose,
                 PXR_NS::HdTimeSampleArray<T, Capacity> *out)
{
  if (!ds) {
    out->Resize(0);
    return;
  }
  std::vector<float> times;
  const bool hasSamples = ds->GetContributingSampleTimesForInterval(
      shutterOpen, shutterClose, &times);
  if (!hasSamples || times.empty()) {
    out->Resize(1);
    out->times[0] = 0.0f;
    out->values[0] = ds->GetTypedValue(0.0f);
    return;
  }
  out->Resize(times.size());
  for (size_t i = 0; i < times.size(); ++i) {
    out->times[i] = times[i];
    out->values[i] = ds->GetTypedValue(times[i]);
  }
}

/* Read a primvar's value at shutter offset 0. */
PXR_NS::VtValue ReadPrimvar(const PXR_NS::HdPrimvarsSchema &primvars, const PXR_NS::TfToken &name);

/* Read a primvar's interpolation. */
PXR_NS::HdInterpolation ReadPrimvarInterpolation(const PXR_NS::HdPrimvarsSchema &primvars,
                                                 const PXR_NS::TfToken &name);

/* Read a primvar's role token. */
PXR_NS::TfToken ReadPrimvarRole(const PXR_NS::HdPrimvarsSchema &primvars,
                                const PXR_NS::TfToken &name);

/* Enumerate primvar names whose interpolation matches. */
PXR_NS::TfTokenVector PrimvarNamesAtInterpolation(const PXR_NS::HdPrimvarsSchema &primvars,
                                                  PXR_NS::HdInterpolation interpolation);

/* Primvar value as a float, float2, float3 or float4 array (scalars become 1-element arrays), or
 * empty for values without a float conversion, like matrices and strings. */
PXR_NS::VtValue PrimvarAsFloats(const PXR_NS::VtValue &value);

/* Float array primvar value as float2 array, dropping or zero-filling components. */
PXR_NS::VtVec2fArray PrimvarAsFloat2(const PXR_NS::VtValue &value);

/* The primvar Cycles uses as the UV map: "st", else "uv", else the first texture coordinate. */
PXR_NS::TfToken FindUVPrimvar(const PXR_NS::HdPrimvarsSchema &primvars);

/* Standard attribute of a primvar: the UV map, displayColor (or Cd without a displayColor) as the
 * vertex color, and names that shaders request as volume standard attributes (color, density). */
CCL_NS::AttributeStandard PrimvarStandard(const PXR_NS::HdPrimvarsSchema &primvars,
                                          const PXR_NS::TfToken &name,
                                          const PXR_NS::TfToken &uvName);

/* Attribute names a primvar is exported as, only those that shaders request: its own name (also
 * when its standard attribute is requested), its Houdini name or USD name (Cd and displayColor,
 * Alpha, uv, v, accel) when no primvar has that name, and its name with a "primvars:" prefix. */
std::vector<CCL_NS::ustring> RequestedPrimvarNames(CCL_NS::Geometry *geom,
                                                   CCL_NS::Scene *scene,
                                                   const PXR_NS::HdPrimvarsSchema &primvars,
                                                   const PXR_NS::TfToken &name,
                                                   CCL_NS::AttributeStandard std);

/* Same, with `hasPrimvar` telling which primvar names exist. */
std::vector<CCL_NS::ustring> RequestedPrimvarNames(
    CCL_NS::Geometry *geom,
    CCL_NS::Scene *scene,
    const std::function<bool(const PXR_NS::TfToken &)> &hasPrimvar,
    const PXR_NS::TfToken &name,
    CCL_NS::AttributeStandard std);

/* Set an object attribute (replacing one of the same name) from a 1-element float array primvar
 * value. Shaders read it when the geometry has no attribute of that name. */
void SetObjectAttribute(CCL_NS::Object *object,
                        const CCL_NS::ustring &name,
                        const PXR_NS::VtValue &value);

/* Same, from `width` (1 to 4) floats. */
void SetObjectAttribute(CCL_NS::Object *object,
                        const CCL_NS::ustring &name,
                        size_t width,
                        const float *data);

/* Convert a float array primvar value to a Cycles attribute, if it has the attribute's size.
 * With `dropExtraValues`, values after the attribute's size are left out. */
void ApplyPrimvars(CCL_NS::AttributeSet &attributes,
                   const CCL_NS::ustring &name,
                   PXR_NS::VtValue value,
                   CCL_NS::AttributeElement elem,
                   CCL_NS::AttributeStandard std,
                   bool dropExtraValues = false);

/* Ray visibility of a light from USD's diffuse and specular multipliers: Cycles lights have no
 * multipliers, a multiplier of 0 turns off the diffuse or glossy rays. */
unsigned int LightMultiplierVisibility(const PXR_NS::HdContainerDataSourceHandle &light);

/* Cycles ray visibility of Karma's object Render Visibility ("karma:object:rendervisibility"):
 * ray types (primary, shadow, diffuse, reflect, refract, volume, sss) or "*" combined with "|"
 * (or), "&" (and) and a "-" or "!" prefix (not), e.g. "-shadow" or "primary|shadow"; empty is
 * invisible to all rays. The result has the camera, shadow, diffuse, glossy, transmission and
 * volume scatter bits. */
unsigned int KarmaRenderVisibility(const std::string &pattern);

/* Karma's Velocity Blur (karma:object:vblur, karma:object:instance_vblur): 0 none, 1 velocity,
 * 2 acceleration blur, from the menu index (geometry settings) or its label (render settings);
 * -1 when empty or unknown. */
int VelocityBlurMode(const PXR_NS::VtValue &value);

/* Diffuse, glossy, transmission and volume scatter ray visibility of a light from Karma's light
 * Contributions ("inputs:karma:light:contribs", components diffuse, reflect, coat, refract,
 * volume, sss in a Render Visibility pattern). All of them when it is not authored. */
unsigned int KarmaLightContributions(const PXR_NS::HdContainerDataSourceHandle &light);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
