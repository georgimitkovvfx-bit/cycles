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
#include <pxr/base/vt/traits.h>
#include <pxr/base/vt/typeHeaders.h> /* The element types VtVisitValue visits. */
#include <pxr/base/vt/visitValue.h>
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneGlobalsSchema.h>
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

  /* Sampled again for the prototypes, the transform or the instance transforms changed. */
  _samples.clear();

  if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, GetId())) {
    SyncPrimvars();
  }
}

const std::vector<HdCyclesInstancer::Sample> &HdCyclesInstancer::GetSamples(
    const std::vector<float> &times)
{
  /* Velocities move the instances by the frame rate, which changes without syncing instancers. */
  double fps = 0.0;
  if (HdDoubleDataSourceHandle fpsDs = HdSceneGlobalsSchema::GetFromSceneIndex(
                                           GetDelegate()->GetRenderIndex().GetTerminalSceneIndex())
                                           .GetTimeCodesPerSecond())
  {
    fps = fpsDs->GetTypedValue(0.0f);
  }
  if (!_samples.empty() && times == _sampleTimes && fps == _sampleFps) {
    return _samples;
  }
  _sampleTimes = times;
  _sampleFps = fps;
  _samples.clear();

  const HdSceneIndexPrim prim = GetPrim(GetDelegate(), GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const HdMatrixDataSourceHandle matrixDs =
      HdXformSchema::GetFromParent(prim.dataSource).GetMatrix();
  auto primvarDs = [&](const TfToken &name) -> HdSampledDataSourceHandle {
    return primvars.GetPrimvar(name).GetPrimvarValue();
  };
  const HdSampledDataSourceHandle translateDs = primvarDs(HdInstancerTokens->instanceTranslations);
  const HdSampledDataSourceHandle rotateDs = primvarDs(HdInstancerTokens->instanceRotations);
  const HdSampledDataSourceHandle scaleDs = primvarDs(HdInstancerTokens->instanceScales);
  const HdSampledDataSourceHandle transformDs = primvarDs(HdInstancerTokens->instanceTransforms);

  /* Like the rprim transforms, only sampled over the shutter when something moves. The scene
   * index resolves velocities into time samples of the instance transforms. */
  bool moving = false;
  std::vector<float> sampleTimes;
  for (const HdSampledDataSourceHandle &ds :
       {HdSampledDataSourceHandle(matrixDs), translateDs, rotateDs, scaleDs, transformDs})
  {
    moving |= !times.empty() && ds &&
              ds->GetContributingSampleTimesForInterval(times.front(), times.back(), &sampleTimes);
  }

  for (const float time : moving ? times : std::vector<float>{0.0f}) {
    Sample &sample = _samples.emplace_back();
    if (matrixDs) {
      sample.xform = matrixDs->GetTypedValue(time);
    }

    auto read_array = [&](const HdSampledDataSourceHandle &ds, auto &out) {
      using T = std::remove_reference_t<decltype(out)>;
      if (ds) {
        const VtValue v = ds->GetValue(time);
        if (v.IsHolding<T>()) {
          out = v.UncheckedGet<T>();
        }
      }
    };
    read_array(translateDs, sample.translate);
    read_array(scaleDs, sample.scale);
    read_array(transformDs, sample.instanceTransform);

    /* Accept different array types for quaternions. */
    if (rotateDs) {
      const VtValue v = rotateDs->GetValue(time);
      if (v.IsHolding<VtVec4fArray>()) {
        sample.rotate = v.UncheckedGet<VtVec4fArray>();
      }
      else if (v.IsHolding<VtQuatfArray>()) {
        const VtQuatfArray &src = v.UncheckedGet<VtQuatfArray>();
        sample.rotate.reserve(src.size());
        for (const GfQuatf &q : src) {
          const GfVec3f &im = q.GetImaginary();
          sample.rotate.push_back(GfVec4f(q.GetReal(), im[0], im[1], im[2]));
        }
      }
      else if (v.IsHolding<VtQuathArray>()) {
        const VtQuathArray &src = v.UncheckedGet<VtQuathArray>();
        sample.rotate.reserve(src.size());
        for (const GfQuath &q : src) {
          const GfVec3h &im = q.GetImaginary();
          sample.rotate.push_back(GfVec4f(q.GetReal(), im[0], im[1], im[2]));
        }
      }
    }
  }

  /* Instances can only be matched over the shutter while their number stays the same. */
  const Sample &center = _samples[_samples.size() / 2];
  for (const Sample &sample : _samples) {
    if (sample.translate.size() != center.translate.size() ||
        sample.rotate.size() != center.rotate.size() ||
        sample.scale.size() != center.scale.size() ||
        sample.instanceTransform.size() != center.instanceTransform.size())
    {
      _samples = {Sample(center)};
      break;
    }
  }
  return _samples;
}

void HdCyclesInstancer::SyncPrimvars()
{
  HdSceneDelegate *const sceneDelegate = GetDelegate();
  const SdfPath &id = GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
  HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);

  /* Per-instance primvars for the prototypes' shaders. Constant ones need nothing here: UsdImaging
   * already passes them on to the prototypes, and turns the constant primvars of native instances
   * into instance primvars of their instancer. */
  _primvars.clear();
  for (const TfToken &name : PrimvarNamesAtInterpolation(primvars, HdInterpolationInstance)) {
    if (TfStringStartsWith(name.GetString(), "hydra:")) {
      continue; /* Instance transforms. */
    }
    const VtValue raw = ReadPrimvar(primvars, name);
    Primvar pv{name, 0, {}, {}};
    /* Object settings of native instances, like the light group: UsdImaging moves the constant
     * primvars of instanceable prims here. Kept with their own type, strings too. */
    if (TfStringStartsWith(name.GetString(), "cycles:object:")) {
      VtVisitValue(raw, [&](const auto &array) {
        if constexpr (VtIsArray<std::decay_t<decltype(array)>>::value) {
          pv.elements.assign(array.begin(), array.end());
        }
      });
    }
    /* shortcut: one value per instance (an elementSize above 1 reads the wrong values), upgrade
     * when per-instance array primvars are needed. */
    const VtValue value = PrimvarAsFloats(raw);
    if (!value.IsEmpty()) {
      pv.width = HdGetComponentCount(HdGetValueTupleType(value).type);
      const float *data = static_cast<const float *>(HdGetValueData(value));
      pv.values.assign(data, data + value.GetArraySize() * pv.width);
    }
    if (pv.width > 0 || !pv.elements.empty()) {
      _primvars.push_back(std::move(pv));
    }
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
    const auto repeat = [&](auto &own) {
      std::remove_reference_t<decltype(own)> values;
      values.reserve(numOuter * own.size());
      for (size_t p = 0; p < numOuter; ++p) {
        values.insert(values.end(), own.begin(), own.end());
      }
      own = std::move(values);
    };
    repeat(pv.values);
    repeat(pv.elements);
  }
  for (const HdCyclesInstancer::Primvar &pv : outer) {
    if (std::any_of(inner.begin(), inner.begin() + numOwn, [&](const auto &own) {
          return own.name == pv.name;
        }))
    {
      continue;
    }
    inner.push_back({pv.name, pv.width, {}, {}});
    HdCyclesInstancer::Primvar &composed = inner.back();
    for (size_t p = 0; p < numOuter; ++p) {
      for (size_t l = 0; l < numInner; ++l) {
        composed.values.insert(composed.values.end(),
                               pv.values.begin() + p * pv.width,
                               pv.values.begin() + (p + 1) * pv.width);
        if (!pv.elements.empty()) {
          composed.elements.push_back(pv.elements[p]);
        }
      }
    }
  }
  return inner;
}

