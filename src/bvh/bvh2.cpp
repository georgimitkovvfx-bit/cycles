/* SPDX-FileCopyrightText: 2009-2010 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted code from NVIDIA Corporation. */

#include <algorithm>

#include "bvh/bvh2.h"

#include "scene/hair.h"
#include "scene/instance.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/pointcloud.h"

#include "bvh/build.h"
#include "bvh/node.h"
#include "bvh/unaligned.h"

#include "util/progress.h"

CCL_NAMESPACE_BEGIN

BVHStackEntry::BVHStackEntry(const BVHNode *n, const int i) : node(n), idx(i) {}

int BVHStackEntry::encodeIdx() const
{
  return (node->is_leaf()) ? ~idx : idx;
}

BVH2::BVH2(const BVHParams &params_,
           const vector<Geometry *> &geometry_,
           const vector<Object *> &objects_)
    : BVH(params_, geometry_, objects_)
{
}

void BVH2::build(Progress &progress, Stats * /*unused*/)
{
  progress.set_substatus("Building BVH");

  /* build nodes */
  BVHBuild bvh_build(objects,
                     pack.prim_type,
                     pack.prim_index,
                     pack.prim_object,
                     pack.prim_time,
                     params,
                     progress);
  unique_ptr<BVHNode> bvh2_root = bvh_build.run();

  if (progress.get_cancel()) {
    return;
  }

  /* BVH builder returns tree in a binary mode (with two children per inner
   * node. Need to adopt that for a wider BVH implementations. */
  const unique_ptr<BVHNode> root = widen_children_nodes(std::move(bvh2_root));

  if (progress.get_cancel()) {
    return;
  }

  /* pack triangles */
  progress.set_substatus("Packing BVH triangles and strands");
  pack_primitives();

  if (progress.get_cancel()) {
    return;
  }

  /* pack nodes */
  progress.set_substatus("Packing BVH nodes");
  pack_nodes(root.get());
}

void BVH2::refit(Progress &progress)
{
  progress.set_substatus("Packing BVH primitives");
  pack_primitives();

  if (progress.get_cancel()) {
    return;
  }

  progress.set_substatus("Refitting BVH nodes");
  refit_nodes();
}

unique_ptr<BVHNode> BVH2::widen_children_nodes(unique_ptr<BVHNode> &&root)
{
  return std::move(root);
}

void BVH2::pack_leaf(const BVHStackEntry &e, const LeafNode *leaf)
{
  assert(e.idx + BVH_NODE_LEAF_SIZE <= pack.leaf_nodes.size());
  int4 data[BVH_NODE_LEAF_SIZE];
  std::fill_n(data, BVH_NODE_LEAF_SIZE, zero_int4());
  if (leaf->num_triangles() == 1 && pack.prim_index[leaf->lo] == -1) {
    /* object */
    data[0].x = ~(leaf->lo);
    data[0].y = 0;
  }
  else {
    /* triangle */
    data[0].x = leaf->lo;
    data[0].y = leaf->hi;
  }
  data[0].z = leaf->visibility;
  if (leaf->num_triangles() != 0) {
    data[0].w = pack.prim_type[leaf->lo];
  }

  std::copy_n(data, BVH_NODE_LEAF_SIZE, &pack.leaf_nodes[e.idx]);
}

void BVH2::pack_inner(const BVHStackEntry &e, const BVHStackEntry &e0, const BVHStackEntry &e1)
{
  if (e0.node->is_unaligned || e1.node->is_unaligned) {
    pack_unaligned_inner(e, e0, e1);
  }
  else {
    pack_aligned_inner(e, e0, e1);
  }
}

void BVH2::pack_aligned_inner(const BVHStackEntry &e,
                              const BVHStackEntry &e0,
                              const BVHStackEntry &e1)
{
  pack_aligned_node(e.idx,
                    e0.node->bounds,
                    e1.node->bounds,
                    e0.encodeIdx(),
                    e1.encodeIdx(),
                    e0.node->visibility,
                    e1.node->visibility);
}

