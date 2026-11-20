/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/geometry.h"
#include "hydra/instancer.h"
#include "hydra/material.h"
#include "hydra/session.h"
#include "hydra/util.h"
#include "kernel/types.h"
#include "scene/geometry.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/volume.h"

#include <type_traits>
#include "util/hash.h"

#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/xformSchema.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

extern Transform convert_transform(const GfMatrix4d &matrix);

/* Per-object render flags from Karma's constant primvars (inherited down the hierarchy), the names Houdini's
 * Render Geometry Settings LOP writes and USD Nodes uses for view-layer passes: holdout, ray visibility and the
 * light group (LPE tag). Cycles has no USD schema for these. */
struct HdCyclesRenderFlags {
  uint visibility = PATH_RAY_VISIBILITY_OBJECT_ALL;
  int holdout = 0; /* 1 matte, 2 background (a shadow catcher) */
  ustring lightgroup;
};

/* Karma ray mask (`*`, `primary|shadow`, `-primary&-shadow`…) → Cycles ray visibility: `|` unions terms, `&`
 * intersects, `-x` is every ray but x. Karma's `sss` has no Cycles ray. */
inline uint karma_mask_to_visibility(const std::string &mask)
{
  static const std::pair<const char *, uint> kRays[] = {{"primary", PATH_RAY_VISIBILITY_CAMERA},
                                                        {"shadow", PATH_RAY_VISIBILITY_SHADOW},
                                                        {"diffuse", PATH_RAY_VISIBILITY_DIFFUSE},
                                                        {"reflect", PATH_RAY_VISIBILITY_GLOSSY},
                                                        {"refract", PATH_RAY_VISIBILITY_TRANSMIT},
                                                        {"volume", PATH_RAY_VISIBILITY_VOLUME_SCATTER},
                                                        {"sss", 0}};
  auto ray_bits = [](const std::string &name) -> uint {
    for (const auto &ray : kRays) {
      if (name == ray.first) {
        return ray.second;
      }
    }
    return 0;
  };
  uint visibility = 0;
  for (const std::string &term : TfStringSplit(mask, "|")) {
    uint bits = PATH_RAY_VISIBILITY_ALL;
    for (std::string atom : TfStringSplit(term, "&")) {
      atom = TfStringTrim(atom);
      if (atom.empty() || atom == "*") {
        continue;
      }
      bits &= (atom[0] == '-') ? (PATH_RAY_VISIBILITY_ALL & ~ray_bits(atom.substr(1))) : ray_bits(atom);
    }
    visibility |= bits;
  }
  return visibility;
}

inline HdCyclesRenderFlags ReadRenderFlags(const HdContainerDataSourceHandle &primDs)
{
  HdCyclesRenderFlags flags;
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(primDs);
  const VtValue holdout = ReadPrimvar(primvars, TfToken("karma:object:holdoutmode"));
  if (holdout.IsHolding<int>()) {
    flags.holdout = holdout.UncheckedGet<int>();
  }
  const VtValue mask = ReadPrimvar(primvars, TfToken("karma:object:rendervisibility"));
  if (mask.IsHolding<std::string>()) {
    /* Karma has no Raycast node rays: those keep Cycles' default (visible). */
    flags.visibility = karma_mask_to_visibility(mask.UncheckedGet<std::string>()) | PATH_RAY_VISIBILITY_RAYCAST;
  }
  const VtValue tag = ReadPrimvar(primvars, TfToken("karma:object:lpetag"));
  if (tag.IsHolding<std::string>()) {
    flags.lightgroup = ustring(tag.UncheckedGet<std::string>());
  }
  return flags;
}

