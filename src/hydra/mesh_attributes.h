/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Mesh attributes that Blender's sync computes on the host (intern/cycles/blender/mesh.cpp):
 * Geometry > Pointiness and Random Per Island. Ported for Hydra polygon meshes, only depends on
 * Cycles utilities so it can be checked outside the delegate. */

#include "util/disjoint_set.h"
#include "util/hash.h"
#include "util/math.h"
#include "util/types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <vector>

CCL_NAMESPACE_BEGIN

/* Polygons as face vertex counts and the vertex index of each corner (USD topology). */
struct PolygonMesh {
  const packed_float3 *positions;
  int num_verts;
  const int *face_counts;
  size_t num_faces;
  const int *face_verts;
  /* Faces wound clockwise (USD leftHanded), the normals point the other way. */
  bool flip;
};

/* Unique edges of the polygons, with the vertices mapped through vert_map. */
inline std::vector<std::pair<int, int>> polygon_mesh_edges(const PolygonMesh &mesh,
                                                            const std::vector<int> &vert_map)
{
  std::vector<std::pair<int, int>> edges;
  std::unordered_set<uint64_t> visited;
  for (size_t face = 0, start = 0; face < mesh.num_faces; start += mesh.face_counts[face++]) {
    const int count = mesh.face_counts[face];
    for (int i = 0; i < count; ++i) {
      const int v0 = vert_map[mesh.face_verts[start + i]];
      const int v1 = vert_map[mesh.face_verts[start + (i + 1) % count]];
      /* Welded away, a zero length edge has no direction. */
      if (v0 == v1) {
        continue;
      }
      const uint64_t key = (uint64_t(std::min(v0, v1)) << 32) | uint32_t(std::max(v0, v1));
      if (visited.insert(key).second) {
        edges.emplace_back(v0, v1);
      }
    }
  }
  return edges;
}

/* Vertex normals weighted by corner angle (Blender's), polygon normals by Newell's method. */
inline std::vector<float3> polygon_mesh_vertex_normals(const PolygonMesh &mesh)
{
  std::vector<float3> normals(mesh.num_verts, zero_float3());
  for (size_t face = 0, start = 0; face < mesh.num_faces; start += mesh.face_counts[face++]) {
    const int count = mesh.face_counts[face];
    const int *verts = mesh.face_verts + start;
    const auto co = [&](const int i) -> float3 { return mesh.positions[verts[i % count]]; };
    float3 face_normal = zero_float3();
    for (int i = 0; i < count; ++i) {
      face_normal += cross(co(i), co(i + 1));
    }
    face_normal = safe_normalize(mesh.flip ? -face_normal : face_normal);
    for (int i = 0; i < count; ++i) {
      const float3 prev = safe_normalize(co(i + count - 1) - co(i));
      const float3 next = safe_normalize(co(i + 1) - co(i));
      normals[verts[i]] += face_normal * safe_acosf(dot(prev, next));
    }
  }
  return normals;
}

/* Blender's weld of attr_create_pointiness: each vertex points to the first vertex after it, in
 * the order of the sum of the coordinates, whose sum is at most 3 * FLT_EPSILON larger and whose
 * squared distance is below FLT_EPSILON, then to the end of that chain. Blender scans the sorted
 * vertices, which is quadratic when many have equal sums (flat grids): the candidates come from a
 * grid of the positions here instead, with the same result. */
inline std::vector<int> polygon_mesh_weld(const PolygonMesh &mesh)
{
  const int num_verts = mesh.num_verts;
  const auto P = [&](const int v) -> float3 { return mesh.positions[v]; };
  const auto sum = [](const float3 co) { return co.x + co.y + co.z; };

  std::vector<int> sorted(num_verts);
  std::iota(sorted.begin(), sorted.end(), 0);
  std::sort(sorted.begin(), sorted.end(), [&](const int a, const int b) {
    const float3 va = P(a);
    const float3 vb = P(b);
    if (va.x == vb.x && va.y == vb.y && va.z == vb.z) {
      return a > b; /* Doubles in a fixed order. */
    }
    return sum(va) < sum(vb);
  });
  std::vector<int> rank(num_verts);
  for (int s = 0; s < num_verts; ++s) {
    rank[sorted[s]] = s;
  }

  /* Cells twice the weld distance (padded for rounding), so a vertex's neighbors are in at most
   * 2 cells per axis. Non-finite positions never weld. */
  const double radius = std::sqrt(double(FLT_EPSILON)) * 1.01;
  const double cell_size = 2.0 * radius;
  const auto cell = [&](const float x, const double offset) {
    /* Clamped: far away cells share one, which only adds candidates. */
    return int64_t(std::clamp(std::floor((double(x) + offset) / cell_size), -4e18, 4e18));
  };
  const auto cell_key = [](const int64_t x, const int64_t y, const int64_t z) {
    return (uint64_t(x) * 73856093u) ^ (uint64_t(y) * 19349663u) ^ (uint64_t(z) * 83492791u);
  };
  const auto finite = [](const float3 co) {
    return std::isfinite(co.x) && std::isfinite(co.y) && std::isfinite(co.z);
  };
  /* Vertices of a cell (key collisions only add candidates) as linked lists. */
  std::unordered_map<uint64_t, int> head;
  head.reserve(num_verts);
  std::vector<int> next(num_verts, -1);
  for (int v = 0; v < num_verts; ++v) {
    const float3 co = P(v);
    if (finite(co)) {
      const auto [it, inserted] = head.try_emplace(
          cell_key(cell(co.x, 0.0), cell(co.y, 0.0), cell(co.z, 0.0)), v);
      if (!inserted) {
        next[v] = it->second;
        it->second = v;
      }
    }
  }

  std::vector<int> orig(num_verts);
  for (int v = 0; v < num_verts; ++v) {
    orig[v] = v;
    const float3 co = P(v);
    if (!finite(co)) {
      continue;
    }
    int best = num_verts;
    for (int64_t x = cell(co.x, -radius); x <= cell(co.x, radius); ++x) {
      for (int64_t y = cell(co.y, -radius); y <= cell(co.y, radius); ++y) {
        for (int64_t z = cell(co.z, -radius); z <= cell(co.z, radius); ++z) {
          const auto it = head.find(cell_key(x, y, z));
          if (it == head.end()) {
            continue;
          }
          for (int other = it->second; other != -1; other = next[other]) {
            const float3 other_co = P(other);
            if (rank[other] > rank[v] && rank[other] < best &&
                sum(other_co) - sum(co) <= 3 * FLT_EPSILON &&
                len_squared(other_co - co) < FLT_EPSILON)
            {
              best = rank[other];
            }
          }
        }
      }
    }
    if (best < num_verts) {
      orig[v] = sorted[best];
    }
  }
  for (int v = 0; v < num_verts; ++v) {
    int o = orig[v];
    while (o != orig[o]) {
      o = orig[o];
    }
    orig[v] = o;
  }
  return orig;
}

