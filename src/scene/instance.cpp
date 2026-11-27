/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "scene/instance.h"
#include "bvh/params.h"
#include "device/device.h"
#include "scene/devicescene.h"
#include "scene/geometry.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader.h"

#include "util/hash.h"
#include "util/set.h"
#include "util/progress.h"
#include "util/tbb.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

#ifdef WITH_EMBREE
#  include <embree4/rtcore_config.h>
#endif

CCL_NAMESPACE_BEGIN

namespace {

/* Deeper trees are a mistake of the application (a group placing itself): stop there. */
constexpr size_t kMaxDepth = 16;

bool same_attributes(const vector<ParamValue> &a, const vector<ParamValue> &b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i].name() != b[i].name() || a[i].type() != b[i].type() || a[i].nvalues() != b[i].nvalues() ||
        std::memcmp(a[i].data(), b[i].data(), a[i].datasize()) != 0)
    {
      return false;
    }
  }
  return true;
}

/* An element's transform at a motion step: a level without motion has one step for all. */
const Transform &element_transform(const InstanceArray *array, const size_t step, const size_t element)
{
  return array->steps[std::min(step, array->steps.size() - 1)][element];
}

}  // namespace

NODE_DEFINE(InstanceExpansion)
{
  NodeType *type = NodeType::add(
      "instance_expansion", create, NodeType::NONE, Procedural::get_node_base_type());
  return type;
}

InstanceExpansion::InstanceExpansion() : Procedural(get_node_type()) {}

/* The placed objects belong to the scene, which deletes them after its procedurals. */
InstanceExpansion::~InstanceExpansion() = default;

InstanceExpansion *InstanceExpansion::find(Scene *scene)
{
  for (Procedural *procedural : scene->procedurals) {
    if (procedural->type == get_node_type()) {
      return static_cast<InstanceExpansion *>(procedural);
    }
  }
  return nullptr;
}

size_t InstanceExpansion::num_placements() const
{
  size_t count = 0;
  for (const auto &[array, objects] : placed_) {
    count += objects.size();
  }
  return count;
}

size_t InstanceExpansion::num_nested() const
{
  size_t count = 0;
  for (const auto &[array, objects] : placed_) {
    for (const Object *object : objects) {
      count += object->instance_root != nullptr;
    }
  }
  return count;
}

size_t InstanceExpansion::num_nested_placements() const
{
  size_t count = 0;
  for (const auto &[array, objects] : placed_) {
    for (const Object *object : objects) {
      if (object->instance_root) {
        count += array->size() * placements_per_element(array);
      }
    }
  }
  return count;
}

InstanceExpansion *InstanceExpansion::get(Scene *scene)
{
  if (InstanceExpansion *expansion = find(scene)) {
    return expansion;
  }
  unique_ptr<InstanceExpansion> node = make_unique<InstanceExpansion>();
  InstanceExpansion *expansion = node.get();
  node->set_owner(scene);
  scene->procedurals.push_back(std::move(node));
  scene->procedural_manager->tag_update();
  return expansion;
}

InstanceArray *InstanceExpansion::create_array()
{
  arrays_.push_back(make_unique<InstanceArray>());
  return arrays_.back().get();
}

InstanceGroup *InstanceExpansion::create_group()
{
  groups_.push_back(make_unique<InstanceGroup>());
  return groups_.back().get();
}

void InstanceExpansion::delete_array(Scene *scene, InstanceArray *array)
{
  if (const auto it = placed_.find(array); it != placed_.end()) {
    scene->delete_nodes(std::set<Object *>(it->second.begin(), it->second.end()), scene);
    placed_.erase(it);
  }
  arrays_.erase(std::remove_if(arrays_.begin(),
                               arrays_.end(),
                               [array](const unique_ptr<InstanceArray> &a) { return a.get() == array; }),
                arrays_.end());
  tag_update(scene);
}

void InstanceExpansion::delete_group(Scene *scene, InstanceGroup *group)
{
  groups_.erase(std::remove_if(groups_.begin(),
                               groups_.end(),
                               [group](const unique_ptr<InstanceGroup> &g) { return g.get() == group; }),
                groups_.end());
  tag_update(scene);
}

void InstanceExpansion::tag_update(Scene *scene)
{
  scene->procedural_manager->tag_update();
}

const vector<Object *> &InstanceExpansion::placements(const InstanceArray *array) const
{
  const auto it = placed_.find(array);
  return it != placed_.end() ? it->second : no_placements_;
}

