/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "session/output_driver.h"
#include "util/transform.h"
#include "util/types.h"

#include <string>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Names of the bake input passes (HdCyclesSession::SyncAovBindings). */
extern const char *const kBakePrimitivePass;
extern const char *const kBakeDifferentialPass;

/* Space of baked normals (Cycles' normal pass is world space). */
enum class BakeNormalSpace { World, Object, Tangent };

/* The bake target's triangles, three corners each, copied from the Cycles mesh while the scene
 * is locked: the output driver reads them while Cycles renders, without the scene lock (which
 * Hydra holds while it resets the session, and so waits for the render to stop).
 * shortcut: about 150 bytes per triangle; share the mesh data if large bake targets need it. */
struct BakeMesh {
  const CCL_NS::Object *object = nullptr;
  size_t numTriangles = 0;
  std::vector<CCL_NS::float2> uv;
  std::vector<CCL_NS::float3> P;
  std::vector<CCL_NS::float3> N;       /* Vertex normals, empty for flat meshes. */
  std::vector<CCL_NS::float3> tangent; /* MikkTSpace tangents and signs, when Cycles has them. */
  std::vector<float> sign;
  CCL_NS::Transform tfm = CCL_NS::transform_identity();
  bool transformApplied = false;

  bool valid() const
  {
    return numTriangles > 0;
  }
};

/* Texture baking like Blender's: the image is the UV space of the bake target mesh (the first
 * object of that prim), each pixel a point on the triangle that covers it in its default UV map.
 * Copies the target's data (empty when it has no triangles or UVs). Needs the scene lock. */
BakeMesh SnapshotBakeMesh(CCL_NS::Scene *scene, const std::string &object);

/* Writes Cycles' bake primitive and differential passes of a tile (all empty for an invalid
 * snapshot). */
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

/* Makes the first mesh object of the prim the bake target (none for an empty name). */
void SetBakeTarget(CCL_NS::Scene *scene, const std::string &object);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
