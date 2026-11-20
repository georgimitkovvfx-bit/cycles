/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "scene/shader_graph.h"

CCL_NAMESPACE_BEGIN

/* Parallax Occlusion Mapping: the texture coordinates where the view ray hits a height field
 * below the surface. Not a kernel node: expand_parallax_occlusion() replaces it with standard
 * nodes before the graph is compiled, so it works with SVM, OSL and all devices. */
class ParallaxOcclusionNode : public ShaderNode {
 public:
  SHADER_NODE_CLASS(ParallaxOcclusionNode)

  NODE_SOCKET_API(float, height)
  NODE_SOCKET_API(float3, vector)
  NODE_SOCKET_API(float, depth)
  NODE_SOCKET_API(int, steps)
  NODE_SOCKET_API(ustring, uv_map)
};

/* Replace every Parallax Occlusion node of the graph by a linear search through Steps copies of
 * the nodes linked to its Height input, each reading the texture coordinates of one depth layer,
 * and a linear refinement between the last two layers. */
void expand_parallax_occlusion(ShaderGraph *graph);

CCL_NAMESPACE_END
