/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "session/output_driver.h"
#include "util/transform.h"
#include "util/types.h"

#include <memory>
#include <string>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Names of the bake input passes (HdCyclesSession::SyncAovBindings). */
extern const char *const kBakePrimitivePass;
extern const char *const kBakeDifferentialPass;
extern const char *const kBakeObjectPass;

/* Space of baked normals (Cycles' normal pass is world space). */
enum class BakeNormalSpace { World, Object, Tangent };

/* Selected to Active: the objects baked from, ray cast from the target (bake.cpp). */
struct BakeSources;

/* The bake render settings that pick what is baked (cycles:bake_*). */
struct BakeSettings {
  /* Prim path of the bake target, empty when not baking. */
  std::string object;
  /* Instance of an instanced target: its instanceId (the instance index of its instancer). */
  int instance = 0;
  /* UV primvar of the target the image maps, empty for its default UV map. */
  std::string uvMap;
  /* Selected to Active like Blender's: bakes the surfaces of the source prims (paths, their
   * descendants, or USD collections "/prim.collection:name", separated by spaces) that rays
   * from the target hit, into the target's UV map. */
  bool selectedToActive = false;
  std::string sources;
  /* Rays start at the cage object (same triangles as the target) or at the target pushed out
   * along its normals by the extrusion, and go inwards up to the max ray distance (0: no limit). */
  std::string cage;
  float cageExtrusion = 0.0f;
  float maxRayDistance = 0.0f;

  bool useSources() const
  {
    return selectedToActive && !sources.empty();
  }
  bool operator==(const BakeSettings &other) const = default;
};

/* The bake target's triangles, three corners each, copied from the Cycles mesh while the scene
 * is locked: the output driver reads them while Cycles renders, without the scene lock (which
 * Hydra holds while it resets the session, and so waits for the render to stop).
 * shortcut: about 150 bytes per triangle; share the mesh data if large bake targets need it. */
struct BakeMesh {
  const CCL_NS::Object *object = nullptr;
  int objectIndex = 0; /* Device index of the object. */
  size_t numTriangles = 0;
  std::vector<CCL_NS::float2> uv;
  std::vector<CCL_NS::float3> P;
  std::vector<CCL_NS::float3> N;       /* Vertex normals, empty for flat meshes. */
  std::vector<CCL_NS::float3> tangent; /* MikkTSpace tangents and signs, when Cycles has them. */
  std::vector<float> sign;
  CCL_NS::Transform tfm = CCL_NS::transform_identity();
  bool transformApplied = false;
  /* Selected to Active, null otherwise. */
  std::shared_ptr<const BakeSources> sources;
  /* Device object and triangle counts when copied: a copy reused after the device scene changed
   * has other object indices (BakeMeshCurrent). */
  size_t deviceObjects = 0;
  size_t deviceTriangles = 0;

  bool valid() const
  {
    return numTriangles > 0;
  }
};

/* Texture baking like Blender's: the image is the UV space of the bake target mesh (the object of
 * that prim and instance), each pixel a point on the triangle that covers it in the bake UV map.
 * Copies the target's data (empty when it has no triangles or UVs), and with Selected to Active
 * builds a ray tracing BVH of the source prims (`sourcePrims`, the resolved bake sources), or
 * reuses `previousSources` when built from the same settings, prims and scene state version
 * (`sourcesVersion`). Needs the scene lock. */
BakeMesh SnapshotBakeMesh(CCL_NS::Scene *scene,
                          const BakeSettings &bake,
                          const std::vector<std::string> &sourcePrims,
                          unsigned int sourcesVersion,
                          const std::shared_ptr<const BakeSources> &previousSources);

/* Whether the object indices of the copy are those of the device scene rendered now. Reads the
 * device scene, which the session thread (the output driver's) updates. */
bool BakeMeshCurrent(const BakeMesh &mesh, const CCL_NS::Scene *scene);

/* Writes Cycles' bake primitive and differential passes of a tile (all empty for an invalid
 * snapshot), and with Selected to Active the bake object pass: the source surface each pixel's
 * ray hits. */
bool FillBakeTile(const BakeMesh &mesh, const CCL_NS::OutputDriver::Tile &tile);

/* Pixels of the tile inside the UV islands (1), from its bake primitive pass. Empty when the
 * tile has no bake passes. */
std::vector<unsigned char> BakeCoverage(const CCL_NS::OutputDriver::Tile &tile);

/* Blender's Extend bake margin: fills the pixels outside the UV islands up to `margin` pixels
 * away with the average of their filled neighbors, one ring per step. */
void ExtendBakeMargin(float *pixels,
                      int width,
                      int height,
                      int channels,
                      std::vector<unsigned char> coverage,
                      int margin);

/* Baked normals in object space or tangent space (a normal map: tangent, bitangent and normal
 * mapped from -1..1 to 0..1, the tangent along the UV map's U direction, the bitangent along
 * V). Pixels outside the UV islands are left as they are. */
void ConvertBakeNormals(const BakeMesh &mesh,
                        const CCL_NS::OutputDriver::Tile &tile,
                        float *pixels,
                        int channels,
                        BakeNormalSpace space);

/* Makes the mesh object of the prim and instance the bake target (none for an empty name). */
void SetBakeTarget(CCL_NS::Scene *scene, const BakeSettings &bake);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
