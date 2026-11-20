/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/dome_texture.h"

#include "scene/constant_fold.h"

CCL_NAMESPACE_BEGIN

NODE_DEFINE(DomeTextureNode)
{
  NodeType *type = NodeType::add("dome_texture", create, NodeType::SHADER);

  SOCKET_OUT_COLOR(color, "Color");

  return type;
}

DomeTextureNode::DomeTextureNode() : ShaderNode(get_node_type()) {}

void DomeTextureNode::constant_fold(const ConstantFolder &folder)
{
  folder.make_zero(); /* Not the world of a dome light. */
}

/* Never compiled: replaced in world materials, folded to black in others. */
void DomeTextureNode::compile(SVMCompiler & /*compiler*/) {}

void DomeTextureNode::compile(OSLCompiler & /*compiler*/) {}

CCL_NAMESPACE_END