bool InstanceExpansion::subtree_modified(const InstanceArray *array) const
{
  if (array->modified || (array->object && array->object->is_modified())) {
    return true;
  }
  if (const InstanceGroup *group = array->group) {
    if (group->modified) {
      return true;
    }
    for (const InstanceGroup::Member &member : group->objects) {
      if (member.object->is_modified()) {
        return true;
      }
    }
    for (const InstanceArray *inner : group->arrays) {
      if (subtree_modified(inner)) {
        return true;
      }
    }
  }
  return false;
}

void InstanceExpansion::generate(Scene *scene, Progress & /*progress*/)
{
  const bool nests = device_nests(scene);
  std::unordered_set<const InstanceArray *> inner;
  for (const unique_ptr<InstanceGroup> &group : groups_) {
    inner.insert(group->arrays.begin(), group->arrays.end());
  }

  /* An array that went into a group places nothing itself any more. */
  for (auto it = placed_.begin(); it != placed_.end();) {
    if (inner.count(it->first)) {
      scene->delete_nodes(std::set<Object *>(it->second.begin(), it->second.end()), scene);
      it = placed_.erase(it);
    }
    else {
      ++it;
    }
  }

  for (const unique_ptr<InstanceArray> &array : arrays_) {
    if (inner.count(array.get()) || !subtree_modified(array.get())) {
      continue;
    }
    vector<Object *> &made = placed_[array.get()];

    /* Nested: one object for the whole chain, which the kernel places (ADR 0082 › E2). */
    if (nests && chain_nests(scene, array.get())) {
      if (made.size() > 1) {
        scene->delete_nodes(std::set<Object *>(made.begin() + 1, made.end()), scene);
        made.resize(1);
      }
      if (made.empty()) {
        made.push_back(scene->create_node<Object>());
      }
      Object *object = made[0];
      const vector<Level> no_levels;
      place(const_cast<Object *>(chain_prototype(array.get())), true, no_levels, object);
      if (object->instance_root != array.get()) {
        object->instance_root = array.get();
        object->tag_modified();
      }
      /* The arrays changed (the reason to be here): the object's instance tables and BVH. */
      object->tag_modified();
      object->tag_update(scene);
      continue;
    }

    /* Every element of an array places the same objects: one slot range per element, filled in parallel. */
    const size_t per_element = placements_per_element(array.get());
    const size_t count = array->size() * per_element;
    if (count < made.size()) {
      scene->delete_nodes(std::set<Object *>(made.begin() + count, made.end()), scene);
      made.resize(count);
    }
    while (made.size() < count) {
      made.push_back(scene->create_node<Object>());
    }
    const InstanceArray *root = array.get();
    parallel_for(size_t(0), root->size(), [&](const size_t element) {
      vector<Level> chain;
      size_t used = element * per_element;
      expand_element(root, element, chain, made, used);
    });
    for (Object *object : made) {
      if (object->instance_root) {
        object->instance_root = nullptr;
        object->tag_modified();
      }
      if (object->is_modified()) {
        object->tag_update(scene);
      }
    }
  }

  /* Everything is placed as it is now. */
  for (const unique_ptr<InstanceArray> &array : arrays_) {
    array->modified = false;
    if (array->object) {
      array->object->clear_modified();
    }
  }
  for (const unique_ptr<InstanceGroup> &group : groups_) {
    group->modified = false;
    for (const InstanceGroup::Member &member : group->objects) {
      member.object->clear_modified();
    }
  }
}

bool InstanceExpansion::device_nests(const Scene *scene)
{
  /* The kernel's __INSTANCE_NESTING__: the CPU with Embree (E2) or Cycles' own BVH, CUDA with Cycles' own BVH (E3).
   * Not OSL (it reads objects by index and name), not the motion pass. */
  if (!scene->device || scene->shader_manager->use_osl() ||
      scene->need_motion() == Scene::MOTION_PASS ||
      scene->need_motion() == Scene::MOTION_PASS_INTERACTIVE)
  {
    return false;
  }
  const DeviceType type = scene->device->info.type;
  const BVHLayout layout = BVHParams::best_bvh_layout(
      scene->params.bvh_layout, scene->device->get_bvh_layout_mask(scene->dscene.data.kernel_features));
  if (type == DEVICE_CUDA) {
    return layout == BVH_LAYOUT_BVH2;
  }
  if (type != DEVICE_CPU) {
    return false;
  }
#if defined(WITH_EMBREE) && RTC_MAX_INSTANCE_LEVEL_COUNT > 1
  return layout == BVH_LAYOUT_EMBREE || layout == BVH_LAYOUT_BVH2;
#else
  return layout == BVH_LAYOUT_BVH2;
#endif
}

