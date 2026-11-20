/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "scene/shader_graph.h"

CCL_NAMESPACE_BEGIN

/* Dome Texture input node of world materials: the color of the dome light the material is the
 * world of (its texture times its color and intensity). Not a kernel node: the delegate replaces
 * it with a copy of the dome light's shader nodes (HdCyclesSession::ExpandDomeTextures). Black
 * in other materials. */
class DomeTextureNode : public ShaderNode {
 public:
  SHADER_NODE_CLASS(DomeTextureNode)
  void constant_fold(const ConstantFolder &folder) override;
};

CCL_NAMESPACE_END
