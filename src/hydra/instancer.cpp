/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/instancer.h"
#include "hydra/util.h"

#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/xformSchema.h>

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
  Arrays arrays;
  ReadPrimvars(0.0f, arrays);
  _translate = std::move(arrays.translate);
  _rotate = std::move(arrays.rotate);
  _scale = std::move(arrays.scale);
  _instanceTransform = std::move(arrays.instanceTransform);

  GetDelegate()->GetRenderIndex().GetChangeTracker().MarkInstancerClean(GetId());
}

void HdCyclesInstancer::ReadPrimvars(const float time, Arrays &out)
{
  HdSceneDelegate *const sceneDelegate = GetDelegate();
  const SdfPath &id = GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
  HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);

  auto read_array_primvar = [&](const TfToken &name, auto &result) {
    using T = std::remove_reference_t<decltype(result)>;
    if (HdPrimvarSchema pv = primvars.GetPrimvar(name)) {
      if (auto valueDs = pv.GetPrimvarValue()) {
        const VtValue v = valueDs->GetValue(time);
        if (v.IsHolding<T>()) {
          result = v.UncheckedGet<T>();
        }
      }
    }
  };

  read_array_primvar(HdInstancerTokens->instanceTranslations, out.translate);
  read_array_primvar(HdInstancerTokens->instanceScales, out.scale);
  read_array_primvar(HdInstancerTokens->instanceTransforms, out.instanceTransform);

  /* Accept different array types for quaternions. */
  if (HdPrimvarSchema pv = primvars.GetPrimvar(HdInstancerTokens->instanceRotations)) {
    if (auto valueDs = pv.GetPrimvarValue()) {
      const VtValue v = valueDs->GetValue(time);
      if (v.IsHolding<VtVec4fArray>()) {
        out.rotate = v.UncheckedGet<VtVec4fArray>();
      }
      else if (v.IsHolding<VtQuatfArray>()) {
        const VtQuatfArray &src = v.UncheckedGet<VtQuatfArray>();
        out.rotate.clear();
        out.rotate.reserve(src.size());
        for (const GfQuatf &q : src) {
          const GfVec3f &im = q.GetImaginary();
          out.rotate.push_back(GfVec4f(q.GetReal(), im[0], im[1], im[2]));
        }
      }
      else if (v.IsHolding<VtQuathArray>()) {
        const VtQuathArray &src = v.UncheckedGet<VtQuathArray>();
        out.rotate.clear();
        out.rotate.reserve(src.size());
        for (const GfQuath &q : src) {
          const GfVec3h &im = q.GetImaginary();
          out.rotate.push_back(GfVec4f(q.GetReal(), im[0], im[1], im[2]));
        }
      }
    }
  }
}

bool HdCyclesInstancer::IsMoving(const float open, const float close)
{
  HdSceneDelegate *const sceneDelegate = GetDelegate();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  std::vector<float> times;
  if (auto matrixDs = HdXformSchema::GetFromParent(prim.dataSource).GetMatrix()) {
    if (matrixDs->GetContributingSampleTimesForInterval(open, close, &times)) {
      return true;
    }
  }
  HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  for (const TfToken &name : {HdInstancerTokens->instanceTranslations,
                              HdInstancerTokens->instanceRotations,
                              HdInstancerTokens->instanceScales,
                              HdInstancerTokens->instanceTransforms})
  {
    if (HdPrimvarSchema pv = primvars.GetPrimvar(name)) {
      if (auto valueDs = pv.GetPrimvarValue()) {
        if (valueDs->GetContributingSampleTimesForInterval(open, close, &times)) {
          return true;
        }
      }
    }
  }
  SdfPath parentId;
  if (auto pathsDs = HdInstancedBySchema::GetFromParent(prim.dataSource).GetPaths()) {
    const VtArray<SdfPath> parentPaths = pathsDs->GetTypedValue(0.0f);
    if (!parentPaths.empty()) {
      parentId = parentPaths.front();
    }
  }
  if (parentId.IsEmpty()) {
    parentId = GetParentId();
  }
  if (!parentId.IsEmpty()) {
    if (auto *parent = static_cast<HdCyclesInstancer *>(sceneDelegate->GetRenderIndex().GetInstancer(parentId)))
    {
      return parent->IsMoving(open, close);
    }
  }
  return false;
}

VtMatrix4dArray HdCyclesInstancer::ComputeInstanceTransforms(const SdfPath &prototypeId)
{
  return ComputeInstanceTransforms(prototypeId, 0.0f);
}

VtMatrix4dArray HdCyclesInstancer::ComputeInstanceTransforms(const SdfPath &prototypeId, const float time)
{
  HdSceneDelegate *const sceneDelegate = GetDelegate();
  Arrays sampled;
  if (time != 0.0f) {
    ReadPrimvars(time, sampled);
  }
  const VtVec3fArray &_translate = time != 0.0f ? sampled.translate : this->_translate;
  const VtVec4fArray &_rotate = time != 0.0f ? sampled.rotate : this->_rotate;
  const VtVec3fArray &_scale = time != 0.0f ? sampled.scale : this->_scale;
  const VtMatrix4dArray &_instanceTransform = time != 0.0f ? sampled.instanceTransform :
                                                             this->_instanceTransform;
  const SdfPath &id = GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);

  HdInstancerTopologySchema topology = HdInstancerTopologySchema::GetFromParent(prim.dataSource);
  const VtIntArray instanceIndices = topology ?
                                         topology.ComputeInstanceIndicesForProto(prototypeId) :
                                         VtIntArray();

  GfMatrix4d instanceTransform(1.0);
  if (auto matrixDs = HdXformSchema::GetFromParent(prim.dataSource).GetMatrix()) {
    instanceTransform = matrixDs->GetTypedValue(time);
  }

  VtMatrix4dArray transforms;
  transforms.reserve(instanceIndices.size());

  /* Gf matrices act on row vectors (p' = p * M), so USD's order — the per-instance matrix, then scale, rotation,
   * translation, then the instancer's own transform — composes left to right. */
  for (const int index : instanceIndices) {
    GfMatrix4d transform(1.0);

    if (index < _instanceTransform.size()) {
      transform = _instanceTransform[index];
    }

    if (index < _scale.size()) {
      GfMatrix4d scaleMat(1);
      scaleMat.SetScale(_scale[index]);
      transform *= scaleMat;
    }

    if (index < _rotate.size()) {
      GfMatrix4d rotateMat(1);
      const GfVec4f &quat = _rotate[index];
      rotateMat.SetRotate(GfQuatd(quat[0], quat[1], quat[2], quat[3]));
      transform *= rotateMat;
    }

    if (index < _translate.size()) {
      GfMatrix4d translateMat(1);
      translateMat.SetTranslate(_translate[index]);
      transform *= translateMat;
    }

    transform *= instanceTransform;
    transforms.push_back(transform);
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
      for (const GfMatrix4d &parentTransform : parentInstancer->ComputeInstanceTransforms(id, time)) {
        for (const GfMatrix4d &localTransform : transforms) {
          resultTransforms.push_back(localTransform * parentTransform);  /* child, then parent */
        }
      }
      return resultTransforms;
    }
  }

  return transforms;
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
