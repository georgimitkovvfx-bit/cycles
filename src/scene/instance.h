/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Nested instancing, the host step (USD Nodes ADR 0082, phase E1). An application describes its instances as a
 * tree: an InstanceArray places one child (a prototype object or an InstanceGroup) N times, a group places its
 * objects and arrays as a whole, so a forest is an array of tree groups, each a trunk object and an array of leaves.
 * Every device still renders one Object per final placement: InstanceExpansion makes them in the scene update, before
 * the managers run, so they need no change. The kernel reading the tree itself is the next phase. */

#include "scene/procedural.h"

#include "util/boundbox.h"
#include "util/map.h"
#include "util/param.h"
#include "util/transform.h"
#include "util/types.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

class DeviceScene;
class InstanceGroup;
class Object;

/* Per-element values of an object attribute (an instance primvar): `width` floats (1 to 4) per element. */
struct InstanceAttribute {
  ustring name;
  int width = 1;
  vector<float> values;
};

class InstanceArray {
 public:
  /* What each element places: a prototype (an Object outside scene->objects, whose settings, geometry and shaders
   * every placement copies) or a group. */
  Object *object = nullptr;
  InstanceGroup *group = nullptr;
  /* With an object: its own colour wins over the elements' (the prim has one). */
  bool keep_object_color = false;

  /* Element transforms from the child's space to the array's parent space, per motion step: steps[step][element].
   * One step when the elements do not move. */
  vector<vector<Transform>> steps;
  /* Per-element data, empty when the array does not author it; an inner array's wins over an outer one's. */
  vector<float3> colors;
  vector<InstanceAttribute> attributes;

  size_t size() const
  {
    return steps.empty() ? 0 : steps.front().size();
  }

  /* After changing anything above: the root arrays placing it are expanded again. */
  bool modified = true;
};

class InstanceGroup {
 public:
  struct Member {
    Object *object;
    bool keep_object_color;
  };
  vector<Member> objects;
  vector<InstanceArray *> arrays;

  bool modified = true;
};

class InstanceExpansion : public Procedural {
 public:
  NODE_DECLARE

  InstanceExpansion();
  ~InstanceExpansion() override;

  /* The scene's expansion, made on first use; find() makes none. */
  static InstanceExpansion *get(Scene *scene);
  static InstanceExpansion *find(Scene *scene);

  /* Statistics: the scene objects all arrays placed, how many of them are nested (the kernel places them), and the
   * placements those stand for. */
  size_t num_placements() const;
  size_t num_nested() const;
  size_t num_nested_placements() const;

  InstanceArray *create_array();
  InstanceGroup *create_group();
  /* Deletes the node; groups still holding it must drop it first. */
  void delete_array(Scene *scene, InstanceArray *array);
  void delete_group(Scene *scene, InstanceGroup *group);

  /* After changing an array, a group or a prototype: expand again in the next scene update. */
  void tag_update(Scene *scene);

  /* The scene objects made for an array placed at the top level (none for an array inside a group). */
  const vector<Object *> &placements(const InstanceArray *array) const;

  void generate(Scene *scene, Progress &progress) override;

  /* A nested object's bounds in world space (Object::compute_bounds); with `element_bounds`, also each level's elements'
   * in their parent's space, outer level first (Cycles' own BVH builds a BVH per level from them). */
  static BoundBox chain_bounds(const Object *object, vector<vector<BoundBox>> *element_bounds = nullptr);

  /* A nested object's placements: their number, and their object-to-world transforms (the frame) in flattened order
   * (the light tree's emitters of an emissive one). */
  static size_t num_placements(const Object *object);
  static void placement_transforms(const Object *object, vector<Transform> &transforms);

  /* How many of a nested volume object's placements overlap another of them (up to `limit`): the volume stack depth
   * they may need, as Scene::get_volume_stack_size counts the objects that intersect another volume. */
  static int num_overlapping_placements(const Object *object, int limit);

  /* The kernel's instance tables (object_instance, instance_levels, instance_transforms, instance_colors) for the
   * nested objects (Object::instance_root), every object's entry. Called by the object manager. */
  static void device_update(DeviceScene *dscene, Scene *scene);

 private:
  struct Level {
    const InstanceArray *array;
    size_t element;
  };
  /* The objects one element of an array places, through its subtree. */
  static size_t placements_per_element(const InstanceArray *array, size_t depth = 0);
  static void expand_element(const InstanceArray *array,
                             size_t element,
                             vector<Level> &chain,
                             const vector<Object *> &made,
                             size_t &used);
  static void place(Object *prototype, bool keep_color, const vector<Level> &chain, Object *object);
  bool subtree_modified(const InstanceArray *array) const;

  /* Nested instancing (USD Nodes ADR 0082 › E2): whether the device traces instance arrays itself, and whether a root
   * array's chain is one it can trace (else its placements are made as objects). */
  static bool device_nests(const Scene *scene);
  static bool chain_nests(const Scene *scene, const InstanceArray *root);
  static const Object *chain_prototype(const InstanceArray *root);

  vector<unique_ptr<InstanceArray>> arrays_;
  vector<unique_ptr<InstanceGroup>> groups_;
  /* Per array: the scene objects it placed, when it is a root. */
  unordered_map<const InstanceArray *, vector<Object *>> placed_;
  vector<Object *> no_placements_;
};

CCL_NAMESPACE_END
