/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/instancer.h"
#include "hydra/util.h"

#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/xformSchema.h>

#include <algorithm>

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdCyclesInstancer::HdCyclesInstancer(HdSceneDelegate *delegate, const SdfPath &instancerId)
    : HdInstancer(delegate, instancerId)
{
}

HdCyclesInstancer::~HdCyclesInstancer() = default;

void HdCyclesInstancer::Sync(HdSceneDelegate *sceneDelegate,
                             HdRenderParam * /*renderParam*/,
                             HdDirtyBits *dirtyBits)
{
  _UpdateInstancer(sceneDelegate, dirtyBits);

  if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, GetId())) {
    SyncPrimvars();
  }
}

void HdCyclesInstancer::SyncPrimvars()
{
  HdSceneDelegate *const sceneDelegate = GetDelegate();
  const SdfPath &id = GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
  HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);

  auto read_array_primvar = [&](const TfToken &name, auto &out) {
    using T = std::remove_reference_t<decltype(out)>;
    if (HdPrimvarSchema pv = primvars.GetPrimvar(name)) {
      if (auto valueDs = pv.GetPrimvarValue()) {
        const VtValue v = valueDs->GetValue(0.0f);
        if (v.IsHolding<T>()) {
          out = v.UncheckedGet<T>();
        }
      }
    }
  };

  read_array_primvar(HdInstancerTokens->instanceTranslations, _translate);
  read_array_primvar(HdInstancerTokens->instanceScales, _scale);
  read_array_primvar(HdInstancerTokens->instanceTransforms, _instanceTransform);

  /* Accept different array types for quaternions. */
  if (HdPrimvarSchema pv = primvars.GetPrimvar(HdInstancerTokens->instanceRotations)) {
    if (auto valueDs = pv.GetPrimvarValue()) {
      const VtValue v = valueDs->GetValue(0.0f);
      if (v.IsHolding<VtVec4fArray>()) {
        _rotate = v.UncheckedGet<VtVec4fArray>();
      }
      else if (v.IsHolding<VtQuatfArray>()) {
        const VtQuatfArray &src = v.UncheckedGet<VtQuatfArray>();
        _rotate.clear();
        _rotate.reserve(src.size());
        for (const GfQuatf &q : src) {
          const GfVec3f &im = q.GetImaginary();
          _rotate.push_back(GfVec4f(q.GetReal(), im[0], im[1], im[2]));
        }
      }
      else if (v.IsHolding<VtQuathArray>()) {
        const VtQuathArray &src = v.UncheckedGet<VtQuathArray>();
        _rotate.clear();
        _rotate.reserve(src.size());
        for (const GfQuath &q : src) {
          const GfVec3h &im = q.GetImaginary();
          _rotate.push_back(GfVec4f(q.GetReal(), im[0], im[1], im[2]));
        }
      }
    }
  }

  /* Per-instance primvars for the prototypes' shaders. Constant ones need nothing here: UsdImaging
   * already passes them on to the prototypes, and turns the constant primvars of native instances
   * into instance primvars of their instancer. */
  _primvars.clear();
  for (const TfToken &name : PrimvarNamesAtInterpolation(primvars, HdInterpolationInstance)) {
    if (TfStringStartsWith(name.GetString(), "hydra:")) {
      continue; /* Instance transforms. */
    }
    /* shortcut: one value per instance (an elementSize above 1 reads the wrong values), upgrade
     * when per-instance array primvars are needed. */
    const VtValue value = PrimvarAsFloats(ReadPrimvar(primvars, name));
    if (value.IsEmpty()) {
      continue;
    }
    const size_t width = HdGetComponentCount(HdGetValueTupleType(value).type);
    const float *data = static_cast<const float *>(HdGetValueData(value));
    _primvars.push_back({name, width, {data, data + value.GetArraySize() * width}});
  }

  sceneDelegate->GetRenderIndex().GetChangeTracker().MarkInstancerClean(id);
}

/* Primvars of nested instances, at index `outer * numInner + inner` like the transforms. The inner
 * instancer's primvar wins over an outer one of the same name, like Karma. */
