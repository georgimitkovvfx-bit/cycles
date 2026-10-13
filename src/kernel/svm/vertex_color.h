/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/geom/attribute.h"
#include "kernel/geom/primitive.h"
#include "kernel/svm/node_types.h"
#include "kernel/svm/util.h"
#include "util/math_base.h"

CCL_NAMESPACE_BEGIN

/* Color of a float, float2 or float3 attribute, read from the device array of its type. Float3Type
 * is float3 or dual3. */
template<typename Float3Type>
ccl_device_inline Float3Type svm_vertex_color_attribute(KernelGlobals kg,
                                                        const ccl_private ShaderData *sd,
                                                        const AttributeDescriptor desc)
{
  using FloatType = dual_scalar_t<Float3Type>;
  if (desc.type == NODE_ATTR_FLOAT) {
    const FloatType f = primitive_surface_attribute<FloatType>(kg, sd, desc);
    return make_float3(f, f, f);
  }
  if (desc.type == NODE_ATTR_FLOAT2) {
    if constexpr (is_dual_v<Float3Type>) {
      return make_float3(primitive_surface_attribute<dual2>(kg, sd, desc));
    }
    else {
      return make_float3(primitive_surface_attribute<float2>(kg, sd, desc));
    }
  }
  return primitive_surface_attribute<Float3Type>(kg, sd, desc);
}

ccl_device_noinline void svm_node_vertex_color(KernelGlobals kg,
                                               ccl_private ShaderData *sd,
                                               ccl_private float *ccl_restrict stack,
                                               const ccl_global SVMNodeVertexColor &ccl_restrict
                                                   node)
{
  float3 color;
  float alpha;

  const AttributeDescriptor descriptor = find_attribute(kg, sd, node.layer_id);
  if (is_attribute_found(descriptor)) {
    if (descriptor.type == NODE_ATTR_FLOAT4 || descriptor.type == NODE_ATTR_RGBA) {
      const float4 vertex_color = primitive_surface_attribute<float4>(kg, sd, descriptor);
      color = make_float3(vertex_color);
      alpha = vertex_color.w;
    }
    else {
      color = svm_vertex_color_attribute<float3>(kg, sd, descriptor);
      alpha = 1.0f;
    }
  }
  else {
    color = make_float3(0.0f, 0.0f, 0.0f);
    alpha = 0.0f;
  }

  if (stack_valid(node.color_offset)) {
    stack_store_float3(stack, node.color_offset, color);
  }
  if (stack_valid(node.alpha_offset)) {
    stack_store_float(stack, node.alpha_offset, alpha);
  }
}

ccl_device_noinline void svm_node_vertex_color_derivative(
    KernelGlobals kg,
    ccl_private ShaderData *sd,
    ccl_private float *ccl_restrict stack,
    const ccl_global SVMNodeVertexColor &ccl_restrict node)
{
  float3 color;
  float alpha;

  const AttributeDescriptor descriptor = find_attribute(kg, sd, node.layer_id);
  if (is_attribute_found(descriptor)) {
    if (descriptor.type == NODE_ATTR_FLOAT4 || descriptor.type == NODE_ATTR_RGBA) {
      dual4 vertex_color = primitive_surface_attribute<dual4>(kg, sd, descriptor);
      if (node.bump_offset == NODE_BUMP_OFFSET_DX) {
        vertex_color.val += vertex_color.dx * node.bump_filter_width;
      }
      else if (node.bump_offset == NODE_BUMP_OFFSET_DY) {
        vertex_color.val += vertex_color.dy * node.bump_filter_width;
      }
      color = make_float3(vertex_color.val);
      alpha = vertex_color.val.w;
    }
    else {
      dual3 vertex_color = svm_vertex_color_attribute<dual3>(kg, sd, descriptor);
      if (node.bump_offset == NODE_BUMP_OFFSET_DX) {
        vertex_color.val += vertex_color.dx * node.bump_filter_width;
      }
      else if (node.bump_offset == NODE_BUMP_OFFSET_DY) {
        vertex_color.val += vertex_color.dy * node.bump_filter_width;
      }
      color = vertex_color.val;
      alpha = 1.0f;
    }
  }
  else {
    color = make_float3(0.0f, 0.0f, 0.0f);
    alpha = 0.0f;
  }

  if (stack_valid(node.color_offset)) {
    stack_store_float3(stack, node.color_offset, color);
  }
  if (stack_valid(node.alpha_offset)) {
    stack_store_float(stack, node.alpha_offset, alpha);
  }
}

CCL_NAMESPACE_END