void BVH2::pack_aligned_node(const int idx,
                             const BoundBox &b0,
                             const BoundBox &b1,
                             int c0,
                             int c1,
                             uint visibility0,
                             uint visibility1)
{
  assert(idx + BVH_NODE_SIZE <= pack.nodes.size());
  assert(c0 < 0 || c0 < pack.nodes.size());
  assert(c1 < 0 || c1 < pack.nodes.size());

  int4 data[BVH_NODE_SIZE] = {
      make_int4(visibility0 & ~PATH_RAY_VISIBILITY_NODE_UNALIGNED,
                visibility1 & ~PATH_RAY_VISIBILITY_NODE_UNALIGNED,
                c0,
                c1),
      make_int4(__float_as_int(b0.min.x),
                __float_as_int(b1.min.x),
                __float_as_int(b0.max.x),
                __float_as_int(b1.max.x)),
      make_int4(__float_as_int(b0.min.y),
                __float_as_int(b1.min.y),
                __float_as_int(b0.max.y),
                __float_as_int(b1.max.y)),
      make_int4(__float_as_int(b0.min.z),
                __float_as_int(b1.min.z),
                __float_as_int(b0.max.z),
                __float_as_int(b1.max.z)),
  };

  std::copy_n(data, BVH_NODE_SIZE, &pack.nodes[idx]);
}

void BVH2::pack_unaligned_inner(const BVHStackEntry &e,
                                const BVHStackEntry &e0,
                                const BVHStackEntry &e1)
{
  pack_unaligned_node(e.idx,
                      e0.node->get_aligned_space(),
                      e1.node->get_aligned_space(),
                      e0.node->bounds,
                      e1.node->bounds,
                      e0.encodeIdx(),
                      e1.encodeIdx(),
                      e0.node->visibility,
                      e1.node->visibility);
}

void BVH2::pack_unaligned_node(const int idx,
                               const Transform &aligned_space0,
                               const Transform &aligned_space1,
                               const BoundBox &b0,
                               const BoundBox &b1,
                               int c0,
                               int c1,
                               uint visibility0,
                               uint visibility1)
{
  assert(idx + BVH_UNALIGNED_NODE_SIZE <= pack.nodes.size());
  assert(c0 < 0 || c0 < pack.nodes.size());
  assert(c1 < 0 || c1 < pack.nodes.size());

  int4 data[BVH_UNALIGNED_NODE_SIZE];
  const Transform space0 = BVHUnaligned::compute_node_transform(b0, aligned_space0);
  const Transform space1 = BVHUnaligned::compute_node_transform(b1, aligned_space1);
  data[0] = make_int4(visibility0 | PATH_RAY_VISIBILITY_NODE_UNALIGNED,
                      visibility1 | PATH_RAY_VISIBILITY_NODE_UNALIGNED,
                      c0,
                      c1);

  data[1] = __float4_as_int4(space0.x);
  data[2] = __float4_as_int4(space0.y);
  data[3] = __float4_as_int4(space0.z);
  data[4] = __float4_as_int4(space1.x);
  data[5] = __float4_as_int4(space1.y);
  data[6] = __float4_as_int4(space1.z);

  std::copy_n(data, BVH_UNALIGNED_NODE_SIZE, &pack.nodes[idx]);
}