const Object *InstanceExpansion::chain_prototype(const InstanceArray *root)
{
  const InstanceArray *array = root;
  for (size_t depth = 0; array && depth < kMaxDepth; depth++) {
    if (array->object) {
      return array->object;
    }
    const InstanceGroup *group = array->group;
    array = (group && group->objects.empty() && group->arrays.size() == 1) ? group->arrays[0] : nullptr;
  }
  return nullptr;
}

bool InstanceExpansion::chain_nests(const Scene *scene, const InstanceArray *root)
{
  /* A chain (each group holds one array: the delegate's per-prim chains) of up to the device's levels, without what
   * the first version flattens: motion on any level, and prototypes whose shading keys on one object index or
   * transform (volumes, emission, subsurface, bevel / AO, caustics, shadow links, baking). Per-element attributes
   * and instanceId are per-placement attributes (ATTR_ELEMENT_INSTANCE). */
#if defined(WITH_EMBREE) && RTC_MAX_INSTANCE_LEVEL_COUNT > 1
  const int max_levels = std::min(RTC_MAX_INSTANCE_LEVEL_COUNT, BVH_MAX_INSTANCE_LEVELS);
#else
  const int max_levels = BVH_MAX_INSTANCE_LEVELS;
#endif
  const Object *prototype = chain_prototype(root);
  if (!prototype || !prototype->get_geometry()) {
    return false;
  }
  int levels = 0;
  for (const InstanceArray *array = root; array;) {
    if (++levels > max_levels || array->steps.size() != 1) {
      return false;
    }
    array = array->object ? nullptr : array->group->arrays[0];
  }
  const Geometry *geom = prototype->get_geometry();
  if (!prototype->get_motion().empty() || geom->is_volume() || geom->is_light() ||
      geom->has_motion_blur() || prototype->get_is_bake_target() ||
      prototype->get_is_caustics_caster() || prototype->get_is_caustics_receiver() ||
      prototype->has_shadow_linking() || prototype->usable_as_light())
  {
    return false;
  }
  for (const Node *node : geom->get_used_shaders()) {
    const Shader *shader = static_cast<const Shader *>(node);
    if (shader->has_volume || shader->has_surface_bssrdf || shader->has_surface_raytrace) {
      return false;
    }
  }
  (void)scene;
  return true;
}

BoundBox InstanceExpansion::chain_bounds(const Object *object, vector<vector<BoundBox>> *element_bounds)
{
  vector<const InstanceArray *> chain;
  for (const InstanceArray *array = object->instance_root; array;
       array = array->object ? nullptr : array->group->arrays[0])
  {
    chain.push_back(array);
  }
  if (element_bounds) {
    element_bounds->assign(chain.size(), {});
  }
  /* Innermost first: each element's bounds are its child's (the geometry with the object's own transform, or the
   * level inside it) in its parent's space. */
  BoundBox child = object->get_geometry()->bounds;
  for (int level = int(chain.size()) - 1; level >= 0; level--) {
    const vector<Transform> &transforms = chain[level]->steps[0];
    const bool innermost = level == int(chain.size()) - 1;
    BoundBox all = BoundBox::empty;
    for (size_t e = 0; e < transforms.size(); e++) {
      const Transform tfm = innermost ? transforms[e] * object->get_tfm() : transforms[e];
      const BoundBox bounds = child.transformed(&tfm);
      all.grow(bounds);
      if (element_bounds) {
        (*element_bounds)[level].push_back(bounds);
      }
    }
    child = all;
  }
  return child;
}