static std::vector<HdCyclesInstancer::Primvar> ComposeNested(
    std::vector<HdCyclesInstancer::Primvar> inner,
    const std::vector<HdCyclesInstancer::Primvar> &outer,
    const size_t numOuter,
    const size_t numInner)
{
  const size_t numOwn = inner.size();
  for (HdCyclesInstancer::Primvar &pv : inner) {
    std::vector<float> values;
    values.reserve(numOuter * pv.values.size());
    for (size_t p = 0; p < numOuter; ++p) {
      values.insert(values.end(), pv.values.begin(), pv.values.end());
    }
    pv.values = std::move(values);
  }
  for (const HdCyclesInstancer::Primvar &pv : outer) {
    if (std::any_of(inner.begin(), inner.begin() + numOwn, [&](const auto &own) {
          return own.name == pv.name;
        }))
    {
      continue;
    }
    inner.push_back({pv.name, pv.width, {}});
    HdCyclesInstancer::Primvar &composed = inner.back();
    for (size_t p = 0; p < numOuter; ++p) {
      for (size_t l = 0; l < numInner; ++l) {
        composed.values.insert(composed.values.end(),
                               pv.values.begin() + p * pv.width,
                               pv.values.begin() + (p + 1) * pv.width);
      }
    }
  }
  return inner;
}

VtMatrix4dArray HdCyclesInstancer::ComputeInstanceTransforms(const SdfPath &prototypeId,
                                                             std::vector<Primvar> *primvars)
{
  HdSceneDelegate *const sceneDelegate = GetDelegate();
  const SdfPath &id = GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);

  HdInstancerTopologySchema topology = HdInstancerTopologySchema::GetFromParent(prim.dataSource);
  const VtIntArray instanceIndices = topology ?
                                         topology.ComputeInstanceIndicesForProto(prototypeId) :
                                         VtIntArray();

  GfMatrix4d instanceTransform(1.0);
  if (auto matrixDs = HdXformSchema::GetFromParent(prim.dataSource).GetMatrix()) {
    instanceTransform = matrixDs->GetTypedValue(0.0f);
  }

  VtMatrix4dArray transforms;
  transforms.reserve(instanceIndices.size());

  /* Hydra applies the instance transform, scale, rotation, translation and then the instancer
   * transform (see HdInstancer). Gf matrices multiply row vectors, so each one is prepended. */
  for (const int index : instanceIndices) {
    GfMatrix4d transform = instanceTransform;

    if (index < _translate.size()) {
      GfMatrix4d translateMat(1);
      translateMat.SetTranslate(_translate[index]);
      transform = translateMat * transform;
    }

    if (index < _rotate.size()) {
      GfMatrix4d rotateMat(1);
      const GfVec4f &quat = _rotate[index];
      rotateMat.SetRotate(GfQuatd(quat[0], quat[1], quat[2], quat[3]));
      transform = rotateMat * transform;
    }

    if (index < _scale.size()) {
      GfMatrix4d scaleMat(1);
      scaleMat.SetScale(_scale[index]);
      transform = scaleMat * transform;
    }

    if (index < _instanceTransform.size()) {
      transform = _instanceTransform[index] * transform;
    }

    transforms.push_back(transform);
  }

  std::vector<Primvar> localPrimvars;
  if (primvars) {
    for (const Primvar &pv : _primvars) {
      std::vector<float> values(instanceIndices.size() * pv.width, 0.0f);
      for (size_t i = 0; i < instanceIndices.size(); ++i) {
        const size_t index = instanceIndices[i];
        if ((index + 1) * pv.width <= pv.values.size()) {
          std::copy_n(&pv.values[index * pv.width], pv.width, &values[i * pv.width]);
        }
      }
      localPrimvars.push_back({pv.name, pv.width, std::move(values)});
    }
  }

  /* Recurse into the parent instancer. */
  VtMatrix4dArray resultTransforms;

  HdInstancedBySchema instancedBy = HdInstancedBySchema::GetFromParent(prim.dataSource);
  SdfPath parentId;
  if (auto pathsDs = instancedBy.GetPaths()) {
    const VtArray<SdfPath> parentPaths = pathsDs->GetTypedValue(0.0f);
    if (!parentPaths.empty()) {
      parentId = parentPaths.front();
    }
  }
  if (parentId.IsEmpty()) {
    parentId = GetParentId();
  }

  if (!parentId.IsEmpty()) {
    auto *const parentInstancer = static_cast<HdCyclesInstancer *>(
        sceneDelegate->GetRenderIndex().GetInstancer(parentId));
    if (parentInstancer) {
      std::vector<Primvar> parentPrimvars;
      const VtMatrix4dArray parentTransforms = parentInstancer->ComputeInstanceTransforms(
          id, primvars ? &parentPrimvars : nullptr);
      /* The inner (local) instance transform applies before the outer one. */
      for (const GfMatrix4d &parentTransform : parentTransforms) {
        for (const GfMatrix4d &localTransform : transforms) {
          resultTransforms.push_back(localTransform * parentTransform);
        }
      }
      if (primvars) {
        *primvars = ComposeNested(
            std::move(localPrimvars), parentPrimvars, parentTransforms.size(), transforms.size());
      }
      return resultTransforms;
    }
  }

  if (primvars) {
    *primvars = std::move(localPrimvars);
  }
  return transforms;
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