void BVH2::pack_nodes(const BVHNode *root)
{
  const size_t num_nodes = root->getSubtreeSize(BVH_STAT_NODE_COUNT);
  const size_t num_leaf_nodes = root->getSubtreeSize(BVH_STAT_LEAF_COUNT);
  assert(num_leaf_nodes <= num_nodes);
  const size_t num_inner_nodes = num_nodes - num_leaf_nodes;
  size_t node_size;
  if (params.use_unaligned_nodes) {
    const size_t num_unaligned_nodes = root->getSubtreeSize(BVH_STAT_UNALIGNED_INNER_COUNT);
    node_size = (num_unaligned_nodes * BVH_UNALIGNED_NODE_SIZE) +
                (num_inner_nodes - num_unaligned_nodes) * BVH_NODE_SIZE;
  }
  else {
    node_size = num_inner_nodes * BVH_NODE_SIZE;
  }
  /* Resize arrays */
  pack.nodes.clear();
  pack.leaf_nodes.clear();
  /* For top level BVH, first merge existing BVH's so we know the offsets. */
  if (params.top_level) {
    pack_instances(node_size, num_leaf_nodes * BVH_NODE_LEAF_SIZE);
  }
  else {
    pack.nodes.resize(node_size);
    pack.leaf_nodes.resize(num_leaf_nodes * BVH_NODE_LEAF_SIZE);
  }

  int nextNodeIdx = 0;
  int nextLeafNodeIdx = 0;

  vector<BVHStackEntry> stack;
  stack.reserve(BVHParams::MAX_DEPTH * 2);
  if (root->is_leaf()) {
    stack.push_back(BVHStackEntry(root, nextLeafNodeIdx++));
  }
  else {
    stack.push_back(BVHStackEntry(root, nextNodeIdx));
    nextNodeIdx += root->has_unaligned() ? BVH_UNALIGNED_NODE_SIZE : BVH_NODE_SIZE;
  }

  while (!stack.empty()) {
    const BVHStackEntry e = stack.back();
    stack.pop_back();

    if (e.node->is_leaf()) {
      /* leaf node */
      const LeafNode *leaf = reinterpret_cast<const LeafNode *>(e.node);
      pack_leaf(e, leaf);
    }
    else {
      /* inner node */
      int idx[2];
      for (int i = 0; i < 2; ++i) {
        if (e.node->get_child(i)->is_leaf()) {
          idx[i] = nextLeafNodeIdx++;
        }
        else {
          idx[i] = nextNodeIdx;
          nextNodeIdx += e.node->get_child(i)->has_unaligned() ? BVH_UNALIGNED_NODE_SIZE :
                                                                 BVH_NODE_SIZE;
        }
      }

      stack.push_back(BVHStackEntry(e.node->get_child(0), idx[0]));
      stack.push_back(BVHStackEntry(e.node->get_child(1), idx[1]));

      pack_inner(e, stack[stack.size() - 2], stack[stack.size() - 1]);
    }
  }
  assert(node_size == nextNodeIdx);
  /* root index to start traversal at, to handle case of single leaf node */
  pack.root_index = (root->is_leaf()) ? -1 : 0;
}

void BVH2::refit_nodes()
{
  assert(!params.top_level);

  BoundBox bbox = BoundBox::empty;
  uint visibility = 0;
  refit_node(0, (pack.root_index == -1) ? true : false, bbox, visibility);
}

void BVH2::refit_node(const int idx, bool leaf, BoundBox &bbox, uint &visibility)
{
  if (leaf) {
    /* refit leaf node */
    assert(idx + BVH_NODE_LEAF_SIZE <= pack.leaf_nodes.size());
    const int4 *data = &pack.leaf_nodes[idx];
    const int c0 = data[0].x;
    const int c1 = data[0].y;

    refit_primitives(c0, c1, bbox, visibility);

    /* TODO(sergey): De-duplicate with pack_leaf(). */
    int4 leaf_data[BVH_NODE_LEAF_SIZE];
    leaf_data[0].x = c0;
    leaf_data[0].y = c1;
    leaf_data[0].z = visibility;
    leaf_data[0].w = data[0].w;
    std::copy_n(leaf_data, BVH_NODE_LEAF_SIZE, &pack.leaf_nodes[idx]);
  }
  else {
    assert(idx + BVH_NODE_SIZE <= pack.nodes.size());

    const int4 *data = &pack.nodes[idx];
    const bool is_unaligned = (data[0].x & PATH_RAY_VISIBILITY_NODE_UNALIGNED) != 0;
    const int c0 = data[0].z;
    const int c1 = data[0].w;
    /* refit inner node, set bbox from children */
    BoundBox bbox0 = BoundBox::empty;
    BoundBox bbox1 = BoundBox::empty;
    uint visibility0 = 0;
    uint visibility1 = 0;

    refit_node((c0 < 0) ? -c0 - 1 : c0, (c0 < 0), bbox0, visibility0);
    refit_node((c1 < 0) ? -c1 - 1 : c1, (c1 < 0), bbox1, visibility1);

    if (is_unaligned) {
      const Transform aligned_space = transform_identity();
      pack_unaligned_node(
          idx, aligned_space, aligned_space, bbox0, bbox1, c0, c1, visibility0, visibility1);
    }
    else {
      pack_aligned_node(idx, bbox0, bbox1, c0, c1, visibility0, visibility1);
    }

    bbox.grow(bbox0);
    bbox.grow(bbox1);
    visibility = visibility0 | visibility1;
  }
}

/* Refitting */

