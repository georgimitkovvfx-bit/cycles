/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/geometry.h"
#include "hydra/instancer.h"
#include "hydra/material.h"
#include "hydra/session.h"
#include "hydra/node_util.h"
#include "hydra/util.h"
#include "scene/geometry.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "util/hash.h"

#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/xformSchema.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

extern Transform convert_transform(const GfMatrix4d &matrix);

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
         HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyPrimvar;
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
    else if (_geom->is_volume()) {
      /* Like Karma and Storm, show volumes without a material by their density. */
      usedShaders[0] = lock.scene->default_volume;
    }
    else {
      usedShaders[0] = lock.scene->default_surface;
    }

    for (Node *shader : usedShaders) {
      static_cast<Shader *>(shader)->tag_used(lock.scene);
    }

    _geom->set_used_shaders(usedShaders);
  }

  /* A prototype that no point of its instancer uses has no instances. */
  if (HdChangeTracker::IsPrimIdDirty(*dirtyBits, id) && !_instances.empty()) {
    // This needs to be corrected in the AOV
    _instances[0]->set_pass_id(Base::GetPrimId() + 1);
  }

  if (HdChangeTracker::IsTransformDirty(*dirtyBits, id)) {
    _geomTransform = GfMatrix4d(1.0);
    _geomTransformMotion.clear();
    if (auto matrixDs = HdXformSchema::GetFromParent(primDs).GetMatrix()) {
      _geomTransform = matrixDs->GetTypedValue(0.0f);

      // Motion blur: transforms at the motion steps, rendered at the center one
      const std::vector<float> motionTimes =
          static_cast<HdCyclesSession *>(renderParam)->GetMotionTimes();
      std::vector<float> sampleTimes;
      if (!motionTimes.empty() && matrixDs->GetContributingSampleTimesForInterval(
                                      motionTimes.front(), motionTimes.back(), &sampleTimes))
      {
        for (const float time : motionTimes) {
          _geomTransformMotion.push_back(matrixDs->GetTypedValue(time));
        }
        _geomTransform = _geomTransformMotion[motionTimes.size() / 2];
      }
    }
  }

  /* Shader and AOV changes (DirtyMaterialId, DirtyPrimvar) change which instance primvars are
   * needed. */
  const bool instancePrimvarsDirty = HdChangeTracker::IsInstancerDirty(*dirtyBits, id) ||
                                     (*dirtyBits & (HdChangeTracker::DirtyPrimvar |
                                                    HdChangeTracker::DirtyMaterialId));
  if (HdChangeTracker::IsTransformDirty(*dirtyBits, id) || instancePrimvarsDirty) {
    auto *const instancer = static_cast<HdCyclesInstancer *>(
        sceneDelegate->GetRenderIndex().GetInstancer(Base::GetInstancerId()));

    VtMatrix4dArray transforms;
    std::vector<HdCyclesInstancer::Primvar> instancePrimvars;
    if (instancer) {
      transforms = instancer->ComputeInstanceTransforms(
          id, instancePrimvarsDirty ? &instancePrimvars : nullptr);
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
    }

    /* After the resize: a prototype without instances has no object left to write to. */
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

      array<Transform> motion(_geomTransformMotion.size());
      for (size_t step = 0; step < _geomTransformMotion.size(); ++step) {
        motion[step] = transform_scale(make_float3(metersPerUnit)) *
                       convert_transform(_geomTransformMotion[step] * transforms[i]);
      }
      _instances[i]->set_motion(motion);
    }

    if (instancePrimvarsDirty) {
      SetInstancePrimvars(lock.scene, primDs, instancePrimvars);
    }
  }

  if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id) ||
      HdChangeTracker::IsInstancerDirty(*dirtyBits, id) ||
      (*dirtyBits & HdChangeTracker::DirtyPrimvar))
  {
    UpdateObjectSettings(primDs);
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
void HdCyclesGeometry<Base, CyclesBase>::UpdateObjectSettings(
    const HdContainerDataSourceHandle &primDs)
{
  // Per-object settings from constant "cycles:object:<setting>" primvars, like Blender's object
  // Visibility panel. "visible_<ray>" hides the object from a ray type, the others set the
  // Cycles object socket of the same name.
  static const std::unordered_map<std::string, uint> rayVisibility = {
      {"visible_camera", PATH_RAY_VISIBILITY_CAMERA},
      {"visible_diffuse", PATH_RAY_VISIBILITY_DIFFUSE},
      {"visible_glossy", PATH_RAY_VISIBILITY_GLOSSY},
      {"visible_transmission", PATH_RAY_VISIBILITY_TRANSMIT},
      {"visible_volume_scatter", PATH_RAY_VISIBILITY_VOLUME_SCATTER},
      {"visible_shadow", PATH_RAY_VISIBILITY_SHADOW},
  };
  static const ustring objectSettings[] = {
      ustring("is_shadow_catcher"),
      ustring("use_holdout"),
      ustring("is_caustics_caster"),
      ustring("is_caustics_receiver"),
      ustring("shadow_terminator_geometry_offset"),
      ustring("shadow_terminator_shading_offset"),
      ustring("ao_distance"),
      ustring("lightgroup"),
  };
  static const std::string prefix = "cycles:object:";

  uint visibility = Base::IsVisible() ? PATH_RAY_VISIBILITY_ALL : 0;
  std::vector<std::pair<const SocketType *, VtValue>> values;

  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(primDs);
  for (const TfToken &primvarName : PrimvarNamesAtInterpolation(primvars, HdInterpolationConstant))
  {
    if (!TfStringStartsWith(primvarName.GetString(), prefix)) {
      continue;
    }
    const std::string setting = primvarName.GetString().substr(prefix.size());
    const VtValue value = ReadPrimvar(primvars, primvarName);

    const auto ray = rayVisibility.find(setting);
    if (ray != rayVisibility.end()) {
      if (!VtValue::Cast<bool>(value).GetWithDefault(true)) {
        visibility &= ~ray->second;
      }
      continue;
    }

    const ustring socketName(setting);
    const bool known = std::find(std::begin(objectSettings), std::end(objectSettings),
                                 socketName) != std::end(objectSettings);
    const SocketType *socket = known ? Object::get_node_type()->find_input(socketName) : nullptr;
    if (!socket) {
      TF_WARN("Unknown Cycles object setting %s on %s",
              primvarName.GetText(),
              Base::GetId().GetText());
      continue;
    }
    values.emplace_back(socket, value);
  }

  for (Object *instance : _instances) {
    instance->set_visibility(visibility);
    // Settings without a primvar go back to their default
    for (const ustring &socketName : objectSettings) {
      instance->set_default_value(*Object::get_node_type()->find_input(socketName));
    }
    for (const auto &[socket, value] : values) {
      SetNodeValue(instance, *socket, value);
    }
  }
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::SetInstancePrimvars(
    Scene *scene,
    const HdContainerDataSourceHandle &primDs,
    const std::vector<HdCyclesInstancer::Primvar> &primvars)
{
  static const TfToken Cd("Cd");
  const auto find = [&](const TfToken &name) -> const HdCyclesInstancer::Primvar * {
    for (const HdCyclesInstancer::Primvar &pv : primvars) {
      if (pv.name == name) {
        assert(pv.values.size() == _instances.size() * pv.width);
        return &pv;
      }
    }
    return nullptr;
  };

  /* Instance primvars are object attributes, only the names that shaders read. Cycles uses one
   * only where the prototype has no attribute of that name, so the prototype's own primvar wins,
   * like Karma. */
  std::vector<std::pair<ustring, const HdCyclesInstancer::Primvar *>> attributes;
  for (const HdCyclesInstancer::Primvar &pv : primvars) {
    for (const ustring &name : RequestedPrimvarNames(
             _geom,
             scene,
             [&](const TfToken &primvarName) { return find(primvarName) != nullptr; },
             pv.name,
             ATTR_STD_NONE))
    {
      attributes.emplace_back(name, &pv);
    }
  }

  /* The Object Info color from a per-instance displayColor (or Cd), unless the prototype has its
   * own constant color (SetInstanceColor). */
  const HdPrimvarsSchema ownPrimvars = HdPrimvarsSchema::GetFromParent(primDs);
  bool ownColor = false;
  for (const TfToken &name : {HdTokens->displayColor, Cd}) {
    ownColor |= ReadPrimvarInterpolation(ownPrimvars, name) == HdInterpolationConstant &&
                PrimvarStandard(ownPrimvars, name, TfToken()) == ATTR_STD_VERTEX_COLOR;
  }
  const HdCyclesInstancer::Primvar *color = find(HdTokens->displayColor);
  if (!color) {
    color = find(Cd);
  }

  for (size_t i = 0; i < _instances.size(); ++i) {
    Object *instance = _instances[i];
    if (!ownColor) {
      instance->set_color((color && color->width == 3) ? make_float3(color->values[i * 3],
                                                                     color->values[i * 3 + 1],
                                                                     color->values[i * 3 + 2]) :
                                                         make_float3(0.8f, 0.8f, 0.8f));
    }

    instance->attributes.resize(1); /* Keep the instanceId. */
    for (const auto &[name, pv] : attributes) {
      SetObjectAttribute(instance, name, pv->width, &pv->values[i * pv->width]);
    }
  }
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::SetInstanceColor(const VtValue &value)
{
  if (value.IsHolding<VtVec3fArray>() && value.GetArraySize() == 1) {
    const GfVec3f color = value.UncheckedGet<VtVec3fArray>()[0];
    for (Object *instance : _instances) {
      instance->set_color(make_float3(color[0], color[1], color[2]));
    }
  }
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

  _session = static_cast<const HdCyclesSession *>(renderParam);
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

  instance->attributes.emplace_back(HdAovTokens->instanceId.GetString(),
                                    _instances.size() == 1 ? -1.0f : static_cast<float>(index));

  instance->set_color(index > 0 ? _instances[0]->get_color() : make_float3(0.8f, 0.8f, 0.8f));
  instance->set_random_id(hash_uint2(hash_string(_geom->name.c_str()), index));

  // Cryptomatte object and asset names: the prim and its parent
  instance->name = _geom->name;
  instance->set_asset_name(ustring(Base::GetId().GetParentPath().GetString()));
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