void InstanceExpansion::device_update(DeviceScene *dscene, Scene *scene)
{
  const size_t num_objects = scene->objects.size();
  KernelObjectInstance *object_instance = dscene->object_instance.alloc(std::max(num_objects, size_t(1)));
  vector<KernelInstanceLevel> levels;
  vector<Transform> transforms;
  vector<float> colors;
  object_instance[0] = {-1, 0, 0, 0};
  vector<Transform> inverse_transforms;
  vector<KernelInstanceAttribute> attributes;
  vector<float> attribute_values;
  static const ustring instance_id("instanceId");
  for (Object *object : scene->objects) {
    KernelObjectInstance &placed = object_instance[object->index];
    placed = {-1, 0, 0, 0};
    const InstanceArray *root = object->instance_root;
    object->instance_levels_offset = root ? int(levels.size()) : -1;
    object->instance_attribute_refs.clear();
    if (!root) {
      continue;
    }

    /* Per-placement attributes: each level's primvars (an inner level's wins, as when expanded), and the instanceId
     * the prototype carries when shaders read it. */
    vector<const InstanceArray *> chain;
    for (const InstanceArray *array = root; array;
         array = array->object ? nullptr : array->group->arrays[0])
    {
      chain.push_back(array);
    }
    for (int level = int(chain.size()) - 1; level >= 0; level--) {
      for (const InstanceAttribute &attribute : chain[level]->attributes) {
        const bool known = std::any_of(object->instance_attribute_refs.begin(),
                                       object->instance_attribute_refs.end(),
                                       [&](const auto &ref) { return ref.name == attribute.name; });
        if (known || attribute.width < 1 || attribute.width > 4) {
          continue;
        }
        object->instance_attribute_refs.push_back({attribute.name, int(attributes.size()), attribute.width});
        attributes.push_back({level, attribute.width, int(attribute_values.size()), 0});
        attribute_values.insert(attribute_values.end(), attribute.values.begin(), attribute.values.end());
      }
    }
    for (const ParamValue &attribute : object->attributes) {
      if (attribute.name() == instance_id) {
        object->instance_attribute_refs.push_back({instance_id, int(attributes.size()), 1});
        attributes.push_back({-1, 1, 0, 0});
      }
    }
    placed.levels_offset = int(levels.size());
    placed.name_hash = hash_string(object->get_geometry()->name.c_str());
    const Object *prototype = chain_prototype(root);
    const bool keep_color = prototype && [&] {
      for (const InstanceArray *array = root; array; array = array->object ? nullptr : array->group->arrays[0]) {
        if (array->object) {
          return array->keep_object_color;
        }
      }
      return false;
    }();
    /* Outer first; each level's stride is the product of the inner levels' counts. */
    const size_t first = levels.size();
    for (const InstanceArray *array = root; array; array = array->object ? nullptr : array->group->arrays[0]) {
      KernelInstanceLevel level;
      level.count = uint(array->size());
      level.stride = 1;
      level.transform_offset = uint(transforms.size());
      level.color_offset = -1;
      transforms.insert(transforms.end(), array->steps[0].begin(), array->steps[0].end());
      /* Cycles' own BVH enters each element through its inverse, the innermost into the geometry's space. */
      for (const Transform &tfm : array->steps[0]) {
        inverse_transforms.push_back(
            transform_inverse(array->object ? tfm * object->get_tfm() : tfm));
      }
      if (!array->colors.empty() && !keep_color) {
        level.color_offset = int(colors.size());
        for (const float3 &color : array->colors) {
          colors.push_back(color.x);
          colors.push_back(color.y);
          colors.push_back(color.z);
        }
      }
      levels.push_back(level);
    }
    for (size_t l = levels.size() - 1; l > first; l--) {
      levels[l - 1].stride = levels[l].stride * std::max(levels[l].count, 1u);
    }
    placed.num_levels = int(levels.size() - first);
  }
  dscene->object_instance.copy_to_device();

  /* Never empty: the kernel's arrays must exist. */
  KernelInstanceLevel *klevels = dscene->instance_levels.alloc(std::max(levels.size(), size_t(1)));
  std::copy(levels.begin(), levels.end(), klevels);
  Transform *ktransforms = dscene->instance_transforms.alloc(std::max(transforms.size(), size_t(1)));
  std::copy(transforms.begin(), transforms.end(), ktransforms);
  float *kcolors = dscene->instance_colors.alloc(std::max(colors.size(), size_t(1)));
  std::copy(colors.begin(), colors.end(), kcolors);
  KernelInstanceAttribute *kattributes = dscene->instance_attributes.alloc(
      std::max(attributes.size(), size_t(1)));
  std::copy(attributes.begin(), attributes.end(), kattributes);
  dscene->instance_attributes.copy_to_device();
  float *kattribute_values = dscene->instance_attribute_values.alloc(
      std::max(attribute_values.size(), size_t(1)));
  std::copy(attribute_values.begin(), attribute_values.end(), kattribute_values);
  dscene->instance_attribute_values.copy_to_device();
  Transform *kinverse = dscene->instance_inverse_transforms.alloc(
      std::max(inverse_transforms.size(), size_t(1)));
  std::copy(inverse_transforms.begin(), inverse_transforms.end(), kinverse);
  dscene->instance_inverse_transforms.copy_to_device();
  dscene->instance_levels.copy_to_device();
  dscene->instance_transforms.copy_to_device();
  dscene->instance_colors.copy_to_device();
}

size_t InstanceExpansion::placements_per_element(const InstanceArray *array, const size_t depth)
{
  /* As expand_element: nothing deeper than kMaxDepth. */
  if (depth >= kMaxDepth) {
    return 0;
  }
  if (array->object) {
    return 1;
  }
  const InstanceGroup *group = array->group;
  if (!group) {
    return 0;
  }
  size_t count = group->objects.size();
  for (const InstanceArray *inner : group->arrays) {
    count += inner->size() * placements_per_element(inner, depth + 1);
  }
  return count;
}