void BVH2::refit_primitives(const int start, const int end, BoundBox &bbox, uint &visibility)
{
  /* Refit range of primitives. */
  for (int prim = start; prim < end; prim++) {
    const int pidx = pack.prim_index[prim];
    const int tob = pack.prim_object[prim];
    Object *ob = objects[tob];

    if (pidx == -1) {
      /* Object instance. */
      bbox.grow(ob->bounds);
    }
    else {
      /* Primitives. */
      if (pack.prim_type[prim] & PRIMITIVE_CURVE) {
        /* Curves. */
        const Hair *hair = static_cast<const Hair *>(ob->get_geometry());
        const int prim_offset = (params.top_level) ? hair->prim_offset : 0;
        const Hair::Curve curve = hair->get_curve(pidx - prim_offset);
        const int k = PRIMITIVE_UNPACK_SEGMENT(pack.prim_type[prim]);

        curve.bounds_grow(k, hair->get_position(), hair->get_radius(), bbox);

        /* Motion curves. */
        if (hair->get_use_motion_blur()) {
          const Attribute *attr_P = hair->attributes.find(ATTR_STD_POSITION);
          const Attribute *attr_R = hair->attributes.find(ATTR_STD_RADIUS);

          if (attr_P->has_motion()) {
            for (int attr_step = 1; attr_step < attr_P->num_motion_steps(); attr_step++) {
              curve.bounds_grow(
                  k, attr_P->data<packed_float3>(attr_step), attr_R->data<float>(attr_step), bbox);
            }
          }
        }
      }
      else if (pack.prim_type[prim] & PRIMITIVE_ANY_POINT) {
        /* Points. */
        const PointCloud *pointcloud = static_cast<const PointCloud *>(ob->get_geometry());
        const int prim_offset = (params.top_level) ? pointcloud->prim_offset : 0;
        const packed_float3 *points = pointcloud->get_position();
        const float *radius = pointcloud->get_radius();
        const PointCloud::Point point = pointcloud->get_point(pidx - prim_offset);

        point.bounds_grow(points, radius, bbox);

        /* Motion points. */
        if (pointcloud->get_use_motion_blur()) {
          const Attribute *attr_P = pointcloud->attributes.find(ATTR_STD_POSITION);

          if (attr_P->has_motion()) {
            const Attribute *attr_R = pointcloud->attributes.find(ATTR_STD_RADIUS);
            for (int attr_step = 1; attr_step < attr_P->num_motion_steps(); attr_step++) {
              const float3 P = attr_P->data<packed_float3>(attr_step)[point.index];
              const float r = attr_R->data<float>(attr_step)[point.index];
              bbox.grow(P, r);
            }
          }
        }
      }
      else {
        assert(pack.prim_type[prim] & PRIMITIVE_TRIANGLE);

        /* Triangles. */
        const Mesh *mesh = static_cast<const Mesh *>(ob->get_geometry());
        const int prim_offset = (params.top_level) ? mesh->prim_offset : 0;
        const Mesh::Triangle triangle = mesh->get_triangle(pidx - prim_offset);
        const packed_float3 *vpos = mesh->get_position();

        triangle.bounds_grow(vpos, bbox);

        /* Motion triangles. */
        if (mesh->use_motion_blur) {
          const Attribute *attr_P = mesh->attributes.find(ATTR_STD_POSITION);

          if (attr_P->has_motion()) {
            for (int attr_step = 1; attr_step < attr_P->num_motion_steps(); attr_step++) {
              triangle.bounds_grow(attr_P->data<packed_float3>(attr_step), bbox);
            }
          }
        }
      }
    }
    visibility |= ob->visibility_for_tracing();
  }
}

/* Triangles */

void BVH2::pack_primitives()
{
  const size_t tidx_size = pack.prim_index.size();
  /* Reserve size for arrays. */
  pack.prim_visibility.clear();
  pack.prim_visibility.resize(tidx_size);
  /* Fill in all the arrays. */
  for (unsigned int i = 0; i < tidx_size; i++) {
    if (pack.prim_index[i] != -1) {
      const int tob = pack.prim_object[i];
      Object *ob = objects[tob];
      pack.prim_visibility[i] = ob->visibility_for_tracing();
    }
    else {
      pack.prim_visibility[i] = 0;
    }
  }
}

/* Pack Instances */

