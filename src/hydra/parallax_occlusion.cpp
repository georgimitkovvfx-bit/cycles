/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/parallax_occlusion.h"

#include "scene/shader_nodes.h"

CCL_NAMESPACE_BEGIN

NODE_DEFINE(ParallaxOcclusionNode)
{
  NodeType *type = NodeType::add("parallax_occlusion", create, NodeType::SHADER);

  /* 1 is the surface, 0 the deepest point, Depth below the surface in UV units. */
  SOCKET_IN_FLOAT(height, "Height", 1.0f);
  SOCKET_IN_POINT(vector, "Vector", zero_float3(), SocketType::LINK_TEXTURE_UV);
  SOCKET_IN_FLOAT(depth, "Depth", 0.05f);
  SOCKET_INT(steps, "Steps", 16);
  SOCKET_STRING(uv_map, "UV Map", ustring());

  SOCKET_OUT_POINT(vector, "Vector");

  return type;
}

ParallaxOcclusionNode::ParallaxOcclusionNode() : ShaderNode(get_node_type()) {}

/* Never compiled: expand_parallax_occlusion() replaces the node. */
void ParallaxOcclusionNode::compile(SVMCompiler & /*compiler*/) {}

void ParallaxOcclusionNode::compile(OSLCompiler & /*compiler*/) {}