void InstanceExpansion::expand_element(const InstanceArray *array,
                                       const size_t element,
                                       vector<Level> &chain,
                                       const vector<Object *> &made,
                                       size_t &used)
{
  if (chain.size() >= kMaxDepth) {
    return;
  }
  chain.push_back({array, element});
  if (array->object) {
    place(array->object, array->keep_object_color, chain, made[used++]);
  }
  else if (const InstanceGroup *group = array->group) {
    for (const InstanceGroup::Member &member : group->objects) {
      place(member.object, member.keep_object_color, chain, made[used++]);
    }
    for (const InstanceArray *inner : group->arrays) {
      for (size_t i = 0; i < inner->size(); i++) {
        expand_element(inner, i, chain, made, used);
      }
    }
  }
  chain.pop_back();
}

/* One placement; runs in parallel with the others (no scene changes here: the caller tags the modified ones). */
void InstanceExpansion::place(Object *prototype, const bool keep_color, const vector<Level> &chain, Object *object)
{

  /* The prototype's settings, geometry and shaders; what differs per placement is set below. */
  static const ustring tfm_name("tfm"), motion_name("motion"), color_name("color"), random_name("random_id");
  for (const SocketType &socket : Object::get_node_type()->inputs) {
    if (socket.name == tfm_name || socket.name == motion_name || socket.name == color_name ||
        socket.name == random_name)
    {
      continue;
    }
    if (!object->equals_value(*prototype, socket)) {
      object->copy_value(socket, *prototype, socket);
      object->tag_modified();
    }
  }
  object->name = prototype->name;

  /* Transforms: outer × inner × the prototype's own, at the centre and at each motion step. */
  Transform tfm = prototype->get_tfm();
  size_t num_steps = prototype->get_motion().size();
  for (auto level = chain.rbegin(); level != chain.rend(); ++level) {
    const size_t steps = level->array->steps.size();
    tfm = element_transform(level->array, steps / 2, level->element) * tfm;
    if (steps > 1) {
      num_steps = std::max(num_steps, steps);
    }
  }
  object->set_tfm(tfm);
  array<Transform> motion(num_steps > 1 ? num_steps : 0);
  for (size_t step = 0; step < motion.size(); step++) {
    const array<Transform> &own = prototype->get_motion();
    Transform composed = own.empty() ? prototype->get_tfm() : own[std::min(step, own.size() - 1)];
    for (auto level = chain.rbegin(); level != chain.rend(); ++level) {
      composed = element_transform(level->array, step, level->element) * composed;
    }
    motion[step] = composed;
  }
  object->set_motion(motion);

  /* The flattened index, outer-major (as an application multiplying the levels out orders them): random id and
   * instanceId as before. */
  uint flat = 0;
  for (const Level &level : chain) {
    flat = flat * uint(level.array->size()) + uint(level.element);
  }
  object->set_random_id(hash_uint2(hash_string(prototype->get_geometry()->name.c_str()), flat));

  /* Colour and attributes of the innermost level that has them. */
  float3 color = prototype->get_color();
  if (!keep_color) {
    for (auto level = chain.rbegin(); level != chain.rend(); ++level) {
      if (!level->array->colors.empty()) {
        color = level->array->colors[level->element];
        break;
      }
    }
  }
  object->set_color(color);

  static const ustring instance_id("instanceId");
  static const TypeDesc types[] = {TypeFloat, TypeFloat2, TypeVector, TypeFloat4};
  vector<ParamValue> attributes = prototype->attributes;
  for (ParamValue &attribute : attributes) {
    if (attribute.name() == instance_id) {
      const float value = float(flat);
      attribute = ParamValue(instance_id, TypeFloat, 1, &value);
    }
  }
  for (auto level = chain.rbegin(); level != chain.rend(); ++level) {
    for (const InstanceAttribute &attribute : level->array->attributes) {
      const bool known = std::any_of(attributes.begin(), attributes.end(), [&](const ParamValue &p) {
        return p.name() == attribute.name;
      });
      if (!known && attribute.width >= 1 && attribute.width <= 4) {
        attributes.emplace_back(attribute.name,
                                types[attribute.width - 1],
                                1,
                                &attribute.values[level->element * attribute.width]);
      }
    }
  }
  if (!same_attributes(attributes, object->attributes)) {
    object->attributes = std::move(attributes);
    object->tag_modified();
  }
}

CCL_NAMESPACE_END