void BVH2::pack_instances(size_t nodes_size, size_t leaf_nodes_size)
{
  /* Adjust primitive index to point to the triangle in the global array, for
   * geometry with transform applied and already in the top level BVH.
   */
  for (size_t i = 0; i < pack.prim_index.size(); i++) {
    if (pack.prim_index[i] != -1) {
      pack.prim_index[i] += objects[pack.prim_object[i]]->get_geometry()->prim_offset;
    }
  }

  /* track offsets of instanced BVH data in global array */
  size_t prim_offset = pack.prim_index.size();
  size_t nodes_offset = nodes_size;
  size_t nodes_leaf_offset = leaf_nodes_size;

  /* clear array that gives the node indexes for instanced objects */
  pack.object_node.clear();

  /* reserve */
  size_t prim_index_size = pack.prim_index.size();

  size_t pack_prim_index_offset = prim_index_size;
  size_t pack_nodes_offset = nodes_size;
  size_t pack_leaf_nodes_offset = leaf_nodes_size;
  size_t object_offset = 0;

  for (Geometry *geom : geometry) {
    BVH2 *bvh = static_cast<BVH2 *>(geom->bvh.get());

    if (geom->need_build_bvh(params.bvh_layout)) {
      prim_index_size += bvh->pack.prim_index.size();
      nodes_size += bvh->pack.nodes.size();
      leaf_nodes_size += bvh->pack.leaf_nodes.size();
    }
  }

  pack.prim_index.resize(prim_index_size);
  pack.prim_type.resize(prim_index_size);
  pack.prim_object.resize(prim_index_size);
  pack.prim_visibility.resize(prim_index_size);
  pack.nodes.resize(nodes_size);
  pack.leaf_nodes.resize(leaf_nodes_size);
  pack.object_node.resize(objects.size());

  if (params.num_motion_curve_steps > 0 || params.num_motion_triangle_steps > 0 ||
      params.num_motion_point_steps > 0)
  {
    pack.prim_time.resize(prim_index_size);
  }

  int *pack_prim_index = (pack.prim_index.size()) ? pack.prim_index.data() : nullptr;
  int *pack_prim_type = (pack.prim_type.size()) ? pack.prim_type.data() : nullptr;
  int *pack_prim_object = (pack.prim_object.size()) ? pack.prim_object.data() : nullptr;
  uint *pack_prim_visibility = (pack.prim_visibility.size()) ? pack.prim_visibility.data() :
                                                               nullptr;
  int4 *pack_nodes = (pack.nodes.size()) ? pack.nodes.data() : nullptr;
  int4 *pack_leaf_nodes = (pack.leaf_nodes.size()) ? pack.leaf_nodes.data() : nullptr;
  float2 *pack_prim_time = (pack.prim_time.size()) ? pack.prim_time.data() : nullptr;

  unordered_map<Geometry *, int> geometry_map;

  /* merge */
  for (Object *ob : objects) {
    Geometry *geom = ob->get_geometry();

    /* We assume that if mesh doesn't need own BVH it was already included
     * into a top-level BVH and no packing here is needed.
     */
    if (!geom->need_build_bvh(params.bvh_layout)) {
      pack.object_node[object_offset++] = 0;
      continue;
    }

    /* if mesh already added once, don't add it again, but used set
     * node offset for this object */
    const unordered_map<Geometry *, int>::iterator it = geometry_map.find(geom);

    if (geometry_map.contains(geom)) {
      const int noffset = it->second;
      pack.object_node[object_offset++] = noffset;
      continue;
    }

    BVH2 *bvh = static_cast<BVH2 *>(geom->bvh.get());

    const int noffset = nodes_offset;
    const int noffset_leaf = nodes_leaf_offset;
    const int geom_prim_offset = geom->prim_offset;

    /* fill in node indexes for instances */
    if (bvh->pack.root_index == -1) {
      pack.object_node[object_offset++] = -noffset_leaf - 1;
    }
    else {
      pack.object_node[object_offset++] = noffset;
    }

    geometry_map[geom] = pack.object_node[object_offset - 1];

    /* merge primitive, object and triangle indexes */
    if (bvh->pack.prim_index.size()) {
      const size_t bvh_prim_index_size = bvh->pack.prim_index.size();
      int *bvh_prim_index = bvh->pack.prim_index.data();
      int *bvh_prim_type = bvh->pack.prim_type.data();
      uint *bvh_prim_visibility = bvh->pack.prim_visibility.data();
      float2 *bvh_prim_time = bvh->pack.prim_time.size() ? bvh->pack.prim_time.data() : nullptr;

      for (size_t i = 0; i < bvh_prim_index_size; i++) {
        pack_prim_index[pack_prim_index_offset] = bvh_prim_index[i] + geom_prim_offset;
        pack_prim_type[pack_prim_index_offset] = bvh_prim_type[i];
        pack_prim_visibility[pack_prim_index_offset] = bvh_prim_visibility[i];
        pack_prim_object[pack_prim_index_offset] = 0;  // unused for instances
        if (bvh_prim_time != nullptr) {
          pack_prim_time[pack_prim_index_offset] = bvh_prim_time[i];
        }
        pack_prim_index_offset++;
      }
    }

    /* merge nodes */
    if (bvh->pack.leaf_nodes.size()) {
      int4 *leaf_nodes_offset = bvh->pack.leaf_nodes.data();
      const size_t leaf_nodes_offset_size = bvh->pack.leaf_nodes.size();
      for (size_t i = 0; i < leaf_nodes_offset_size; i += BVH_NODE_LEAF_SIZE) {
        int4 data = leaf_nodes_offset[i];
        data.x += prim_offset;
        data.y += prim_offset;
        pack_leaf_nodes[pack_leaf_nodes_offset] = data;
        for (int j = 1; j < BVH_NODE_LEAF_SIZE; ++j) {
          pack_leaf_nodes[pack_leaf_nodes_offset + j] = leaf_nodes_offset[i + j];
        }
        pack_leaf_nodes_offset += BVH_NODE_LEAF_SIZE;
      }
    }

    if (bvh->pack.nodes.size()) {
      int4 *bvh_nodes = bvh->pack.nodes.data();
      const size_t bvh_nodes_size = bvh->pack.nodes.size();

      for (size_t i = 0; i < bvh_nodes_size;) {
        size_t nsize;
        size_t nsize_bbox;
        if (bvh_nodes[i].x & PATH_RAY_VISIBILITY_NODE_UNALIGNED) {
          nsize = BVH_UNALIGNED_NODE_SIZE;
          nsize_bbox = 0;
        }
        else {
          nsize = BVH_NODE_SIZE;
          nsize_bbox = 0;
        }

        std::copy_n(bvh_nodes + i, nsize_bbox, pack_nodes + pack_nodes_offset);

        /* Modify offsets into arrays */
        int4 data = bvh_nodes[i + nsize_bbox];
        data.z += (data.z < 0) ? -noffset_leaf : noffset;
        data.w += (data.w < 0) ? -noffset_leaf : noffset;
        pack_nodes[pack_nodes_offset + nsize_bbox] = data;

        /* Usually this copies nothing, but we better
         * be prepared for possible node size extension.
         */
        std::copy_n(&bvh_nodes[i + nsize_bbox + 1],
                    (nsize - (nsize_bbox + 1)),
                    &pack_nodes[pack_nodes_offset + nsize_bbox + 1]);

        pack_nodes_offset += nsize;
        i += nsize;
      }
    }

    nodes_offset += bvh->pack.nodes.size();
    nodes_leaf_offset += bvh->pack.leaf_nodes.size();
    prim_offset += bvh->pack.prim_index.size();
  }

  /* Nested objects lead to their levels' BVHs, the innermost to the geometry merged above. */
  pack.instance_bvh_roots.clear();
  for (size_t i = 0; i < objects.size(); i++) {
    if (objects[i]->instance_root && objects[i]->instance_levels_offset >= 0) {
      pack_nested_levels(objects[i], int(i));
    }
  }
}