std::vector<VtMatrix4dArray> HdCyclesInstancer::ComputeInstanceTransforms(
    const SdfPath &prototypeId, const std::vector<float> &times, std::vector<Primvar> *primvars)
{
  HdSceneDelegate *const sceneDelegate = GetDelegate();
  const SdfPath &id = GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);

  HdInstancerTopologySchema topology = HdInstancerTopologySchema::GetFromParent(prim.dataSource);
  const VtIntArray instanceIndices = topology ?
                                         topology.ComputeInstanceIndicesForProto(prototypeId) :
                                         VtIntArray();

  std::vector<VtMatrix4dArray> transforms;
  for (const Sample &sample : GetSamples(times)) {
    VtMatrix4dArray &stepTransforms = transforms.emplace_back();
    stepTransforms.reserve(instanceIndices.size());

    /* Hydra applies the instance transform, scale, rotation, translation and then the instancer
     * transform (see HdInstancer). Gf matrices multiply row vectors, so each one is prepended. */
    for (const int index : instanceIndices) {
      GfMatrix4d transform = sample.xform;

      if (index < sample.translate.size()) {
        GfMatrix4d translateMat(1);
        translateMat.SetTranslate(sample.translate[index]);
        transform = translateMat * transform;
      }

      if (index < sample.rotate.size()) {
        GfMatrix4d rotateMat(1);
        const GfVec4f &quat = sample.rotate[index];
        rotateMat.SetRotate(GfQuatd(quat[0], quat[1], quat[2], quat[3]));
        transform = rotateMat * transform;
      }

      if (index < sample.scale.size()) {
        GfMatrix4d scaleMat(1);
        scaleMat.SetScale(sample.scale[index]);
        transform = scaleMat * transform;
      }

      if (index < sample.instanceTransform.size()) {
        transform = sample.instanceTransform[index] * transform;
      }

      stepTransforms.push_back(transform);
    }
  }

  std::vector<Primvar> localPrimvars;
  if (primvars) {
    for (const Primvar &pv : _primvars) {
      std::vector<float> values(instanceIndices.size() * pv.width, 0.0f);
      std::vector<VtValue> elements(pv.elements.empty() ? 0 : instanceIndices.size());
      for (size_t i = 0; i < instanceIndices.size(); ++i) {
        const size_t index = instanceIndices[i];
        if ((index + 1) * pv.width <= pv.values.size()) {
          std::copy_n(&pv.values[index * pv.width], pv.width, &values[i * pv.width]);
        }
        if (index < pv.elements.size()) {
          elements[i] = pv.elements[index];
        }
      }
      localPrimvars.push_back({pv.name, pv.width, std::move(values), std::move(elements)});
    }
  }

  /* Recurse into the parent instancer. */
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
      const std::vector<VtMatrix4dArray> parentTransforms =
          parentInstancer->ComputeInstanceTransforms(
              id, times, primvars ? &parentPrimvars : nullptr);
      /* The inner (local) instance transform applies before the outer one. Either instancer can
       * have one step only, when it does not move. */
      std::vector<VtMatrix4dArray> resultTransforms(
          std::max(transforms.size(), parentTransforms.size()));
      for (size_t step = 0; step < resultTransforms.size(); ++step) {
        const VtMatrix4dArray &local = transforms[std::min(step, transforms.size() - 1)];
        for (const GfMatrix4d &parentTransform :
             parentTransforms[std::min(step, parentTransforms.size() - 1)])
        {
          for (const GfMatrix4d &localTransform : local) {
            resultTransforms[step].push_back(localTransform * parentTransform);
          }
        }
      }
      if (primvars) {
        *primvars = ComposeNested(std::move(localPrimvars),
                                  parentPrimvars,
                                  parentTransforms.front().size(),
                                  instanceIndices.size());
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