/* Blender's attr_create_pointiness: per vertex, the angle between the vertex normal and the mean
 * direction of its edges (0.5 flat, above convex, below concave), blurred once over the edges.
 * Vertices at the same position are welded, so split seams don't show. */
inline std::vector<float> compute_pointiness(const PolygonMesh &mesh)
{
  const int num_verts = mesh.num_verts;
  const auto P = [&](const int v) -> float3 { return mesh.positions[v]; };
  std::vector<float> data(num_verts, 0.0f);
  if (num_verts == 0) {
    return data;
  }

  /* Step 1: point duplicated vertices to a single original vertex. */
  const std::vector<int> orig = polygon_mesh_weld(mesh);

  /* Step 2: vertex normals of the welded vertices. */
  const std::vector<float3> vert_normals = polygon_mesh_vertex_normals(mesh);
  std::vector<float3> normal(num_verts, zero_float3());
  for (int v = 0; v < num_verts; ++v) {
    /* Blender's vertex normals are unit length. */
    normal[orig[v]] += safe_normalize(vert_normals[v]);
  }

  /* Step 3: pointiness of the one ring neighborhood. */
  const std::vector<std::pair<int, int>> edges = polygon_mesh_edges(mesh, orig);
  std::vector<int> counter(num_verts, 0);
  std::vector<float3> edge_accum(num_verts, zero_float3());
  for (const auto &[v0, v1] : edges) {
    const float3 edge = safe_normalize(P(v1) - P(v0));
    edge_accum[v0] += edge;
    edge_accum[v1] -= edge;
    ++counter[v0];
    ++counter[v1];
  }
  std::vector<float> raw(num_verts, 0.0f);
  for (int v = 0; v < num_verts; ++v) {
    if (orig[v] == v && counter[v] > 0) {
      raw[v] = safe_acosf(dot(safe_normalize(normal[v]), edge_accum[v] / float(counter[v]))) *
               M_1_PI_F;
    }
  }

  /* Step 4: blur to approximate the two ring neighborhood. */
  data = raw;
  std::fill(counter.begin(), counter.end(), 0);
  for (const auto &[v0, v1] : edges) {
    data[v0] += raw[v1];
    data[v1] += raw[v0];
    ++counter[v0];
    ++counter[v1];
  }
  for (int v = 0; v < num_verts; ++v) {
    data[v] /= float(counter[v] + 1);
  }

  /* Step 5: copy to the duplicated vertices. */
  for (int v = 0; v < num_verts; ++v) {
    data[v] = data[orig[v]];
  }
  return data;
}

/* Blender's attr_create_random_per_island: a random value per face, the same for all faces
 * connected by edges (vertex indices, not welded by position). */
inline std::vector<float> compute_random_per_island(const PolygonMesh &mesh)
{
  std::vector<float> data(mesh.num_faces, 0.0f);
  if (mesh.num_verts == 0) {
    return data;
  }
  DisjointSet islands(mesh.num_verts);
  for (size_t face = 0, start = 0; face < mesh.num_faces; start += mesh.face_counts[face++]) {
    const int count = mesh.face_counts[face];
    for (int i = 0; i < count; ++i) {
      islands.join(mesh.face_verts[start + i], mesh.face_verts[start + (i + 1) % count]);
    }
  }
  for (size_t face = 0, start = 0; face < mesh.num_faces; start += mesh.face_counts[face++]) {
    if (mesh.face_counts[face] > 0) {
      data[face] = hash_uint_to_float(uint(islands.find(mesh.face_verts[start])));
    }
  }
  return data;
}

CCL_NAMESPACE_END
