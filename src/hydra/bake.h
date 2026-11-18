/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "session/output_driver.h"

#include <string>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Names of the bake input passes (HdCyclesSession::SyncAovBindings). */
extern const char *const kBakePrimitivePass;
extern const char *const kBakeDifferentialPass;

/* Texture baking like Blender's: the image is the UV space of the bake target mesh (the first
 * object of that prim), each pixel a point on the triangle that covers it in its default UV map.
 * Writes Cycles' bake primitive and differential passes of a tile; false when the target has no
 * triangles or UVs. */
bool FillBakeTile(CCL_NS::Scene *scene,
                  const std::string &object,
                  const CCL_NS::OutputDriver::Tile &tile);

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

/* Baked normals (Cycles' normal pass is world space) in object space or tangent space (a normal
 * map: tangent, bitangent and normal mapped from -1..1 to 0..1, the tangent along the UV map's U
 * direction, the bitangent along V). Pixels outside the UV islands are left as they are. */
enum class BakeNormalSpace { World, Object, Tangent };
void ConvertBakeNormals(CCL_NS::Scene *scene,
                        const std::string &object,
                        const CCL_NS::OutputDriver::Tile &tile,
                        float *pixels,
                        int channels,
                        BakeNormalSpace space);

/* Makes the first mesh object of the prim the bake target (none for an empty name). */
void SetBakeTarget(CCL_NS::Scene *scene, const std::string &object);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