template<typename Base, typename CyclesBase>
HdCyclesGeometry<Base, CyclesBase>::HdCyclesGeometry(const SdfPath &rprimId)
    : Base(rprimId), _geomTransform(1.0)
{
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::_InitRepr(const TfToken &reprToken,
                                                   HdDirtyBits *dirtyBits)
{
  TF_UNUSED(reprToken);
  TF_UNUSED(dirtyBits);
}

template<typename Base, typename CyclesBase>
HdDirtyBits HdCyclesGeometry<Base, CyclesBase>::GetInitialDirtyBitsMask() const
{
  return HdChangeTracker::DirtyPrimID | HdChangeTracker::DirtyTransform |
         HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtyVisibility |
         HdChangeTracker::DirtyInstancer;
}

template<typename Base, typename CyclesBase>
HdDirtyBits HdCyclesGeometry<Base, CyclesBase>::_PropagateDirtyBits(HdDirtyBits bits) const
{
  return bits;
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::Sync(HdSceneDelegate *sceneDelegate,
                                              HdRenderParam *renderParam,
                                              HdDirtyBits *dirtyBits,
                                              const TfToken &reprToken)
{
  TF_UNUSED(reprToken);

  if (*dirtyBits == HdChangeTracker::Clean) {
    return;
  }

  Initialize(renderParam);

  Base::_UpdateInstancer(sceneDelegate, dirtyBits);
  HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), Base::GetInstancerId());
  Base::_UpdateVisibility(sceneDelegate, dirtyBits);

  const SceneLock lock(renderParam);

  {
    const HdCyclesSession *const session = static_cast<HdCyclesSession *>(renderParam);
    _motionSteps = session->MotionSteps();
    _shutterOpen = session->ShutterOpen();
    _shutterClose = session->ShutterClose();
  }

  const SdfPath &id = Base::GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
  const HdContainerDataSourceHandle &primDs = prim.dataSource;

  if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
    SdfPath materialId;
    /* Purpose here matches #HdCyclesDelegate::GetMaterialBindingPurpose. */
    if (auto pathDs =
            HdMaterialBindingsSchema::GetFromParent(primDs).GetMaterialBinding().GetPath())
    {
      materialId = pathDs->GetTypedValue(0.0f);
    }
    Base::SetMaterialId(materialId);

    const auto *const material = static_cast<const HdCyclesMaterial *>(
        sceneDelegate->GetRenderIndex().GetSprim(HdPrimTypeTokens->material,
                                                 Base::GetMaterialId()));

    array<Node *> usedShaders(1);
    if (material && material->GetCyclesShader()) {
      usedShaders[0] = material->GetCyclesShader();
    }
    else {
      /* No material: a volume gets Cycles' default volume (Principled Volume, as Blender's USD importer gives
       * it); the default surface has no volume closure, so the volume rendered as nothing. */
      if constexpr (std::is_same_v<CyclesBase, Volume>) {
        usedShaders[0] = lock.scene->default_volume;
      }
      else {
        usedShaders[0] = lock.scene->default_surface;
      }
    }

    for (Node *shader : usedShaders) {
      static_cast<Shader *>(shader)->tag_used(lock.scene);
    }

    _geom->set_used_shaders(usedShaders);
  }

  if (HdChangeTracker::IsPrimIdDirty(*dirtyBits, id) && !_instances.empty()) {
    // This needs to be corrected in the AOV
    _instances[0]->set_pass_id(Base::GetPrimId() + 1);
  }

  if (HdChangeTracker::IsTransformDirty(*dirtyBits, id)) {
    _geomTransform = GfMatrix4d(1.0);
    if (auto matrixDs = HdXformSchema::GetFromParent(primDs).GetMatrix()) {
      _geomTransform = matrixDs->GetTypedValue(0.0f);
    }
  }

  if (HdChangeTracker::IsTransformDirty(*dirtyBits, id) ||
      HdChangeTracker::IsInstancerDirty(*dirtyBits, id))
  {
    auto *const instancer = static_cast<HdCyclesInstancer *>(
        sceneDelegate->GetRenderIndex().GetInstancer(Base::GetInstancerId()));

    VtMatrix4dArray transforms;
    if (instancer) {
      transforms = instancer->ComputeInstanceTransforms(id);
    }
    else {
      // Default to a single instance with an identity transform
      transforms.push_back(GfMatrix4d(1.0));
    }

    const size_t oldSize = _instances.size();
    const size_t newSize = transforms.size();

    // Resize instance list
    for (size_t i = newSize; i < oldSize; ++i) {
      lock.scene->delete_node(_instances[i]);
    }
    _instances.resize(newSize);
    for (size_t i = oldSize; i < newSize; ++i) {
      _instances[i] = lock.scene->create_node<Object>();
      InitializeInstance(static_cast<int>(i));
      if (i == 0) {
        _instances[0]->set_pass_id(Base::GetPrimId() + 1);
      }
    }

    /* Transform motion blur: the object's and its instancer's transforms at each motion step, when either moves
     * within the shutter (Cycles: motion[center] is the frame's transform). */
    bool moving = false;
    auto stepMatrixDs = HdXformSchema::GetFromParent(primDs).GetMatrix();
    if (_motionSteps > 1) {
      std::vector<float> times;
      moving = (stepMatrixDs &&
                stepMatrixDs->GetContributingSampleTimesForInterval(_shutterOpen, _shutterClose, &times)) ||
               (instancer && instancer->IsMoving(_shutterOpen, _shutterClose));
    }
    if (moving) {
      const float metersPerUnit = static_cast<HdCyclesSession *>(renderParam)->GetStageMetersPerUnit();
      std::vector<array<Transform>> motions(_instances.size(), array<Transform>(_motionSteps));
      for (int step = 0; step < _motionSteps && moving; ++step) {
        const float time = MotionTime(step);
        const GfMatrix4d geomTransform = stepMatrixDs ? stepMatrixDs->GetTypedValue(time) : GfMatrix4d(1.0);
        VtMatrix4dArray stepTransforms;
        if (instancer) {
          stepTransforms = instancer->ComputeInstanceTransforms(id, time);
        }
        else {
          stepTransforms.push_back(GfMatrix4d(1.0));
        }
        if (stepTransforms.size() != _instances.size()) {
          moving = false; /* the instance count changes within the shutter: no motion */
          break;
        }
        for (size_t i = 0; i < _instances.size(); ++i) {
          motions[i][step] = transform_scale(make_float3(metersPerUnit)) *
                             convert_transform(geomTransform * stepTransforms[i]);
        }
      }
      if (moving) {
        for (size_t i = 0; i < _instances.size(); ++i) {
          _instances[i]->set_motion(motions[i]);
        }
      }
    }
    if (!moving) {
      for (Object *instance : _instances) {
        if (!instance->get_motion().empty()) {
          array<Transform> none;
          instance->set_motion(none);
        }
      }
    }

    /* After the resize: every instance can be masked out (invisibleIds, culled native instances), leaving none —
     * the next sync must not touch _instances[0] then (it was read out of bounds: heap corruption, crashes). */
    if (!_instances.empty()) {
      // Make sure the first object attribute is the instanceId
      assert(_instances[0]->attributes.size() >= 1 &&
             _instances[0]->attributes.front().name() == HdAovTokens->instanceId.GetString());
      _instances[0]->attributes.front() = ParamValue(HdAovTokens->instanceId.GetString(),
                                                     instancer ? +0.0f : -1.0f);
    }

    // Update transforms of all instances
    for (size_t i = 0; i < transforms.size(); ++i) {
      const float metersPerUnit =
          static_cast<HdCyclesSession *>(renderParam)->GetStageMetersPerUnit();

      const Transform tfm = transform_scale(make_float3(metersPerUnit)) *
                            convert_transform(_geomTransform * transforms[i]);
      _instances[i]->set_tfm(tfm);
    }
  }

  {
    /* Every sync: the flags are primvars, and new instances need them too. Unchanged values do not tag. */
    const HdCyclesRenderFlags flags = ReadRenderFlags(primDs);
    for (Object *instance : _instances) {
      instance->set_visibility(Base::IsVisible() ? flags.visibility : 0);
      instance->set_use_holdout(flags.holdout == 1);
      instance->set_is_shadow_catcher(flags.holdout == 2);
      instance->set_lightgroup(flags.lightgroup);
    }
  }

  // Must happen after material ID update, so that attribute decisions can be made
  // based on it (e.g. check whether an attribute is actually needed)
  bool rebuild = false;
  Populate(sceneDelegate, *dirtyBits, rebuild);

  if (_geom->is_modified() || rebuild) {
    _geom->tag_update(lock.scene, rebuild);
  }

  for (Object *instance : _instances) {
    instance->tag_update(lock.scene);
  }

  *dirtyBits = HdChangeTracker::Clean;
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::Finalize(HdRenderParam *renderParam)
{
  if (!_geom && _instances.empty()) {
    return;
  }

  const SceneLock lock(renderParam);
  const bool keep_nodes = static_cast<const HdCyclesSession *>(renderParam)->keep_nodes;

  /* Delete the instances before the geometry, since the objects reference the geometry. */
  if (!keep_nodes) {
    lock.scene->delete_nodes(set<Object *>(_instances.begin(), _instances.end()));
  }
  _instances.clear();
  _instances.shrink_to_fit();

  if (!keep_nodes) {
    lock.scene->delete_node(_geom);
  }
  _geom = nullptr;
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::Initialize(HdRenderParam *renderParam)
{
  if (_geom) {
    return;
  }

  const SceneLock lock(renderParam);

  // Create geometry
  _geom = lock.scene->create_node<CyclesBase>();
  _geom->name = Base::GetId().GetString();

  // Create default instance
  _instances.push_back(lock.scene->create_node<Object>());
  InitializeInstance(0);
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::InitializeInstance(int index)
{
  Object *instance = _instances[index];
  instance->set_geometry(_geom);
  /* The prim path: Cryptomatte object IDs hash the object name (all objects hashed "" before). */
  instance->name = _geom->name;

  instance->attributes.emplace_back(HdAovTokens->instanceId.GetString(),
                                    _instances.size() == 1 ? -1.0f : static_cast<float>(index));

  instance->set_color(make_float3(0.8f, 0.8f, 0.8f));
  instance->set_random_id(hash_uint2(hash_string(_geom->name.c_str()), index));
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