namespace {

/* A node input: linked to an output, or a constant. */
struct Value {
  ShaderOutput *link = nullptr;
  float3 value = zero_float3();
  Value(ShaderOutput *link) : link(link) {}
  Value(const float value) : value(make_float3(value)) {}
  Value(const float3 value) : value(value) {}
};

void set_input(ShaderGraph *graph, ShaderInput *input, const Value &v)
{
  if (v.link) {
    graph->connect(v.link, input);
  }
  else if (input->type() == SocketType::FLOAT) {
    input->set(v.value.x);
  }
  else {
    input->set(v.value);
  }
}

ShaderOutput *math(ShaderGraph *graph,
                   const NodeMathType type,
                   const Value &a,
                   const Value &b,
                   const Value &c = 0.0f)
{
  MathNode *node = graph->create_node<MathNode>();
  node->set_math_type(type);
  set_input(graph, node->input("Value1"), a);
  set_input(graph, node->input("Value2"), b);
  set_input(graph, node->input("Value3"), c);
  return node->output("Value");
}

ShaderOutput *vector_math(ShaderGraph *graph,
                          const NodeVectorMathType type,
                          const Value &a,
                          const Value &b,
                          const Value &c = 0.0f,
                          const Value &scale = 1.0f,
                          const char *output = "Vector")
{
  VectorMathNode *node = graph->create_node<VectorMathNode>();
  node->set_math_type(type);
  set_input(graph, node->input("Vector1"), a);
  set_input(graph, node->input("Vector2"), b);
  set_input(graph, node->input("Vector3"), c);
  set_input(graph, node->input("Scale"), scale);
  return node->output(output);
}

void find_dependencies(ShaderNodeSet &dependencies, ShaderInput *input)
{
  ShaderNode *node = input->link ? input->link->parent : nullptr;
  if (node && !dependencies.contains(node)) {
    for (ShaderInput *in : node->inputs) {
      find_dependencies(dependencies, in);
    }
    dependencies.insert(node);
  }
}

void expand(ShaderGraph *graph, ParallaxOcclusionNode *pom)
{
  ShaderOutput *out = pom->output("Vector");
  if (out->links.empty()) {
    return;
  }
  ShaderInput *heightIn = pom->input("Height");
  ShaderInput *vectorIn = pom->input("Vector");
  ShaderInput *depthIn = pom->input("Depth");
  /* Each step copies the nodes linked to Height. */
  const int steps = clamp(pom->get_steps(), 1, 64);
  const float layer = 1.0f / float(steps);

  /* Unlinked: the UVs of the UV map of the tangent frame. */
  ShaderOutput *uv0 = vectorIn->link;
  if (!uv0 && !pom->get_uv_map().empty()) {
    UVMapNode *uvMap = graph->create_node<UVMapNode>();
    uvMap->set_attribute(pom->get_uv_map());
    uv0 = uvMap->output("UV");
  }
  else if (!uv0) {
    uv0 = graph->create_node<TextureCoordinateNode>()->output("UV");
  }

  /* View direction in the tangent space of the UV map. Normal Map nodes give the tangent and the
   * bitangent with its sign, like normal maps use them. */
  GeometryNode *geometry = graph->create_node<GeometryNode>();
  const auto axis = [&](const float3 color) {
    NormalMapNode *node = graph->create_node<NormalMapNode>();
    node->set_space(NODE_NORMAL_MAP_TANGENT);
    node->set_attribute(pom->get_uv_map());
    node->set_color(color);
    return node->output("Normal");
  };
  const auto dot = [&](ShaderOutput *a, ShaderOutput *b) {
    return vector_math(graph, NODE_VECTOR_MATH_DOT_PRODUCT, a, b, 0.0f, 1.0f, "Value");
  };
  ShaderOutput *incoming = geometry->output("Incoming");
  /* Limits the shift at grazing angles. */
  ShaderOutput *vz = math(
      graph, NODE_MATH_MAXIMUM, dot(incoming, geometry->output("Normal")), 0.05f);
  CombineXYZNode *view = graph->create_node<CombineXYZNode>();
  graph->connect(
      math(graph, NODE_MATH_DIVIDE, dot(incoming, axis(make_float3(1.0f, 0.5f, 0.5f))), vz),
      view->input("X"));
  graph->connect(
      math(graph, NODE_MATH_DIVIDE, dot(incoming, axis(make_float3(0.5f, 1.0f, 0.5f))), vz),
      view->input("Y"));
  /* Texture coordinate shift at the deepest point, away from the viewer. */
  ShaderOutput *shift = vector_math(graph,
                                    NODE_VECTOR_MATH_SCALE,
                                    view->output("Vector"),
                                    0.0f,
                                    0.0f,
                                    depthIn->link ? Value(depthIn->link) :
                                                    Value(pom->get_depth()));

  /* Copies of the height nodes reading other texture coordinates: inputs linked like the Vector
   * input, to the UV output of a Texture Coordinate or UV Map node, or unlinked UV inputs. */
  ShaderNodeSet dependencies;
  find_dependencies(dependencies, heightIn);
  const auto readsUV = [&](const ShaderInput *in) -> bool {
    if (!in->link) {
      return in->flags() & SocketType::LINK_TEXTURE_UV;
    }
    const NodeType *source = in->link->parent->type;
    return in->link == vectorIn->link || ((source == TextureCoordinateNode::get_node_type() ||
                                           source == UVMapNode::get_node_type()) &&
                                          in->link->name() == "UV");
  };
  const auto height = [&](ShaderOutput *uv) -> Value {
    if (!heightIn->link) {
      return pom->get_height();
    }
    ShaderNodeMap copies;
    for (ShaderNode *node : dependencies) {
      copies[node] = node->clone(graph);
    }
    for (ShaderNode *node : dependencies) {
      for (ShaderInput *in : node->inputs) {
        ShaderInput *copy = copies[node]->input(in->name());
        if (readsUV(in)) {
          graph->connect(uv, copy);
        }
        else if (in->link) {
          graph->connect(copies[in->link->parent]->output(in->link->name()), copy);
        }
      }
    }
    return copies[heightIn->link->parent]->output(heightIn->link->name());
  };

  /* Depth below the surface minus the depth of the height field at layer i: positive above it. */
  const auto above = [&](const int i) {
    ShaderOutput *uv = vector_math(
        graph, NODE_VECTOR_MATH_MULTIPLY_ADD, shift, make_float3(-i * layer), uv0);
    return math(graph, NODE_MATH_SUBTRACT, 1.0f - i * layer, height(uv));
  };
  constexpr float epsilon = 1e-5f; /* A height of exactly 0 is hit at the last layer. */
  ShaderOutput *previous = above(0);
  ShaderOutput *found = math(graph, NODE_MATH_LESS_THAN, previous, epsilon);
  Value hit = 0.0f; /* Depth of the first hit, 0 to 1. */
  for (int i = 1; i <= steps; ++i) {
    ShaderOutput *current = above(i);
    ShaderOutput *foundNow = math(
        graph, NODE_MATH_MAXIMUM, found, math(graph, NODE_MATH_LESS_THAN, current, epsilon));
    ShaderOutput *first = math(graph, NODE_MATH_SUBTRACT, foundNow, found);
    /* Linear refinement between the layers above and below the height field. */
    ShaderOutput *t = math(
        graph, NODE_MATH_DIVIDE, previous, math(graph, NODE_MATH_SUBTRACT, previous, current));
    ShaderOutput *depth = math(graph, NODE_MATH_MULTIPLY_ADD, t, layer, (i - 1) * layer);
    hit = math(graph, NODE_MATH_MULTIPLY_ADD, first, depth, hit);
    found = foundNow;
    previous = current;
  }

  ShaderOutput *result = vector_math(graph,
                                     NODE_VECTOR_MATH_MULTIPLY_ADD,
                                     shift,
                                     math(graph, NODE_MATH_MULTIPLY, hit, -1.0f),
                                     uv0);
  const vector<ShaderInput *> links = out->links;
  for (ShaderInput *to : links) {
    graph->disconnect(to);
    graph->connect(result, to);
  }
}

}  // namespace

void expand_parallax_occlusion(ShaderGraph *graph)
{
  vector<ParallaxOcclusionNode *> nodes;
  for (ShaderNode *node : graph->nodes) {
    if (node->type == ParallaxOcclusionNode::get_node_type()) {
      nodes.push_back(static_cast<ParallaxOcclusionNode *>(node));
    }
  }
  for (ParallaxOcclusionNode *node : nodes) {
    expand(graph, node);
  }
}

CCL_NAMESPACE_END
