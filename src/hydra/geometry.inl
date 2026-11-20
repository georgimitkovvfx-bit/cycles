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
#include "scene/integrator.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "session/session.h"
#include "util/hash.h"

#include <pxr/imaging/hd/categoriesSchema.h>
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
         HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyPrimvar |
         HdChangeTracker::DirtyCategories;
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
    else {
      usedShaders[0] = GetDefaultShader(lock.scene);
    }

    for (Node *shader : usedShaders) {
      static_cast<Shader *>(shader)->tag_used(lock.scene);
    }

    _geom->set_used_shaders(usedShaders);
  }

  /* Light linking categories the prim belongs to (HdsiLightLinkingSceneIndex). */
  if (*dirtyBits & HdChangeTracker::DirtyCategories) {
    VtArray<TfToken> categories;
    if (HdCategoriesSchema categoriesSchema = HdCategoriesSchema::GetFromParent(primDs)) {
      categories = categoriesSchema.GetIncludedCategoryNames();
    }
    static_cast<HdCyclesSession *>(renderParam)->SetCategories(id.GetString(), categories);
  }

  /* A prototype that no point of its instancer uses has no instances. */
  if (HdChangeTracker::IsPrimIdDirty(*dirtyBits, id) && !_instances.empty()) {
    for (Object *instance : _instances) {
      SetPrimIdAttribute(instance);
    }
  }

  /* Per-object motion blur: transforms and points again at the object's motion steps. */
  if (*dirtyBits & HdChangeTracker::DirtyPrimvar) {
    const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(primDs);
    /* Without the Cycles setting, Karma's Enable Motion Blur (Render Geometry Settings LOP). */
    VtValue useMotionBlurValue = GetObjectSetting(primvars, "use_motion_blur");
    if (useMotionBlurValue.IsEmpty()) {
      useMotionBlurValue = ReadPrimvar(primvars, TfToken("karma:object:mblur"));
    }
    const bool useMotionBlur = VtValue::Cast<bool>(useMotionBlurValue).GetWithDefault(true);
    const int motionSteps = std::max(
        0, VtValue::Cast<int>(GetObjectSetting(primvars, "motion_steps")).GetWithDefault(0));
    if (useMotionBlur != _useMotionBlur || motionSteps != _motionSteps) {
      _useMotionBlur = useMotionBlur;
      _motionSteps = motionSteps;
      *dirtyBits |= HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyPoints;
    }
  }

  if (HdChangeTracker::IsTransformDirty(*dirtyBits, id)) {
    _geomTransform = GfMatrix4d(1.0);
    _geomTransformMotion.clear();
    if (auto matrixDs = HdXformSchema::GetFromParent(primDs).GetMatrix()) {
      _geomTransform = matrixDs->GetTypedValue(0.0f);

      // Motion blur: transforms at the motion steps, rendered at the center one
      const std::vector<float> motionTimes = GetObjectMotionTimes();
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

    /* Instance transforms at the motion steps when the instances move, else only at the current
     * frame. */
    std::vector<VtMatrix4dArray> stepTransforms;
    std::vector<HdCyclesInstancer::Primvar> instancePrimvars;
    if (instancer) {
      stepTransforms = instancer->ComputeInstanceTransforms(
          id,
          GetObjectMotionTimes(),
          instancePrimvarsDirty ? &instancePrimvars : nullptr);
    }
    else {
      // Default to a single instance with an identity transform
      stepTransforms.push_back(VtMatrix4dArray(1, GfMatrix4d(1.0)));
    }
    const VtMatrix4dArray &transforms = stepTransforms[stepTransforms.size() / 2];

    const size_t oldSize = _instances.size();
    const size_t newSize = transforms.size();

    // Resize instance list
    for (size_t i = newSize; i < oldSize; ++i) {
      static_cast<HdCyclesSession *>(renderParam)->DeleteNode(_instances[i]);
    }
    _instances.resize(newSize);
    for (size_t i = oldSize; i < newSize; ++i) {
      _instances[i] = lock.scene->create_node<Object>();
      InitializeInstance(static_cast<int>(i));
    }
    if (newSize > oldSize) {
      static_cast<HdCyclesSession *>(renderParam)->TagLightLinking();
    }

    // Update transforms and instance IDs of all instances. The prototype and the instances move over the same
    // motion steps, or one of them does not move.
    const size_t numSteps = std::max(_geomTransformMotion.size(),
                                     stepTransforms.size() > 1 ? stepTransforms.size() : 0);
    for (size_t i = 0; i < transforms.size(); ++i) {
      Object *const instance = _instances[i];
      instance->set_tfm(convert_transform(_geomTransform * transforms[i]));

      // The first object attribute is the instanceId, see InitializeInstance()
      instance->attributes.front() = ParamValue(HdAovTokens->instanceId.GetString(),
                                                instancer ? static_cast<float>(i) : -1.0f);

      array<Transform> motion(numSteps);
      for (size_t step = 0; step < numSteps; ++step) {
        const GfMatrix4d &geomTransform = _geomTransformMotion.empty() ?
                                              _geomTransform :
                                              _geomTransformMotion[step];
        const GfMatrix4d &instanceTransform = stepTransforms.size() > 1 ? stepTransforms[step][i] :
                                                                          transforms[i];
        motion[step] = convert_transform(geomTransform * instanceTransform);
      }
      instance->set_motion(motion);
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
  /* A refit keeps the time steps the BVH was built with, and their buffers that changing the
   * motion steps freed. */
  rebuild |= _geom->motion_steps_is_modified() || _geom->use_motion_blur_is_modified();

  if (_geom->is_modified() || rebuild) {
    _geom->tag_update(lock.scene, rebuild);
  }

  for (Object *instance : _instances) {
    instance->tag_update(lock.scene);
  }

  *dirtyBits = HdChangeTracker::Clean;
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::SetRenderTagVisible(Scene *scene, const bool visible)
{
  if (visible == _renderTagVisible) {
    return;
  }
  _renderTagVisible = visible;
  for (Object *instance : _instances) {
    instance->set_visibility(visible ? _visibility : 0);
    instance->tag_update(scene);
  }
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
      {"visible_raycast", PATH_RAY_VISIBILITY_RAYCAST},
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
      ustring("pass_id"),
  };
  // Settings of the geometry, read where it is synced
  static const std::string geometrySettings[] = {
      "use_motion_blur",
      "motion_steps",
      "use_adaptive_subdivision",
      "dicing_rate",
      "volume_step_size",
  };
  static const std::string prefix = "cycles:object:";

  uint visibility = Base::IsVisible() ? PATH_RAY_VISIBILITY_OBJECT_ALL : 0;
  uint cyclesRays = 0; /* Ray types a Cycles setting decides. */
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
      cyclesRays |= ray->second;
      if (!VtValue::Cast<bool>(value).GetWithDefault(true)) {
        visibility &= ~ray->second;
      }
      continue;
    }

    if (std::find(std::begin(geometrySettings), std::end(geometrySettings), setting) !=
        std::end(geometrySettings))
    {
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

  /* Karma's Render Visibility for the ray types without a Cycles setting, so scenes set up for
   * Karma (and Houdini's SOP Import, which makes Gaussian splats invisible to shadows) work. */
  static const TfToken karmaVisibility("karma:object:rendervisibility");
  const VtValue karma = ReadPrimvar(primvars, karmaVisibility);
  if (karma.IsHolding<std::string>() || karma.IsHolding<TfToken>()) {
    const uint karmaRays = PATH_RAY_VISIBILITY_CAMERA | PATH_RAY_VISIBILITY_SHADOW |
                           PATH_RAY_VISIBILITY_DIFFUSE | PATH_RAY_VISIBILITY_GLOSSY |
                           PATH_RAY_VISIBILITY_TRANSMIT | PATH_RAY_VISIBILITY_VOLUME_SCATTER;
    const uint visible = KarmaRenderVisibility(karma.IsHolding<TfToken>() ?
                                                   karma.UncheckedGet<TfToken>().GetString() :
                                                   karma.UncheckedGet<std::string>());
    visibility &= ~(karmaRays & ~cyclesRays & ~visible);
  }
  /* Karma's Holdout Mode: Matte is a Cycles holdout, Background a shadow catcher. */
  static const TfToken karmaHoldout("karma:object:holdoutmode");
  const int holdout = VtValue::Cast<int>(ReadPrimvar(primvars, karmaHoldout)).GetWithDefault(0);
  const auto authored = [&](const char *name) {
    return std::any_of(values.begin(), values.end(), [&](const auto &value) {
      return value.first->name == name;
    });
  };
  if (holdout == 1 && !authored("use_holdout")) {
    values.emplace_back(Object::get_node_type()->find_input(ustring("use_holdout")),
                        VtValue(true));
  }
  else if (holdout == 2 && !authored("is_shadow_catcher")) {
    values.emplace_back(Object::get_node_type()->find_input(ustring("is_shadow_catcher")),
                        VtValue(true));
  }

  _visibility = visibility;
  for (Object *instance : _instances) {
    instance->set_visibility(_renderTagVisible ? visibility : 0);
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

    instance->attributes.resize(2); /* Keep the instanceId and primId. */
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
Shader *HdCyclesGeometry<Base, CyclesBase>::GetDefaultShader(Scene *scene) const
{
  /* Like Karma and Storm, show volumes without a material by their density. */
  return _geom->is_volume() ? scene->default_volume : scene->default_surface;
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::SetPositions(
    Attribute *attr_P,
    const HdPrimvarsSchema &primvars,
    const VtVec3fArray &points,
    const size_t numPoints,
    const std::function<VtVec3fArray(const VtVec3fArray &)> &remap,
    const TfToken &pointsPrimvar)
{
  static_assert(sizeof(GfVec3f) == sizeof(packed_float3));

  /* Motion blur: positions at the motion steps, from time samples of the points (the scene index
   * resolves velocities into those). Rendered at the center step. */
  const std::vector<float> motionTimes = GetObjectMotionTimes();
  std::vector<VtVec3fArray> motionPoints;
  const HdSampledDataSourceHandle pointsDs = primvars.GetPrimvar(pointsPrimvar).GetPrimvarValue();
  std::vector<float> sampleTimes;
  if (!motionTimes.empty() && pointsDs &&
      pointsDs->GetContributingSampleTimesForInterval(
          motionTimes.front(), motionTimes.back(), &sampleTimes))
  {
    for (const float time : motionTimes) {
      const VtValue stepValue = pointsDs->GetValue(time);
      if (!stepValue.IsHolding<VtVec3fArray>() || stepValue.GetArraySize() != points.size()) {
        motionPoints.clear(); /* Changing topology. */
        break;
      }
      motionPoints.push_back(remap ? remap(stepValue.UncheckedGet<VtVec3fArray>()) :
                                     stepValue.UncheckedGet<VtVec3fArray>());
    }
  }

  /* Like Blender, the motion pass only needs the positions, motion blur also the motion BVH. */
  _geom->set_use_motion_blur(!motionPoints.empty() &&
                             _session->session->scene->integrator->get_motion_blur());
  _geom->set_motion_steps(uint(motionPoints.size()));
  if (motionPoints.empty()) {
    attr_P->remove_motion();
  }
  else {
    attr_P->add_motion(_geom);
  }

  const VtVec3fArray remapped = (remap && motionPoints.empty()) ? remap(points) : VtVec3fArray();
  const VtVec3fArray &center = !motionPoints.empty() ? motionPoints[motionPoints.size() / 2] :
                               remap                 ? remapped :
                                                       points;
  std::copy_n(reinterpret_cast<const packed_float3 *>(center.data()),
              numPoints,
              attr_P->data_for_write<packed_float3>());
  for (int step = 0; step < int(motionPoints.size()); ++step) {
    const int attrStep = attr_P->time_step_to_attr_step(step, int(motionPoints.size()));
    if (attrStep != 0) {
      std::copy_n(reinterpret_cast<const packed_float3 *>(motionPoints[step].data()),
                  numPoints,
                  attr_P->data_for_write<packed_float3>(attrStep));
    }
  }
  _geom->tag_position_modified();
}

template<typename Base, typename CyclesBase>
std::vector<float> HdCyclesGeometry<Base, CyclesBase>::GetObjectMotionTimes() const
{
  return _useMotionBlur ? _session->GetMotionTimes(_motionSteps) : std::vector<float>();
}

template<typename Base, typename CyclesBase>
VtValue HdCyclesGeometry<Base, CyclesBase>::GetObjectSetting(const HdPrimvarsSchema &primvars,
                                                             const std::string &setting)
{
  return ReadPrimvar(primvars, TfToken("cycles:object:" + setting));
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::SetPrimIdAttribute(Object *instance) const
{
  /* The primId AOV (Houdini's viewport picking) is written by every shader from this attribute,
   * like the instanceId, because the object's pass index is the Blender Pass Index. Plus one, so
   * the background is -1 after the conversion of ID AOVs. */
  instance->attributes[1] = ParamValue(HdAovTokens->primId.GetString(),
                                       static_cast<float>(Base::GetPrimId() + 1));
}

template<typename Base, typename CyclesBase>
void HdCyclesGeometry<Base, CyclesBase>::Finalize(HdRenderParam *renderParam)
{
  if (!_geom && _instances.empty()) {
    return;
  }

  const SceneLock lock(renderParam);
  const bool keep_nodes = static_cast<const HdCyclesSession *>(renderParam)->keep_nodes;
  static_cast<HdCyclesSession *>(renderParam)->RemoveLinking(Base::GetId().GetString());

  if (!keep_nodes) {
    HdCyclesSession *const session = static_cast<HdCyclesSession *>(renderParam);
    for (Object *instance : _instances) {
      session->DeleteNode(instance);
    }
    session->DeleteNode(_geom);
  }
  _instances.clear();
  _instances.shrink_to_fit();

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

  // Value is set in Sync()
  instance->attributes.emplace_back(HdAovTokens->instanceId.GetString(), -1.0f);
  instance->attributes.emplace_back(HdAovTokens->primId.GetString(), 0.0f);
  SetPrimIdAttribute(instance);

  instance->set_color(index > 0 ? _instances[0]->get_color() : make_float3(0.8f, 0.8f, 0.8f));
  instance->set_random_id(hash_uint2(hash_string(_geom->name.c_str()), index));
  // Pass Index of the object, like in Blender (UpdateObjectSettings)
  if (index > 0) {
    instance->set_pass_id(_instances[0]->get_pass_id());
  }

  // Cryptomatte object and asset names: the prim and its parent
  instance->name = _geom->name;
  instance->set_asset_name(ustring(Base::GetId().GetParentPath().GetString()));
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
