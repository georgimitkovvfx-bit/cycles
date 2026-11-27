/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "scene/instance.h"
#include "scene/geometry.h"
#include "scene/object.h"
#include "scene/scene.h"

#include "util/hash.h"
#include "util/set.h"
#include "util/progress.h"
#include "util/tbb.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

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
    /* Every element of an array places the same objects: one slot range per element, filled in parallel. */
    vector<Object *> &made = placed_[array.get()];
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