namespace {

/* A BVH over one level's elements, one per leaf, split at the median of the longest axis of their centres. `order`
 * holds element indices; a leaf's prim range is its position in `order`. */
unique_ptr<BVHNode> build_level_node(const vector<BoundBox> &bounds,
                                     vector<int> &order,
                                     const int begin,
                                     const int end,
                                     const uint visibility)
{
  BoundBox box = BoundBox::empty;
  BoundBox centres = BoundBox::empty;
  for (int i = begin; i < end; i++) {
    box.grow(bounds[order[i]]);
    centres.grow(bounds[order[i]].center());
  }
  if (end - begin == 1) {
    return make_unique<LeafNode>(box, visibility, begin, end);
  }
  const float3 size = centres.size();
  const int axis = (size.x >= size.y && size.x >= size.z) ? 0 : (size.y >= size.z ? 1 : 2);
  const int mid = (begin + end) / 2;
  std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end, [&](int a, int b) {
    return bounds[a].center()[axis] < bounds[b].center()[axis];
  });
  unique_ptr<BVHNode> left = build_level_node(bounds, order, begin, mid, visibility);
  unique_ptr<BVHNode> right = build_level_node(bounds, order, mid, end, visibility);
  return make_unique<InnerNode>(box, std::move(left), std::move(right));
}

}  // namespace

