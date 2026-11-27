/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Nested instancing in Cycles' own BVH (USD Nodes ADR 0082 › E3). A nested object's top-level leaf leads to the BVH of
 * its outermost instance array level, in world space; each level's BVH has one element per leaf (an instance leaf
 * whose prim_object is the element); an element leads, through its inverse transform, to the next level's BVH and
 * the innermost ones to the object's geometry. The traversal keeps the ray of each level for the way back and the
 * placement's flattened index (Intersection::instance). */

CCL_NAMESPACE_BEGIN

#ifdef __INSTANCE_NESTING__
struct BVHNestedState {
  /* 0 outside a nested object; 1 in its outermost level's BVH; num_levels + 1 in its geometry. */
  int depth;
  int levels_offset;
  /* The flattened index of the placement so far, -1 outside a nested object. */
  int instance;
  /* At each depth, the ray and the index before entering it. */
  float3 P[BVH_MAX_INSTANCE_LEVELS + 1];
  float3 dir[BVH_MAX_INSTANCE_LEVELS + 1];
  float3 idir[BVH_MAX_INSTANCE_LEVELS + 1];
  int instance_before[BVH_MAX_INSTANCE_LEVELS + 1];
};

ccl_device_inline void bvh_nested_init(ccl_private BVHNestedState &nested)
{
  nested.depth = 0;
  nested.levels_offset = 0;
  nested.instance = -1;
}

/* An instance leaf at the top level: a nested object enters its outermost level's BVH (world space, no push). */
ccl_device_inline bool bvh_nested_enter(KernelGlobals kg,
                                        ccl_private BVHNestedState &nested,
                                        const int object,
                                        ccl_private int *node_addr)
{
  const KernelObjectInstance placed = kernel_data_fetch(object_instance, object);
  if (placed.num_levels == 0) {
    return false;
  }
  nested.depth = 1;
  nested.levels_offset = placed.levels_offset;
  nested.instance = 0;
  *node_addr = kernel_data_fetch(object_node, object);
  return true;
}

/* An instance leaf inside a nested object: its element, into the element's space. Returns the node inside it. */
ccl_device_inline int bvh_nested_push_element(KernelGlobals kg,
                                              ccl_private BVHNestedState &nested,
                                              const int leaf_prim,
                                              ccl_private float3 *P,
                                              ccl_private float3 *dir,
                                              ccl_private float3 *idir)
{
  const int index = nested.levels_offset + nested.depth - 1;
  const KernelInstanceLevel level = kernel_data_fetch(instance_levels, index);
  const int element = kernel_data_fetch(prim_object, leaf_prim);

  nested.P[nested.depth] = *P;
  nested.dir[nested.depth] = *dir;
  nested.idir[nested.depth] = *idir;
  nested.instance_before[nested.depth] = nested.instance;
  nested.instance += element * int(level.stride);

  const Transform itfm = kernel_data_fetch(instance_inverse_transforms,
                                           level.transform_offset + element);
  *P = transform_point(&itfm, *P);
  *dir = bvh_clamp_direction(transform_direction(&itfm, *dir));
  *idir = bvh_inverse_direction(*dir);
  nested.depth++;
  return kernel_data_fetch(instance_bvh_roots, index);
}

/* A sentinel inside a nested object: back to the level above. False when it leaves the object (the caller pops as
 * for any object). */
ccl_device_inline bool bvh_nested_pop(ccl_private BVHNestedState &nested,
                                      ccl_private float3 *P,
                                      ccl_private float3 *dir,
                                      ccl_private float3 *idir)
{
  if (nested.depth > 1) {
    nested.depth--;
    *P = nested.P[nested.depth];
    *dir = nested.dir[nested.depth];
    *idir = nested.idir[nested.depth];
    nested.instance = nested.instance_before[nested.depth];
    return true;
  }
  nested.depth = 0;
  nested.instance = -1;
  return false;
}
#  define BVH_NESTED_INSTANCE(nested) ((nested).instance)
#else
#  define BVH_NESTED_INSTANCE(nested) (-1)
#endif

CCL_NAMESPACE_END