int BVH2::append_packed(const PackedBVH &other)
{
  const int prim_offset = int(pack.prim_index.size());
  const int nodes_offset = int(pack.nodes.size());
  const int leaf_offset = int(pack.leaf_nodes.size());

  const auto append = [](auto &to, const auto &from) {
    const size_t start = to.size();
    to.resize(start + from.size());
    std::copy_n(from.data(), from.size(), to.data() + start);
  };
  append(pack.prim_index, other.prim_index);
  append(pack.prim_type, other.prim_type);
  append(pack.prim_object, other.prim_object);
  append(pack.prim_visibility, other.prim_visibility);
  if (pack.prim_time.size()) {
    const size_t start = pack.prim_time.size();
    pack.prim_time.resize(start + other.prim_index.size());
    std::fill_n(pack.prim_time.data() + start, other.prim_index.size(), make_float2(0.0f, 1.0f));
  }

  /* Leaves: instance leaves (~prim) and primitive ranges move with the primitives. */
  const size_t leaf_start = pack.leaf_nodes.size();
  append(pack.leaf_nodes, other.leaf_nodes);
  for (size_t i = leaf_start; i < pack.leaf_nodes.size(); i += BVH_NODE_LEAF_SIZE) {
    int4 &data = pack.leaf_nodes[i];
    if (data.x < 0) {
      data.x = ~(~data.x + prim_offset);
    }
    else {
      data.x += prim_offset;
      data.y += prim_offset;
    }
  }
  /* Inner nodes (aligned only: these BVHs have no curves): child indices move with the nodes. */
  const size_t node_start = pack.nodes.size();
  append(pack.nodes, other.nodes);
  for (size_t i = node_start; i < pack.nodes.size(); i += BVH_NODE_SIZE) {
    int4 &data = pack.nodes[i];
    data.z += (data.z < 0) ? -leaf_offset : nodes_offset;
    data.w += (data.w < 0) ? -leaf_offset : nodes_offset;
  }
  return other.root_index == -1 ? -leaf_offset - 1 : nodes_offset;
}

void BVH2::pack_nested_levels(Object *ob, const int object_index)
{
  vector<vector<BoundBox>> element_bounds;
  InstanceExpansion::chain_bounds(ob, &element_bounds);
  const int num_levels = int(element_bounds.size());
  const int offset = ob->instance_levels_offset;
  if (pack.instance_bvh_roots.size() < size_t(offset + num_levels)) {
    const size_t old_size = pack.instance_bvh_roots.size();
    pack.instance_bvh_roots.resize(offset + num_levels);
    std::fill_n(pack.instance_bvh_roots.data() + old_size, offset + num_levels - old_size, 0);
  }
  const uint visibility = ob->visibility_for_tracing();

  /* Innermost first: each level's elements lead to the BVH of the level inside them. */
  int child_root = pack.object_node[object_index]; /* the geometry, merged by pack_instances */
  for (int level = num_levels - 1; level >= 0; level--) {
    const vector<BoundBox> &bounds = element_bounds[level];
    pack.instance_bvh_roots[offset + level] = child_root;
    if (bounds.empty()) {
      child_root = 0;
      continue;
    }
    vector<int> order(bounds.size());
    for (size_t e = 0; e < order.size(); e++) {
      order[e] = int(e);
    }
    unique_ptr<BVHNode> root = build_level_node(bounds, order, 0, int(order.size()), visibility);

    BVHParams level_params = params;
    level_params.top_level = false;
    level_params.use_unaligned_nodes = false;
    BVH2 level_bvh(level_params, {}, {});
    PackedBVH &level_pack = level_bvh.pack;
    level_pack.prim_index.resize(order.size());
    level_pack.prim_type.resize(order.size());
    level_pack.prim_object.resize(order.size());
    level_pack.prim_visibility.resize(order.size());
    for (size_t i = 0; i < order.size(); i++) {
      level_pack.prim_index[i] = -1;   /* an instance leaf */
      level_pack.prim_type[i] = 0;
      level_pack.prim_object[i] = order[i]; /* the element */
      level_pack.prim_visibility[i] = visibility;
    }
    level_bvh.pack_nodes(root.get());
    child_root = append_packed(level_pack);
  }
  pack.object_node[object_index] = child_root;
}

CCL_NAMESPACE_END
