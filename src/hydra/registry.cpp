/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/registry.h"
#include "hydra/dome_texture.h"
#include "hydra/parallax_occlusion.h"

#include "graph/node_enum.h"
#include "graph/node_type.h"
#include "scene/shader_nodes.h"

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/js/json.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/fileUtils.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/staticTokens.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/sdr/shaderNodeMetadata.h>
#include <pxr/usd/sdr/shaderProperty.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

PXR_NAMESPACE_OPEN_SCOPE

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (cycles)
    ((identifierPrefix, "cycles_"))
);
// clang-format on

namespace {

/* An input's types: Sdr's, and the USD (Sdf) type USD Nodes and the delegate use, which UsdShade's validation
 * checks authored inputs against (Sdr has no boolean, token or float2 of its own). */
struct Types {
  TfToken sdr;
  const char *usd;
  bool dynamicArray;
};

bool TypesOf(const ccl::SocketType &socket, Types &types)
{
  const auto set = [&](const TfToken &sdr, const char *usd, const bool array = false) {
    types = {sdr, usd, array};
    return true;
  };
  switch (socket.type) {
    case ccl::SocketType::BOOLEAN:
      return set(SdrPropertyTypes->Int, "bool");
    case ccl::SocketType::INT:
    case ccl::SocketType::UINT:
      return set(SdrPropertyTypes->Int, "int");
    case ccl::SocketType::FLOAT:
      return set(SdrPropertyTypes->Float, "float");
    case ccl::SocketType::COLOR:
      return set(SdrPropertyTypes->Color, "color3f");
    case ccl::SocketType::VECTOR:
      return set(SdrPropertyTypes->Vector, "vector3f");
    case ccl::SocketType::POINT:
      return set(SdrPropertyTypes->Point, "point3f");
    case ccl::SocketType::NORMAL:
      return set(SdrPropertyTypes->Normal, "normal3f");
    case ccl::SocketType::POINT2:
      return set(SdrPropertyTypes->Float, "float2");
    case ccl::SocketType::TRANSFORM:
      return set(SdrPropertyTypes->Matrix, "matrix4d");
    case ccl::SocketType::CLOSURE:
      return set(SdrPropertyTypes->Terminal, "token");
    case ccl::SocketType::ENUM:
      return set(SdrPropertyTypes->String, "token");
    case ccl::SocketType::STRING:
      /* File names are assets (USD resolves them), other strings strings. */
      return socket.name == "filename" ? set(SdrPropertyTypes->String, "asset") :
                                         set(SdrPropertyTypes->String, "string");
    case ccl::SocketType::FLOAT_ARRAY:
      return set(SdrPropertyTypes->Float, "float[]", true);
    case ccl::SocketType::INT_ARRAY:
      return set(SdrPropertyTypes->Int, "int[]", true);
    case ccl::SocketType::COLOR_ARRAY:
      return set(SdrPropertyTypes->Color, "color3f[]", true);
    case ccl::SocketType::VECTOR_ARRAY:
      return set(SdrPropertyTypes->Vector, "vector3f[]", true);
    default:
      /* Node references and the rest: not shader inputs. */
      return false;
  }
}

/* The socket's default, of its USD type; empty where there is none to give. */
VtValue DefaultOf(const ccl::SocketType &socket, const Types &types)
{
  const void *value = socket.default_value;
  if (value == nullptr || types.dynamicArray) {
    return VtValue();
  }
  switch (socket.type) {
    case ccl::SocketType::BOOLEAN:
      return VtValue(*static_cast<const bool *>(value));
    case ccl::SocketType::INT:
      return VtValue(*static_cast<const int *>(value));
    case ccl::SocketType::UINT:
      return VtValue(int(*static_cast<const uint32_t *>(value)));
    case ccl::SocketType::FLOAT:
      return VtValue(*static_cast<const float *>(value));
    case ccl::SocketType::COLOR:
    case ccl::SocketType::VECTOR:
    case ccl::SocketType::POINT:
    case ccl::SocketType::NORMAL: {
      const ccl::float3 v = *static_cast<const ccl::float3 *>(value);
      return VtValue(GfVec3f(v.x, v.y, v.z));
    }
    case ccl::SocketType::POINT2: {
      const ccl::float2 v = *static_cast<const ccl::float2 *>(value);
      return VtValue(GfVec2f(v.x, v.y));
    }
    case ccl::SocketType::ENUM: {
      const int index = *static_cast<const int *>(value);
      if (socket.enum_values && socket.enum_values->exists(index)) {
        return VtValue(TfToken((*socket.enum_values)[index].string()));
      }
      return VtValue();
    }
    case ccl::SocketType::STRING: {
      const std::string text = static_cast<const ccl::ustring *>(value)->string();
      return std::string(types.usd) == "asset" ? VtValue(SdfAssetPath(text)) : VtValue(text);
    }
    default:
      return VtValue();
  }
}

SdrShaderPropertyUniquePtr PropertyOf(const ccl::SocketType &socket, const Types &types, const bool isOutput)
{
  SdrTokenMap metadata;
  metadata[SdrPropertyMetadata->SdrUsdDefinitionType] = types.usd;
  if (!socket.ui_name.empty()) {
    metadata[SdrPropertyMetadata->Label] = socket.ui_name.string();
  }
  if (socket.type == ccl::SocketType::BOOLEAN) {
    metadata[SdrPropertyMetadata->Widget] = "checkBox";
  }
  if (std::string(types.usd) == "asset") {
    metadata[SdrPropertyMetadata->IsAssetIdentifier] = "1";
  }
  if (types.dynamicArray) {
    metadata[SdrPropertyMetadata->IsDynamicArray] = "1";
  }
  /* Only sockets Cycles marks linkable take a connection. */
  if (!isOutput && !(socket.flags & ccl::SocketType::LINKABLE)) {
    metadata[SdrPropertyMetadata->Connectable] = "0";
  }
  SdrOptionVec options;
  if (socket.type == ccl::SocketType::ENUM && socket.enum_values) {
    for (const auto &entry : *socket.enum_values) {
      const TfToken name(entry.first.string());
      options.emplace_back(name, name);
    }
  }
  /* USD property names cannot have dots: "tex_mapping.scale" is published as "tex_mapping_scale", as the Houdini
   * package names it (the delegate ignores dots and underscores when it matches a name to a socket). */
  std::string name = socket.name.string();
  std::replace(name.begin(), name.end(), '.', '_');
  return std::make_unique<SdrShaderProperty>(TfToken(name),
                                             types.sdr,
                                             DefaultOf(socket, types),
                                             isOutput,
                                             0,
                                             metadata,
                                             SdrTokenMap(),
                                             options);
}

/* Shading context, from the closure the node outputs. */
TfToken ContextOf(const ccl::NodeType &nodeType)
{
  for (const ccl::SocketType &socket : nodeType.outputs) {
    if (socket.type == ccl::SocketType::CLOSURE) {
      return socket.name == "volume" ? SdrNodeContext->Volume : SdrNodeContext->Surface;
    }
    if (socket.name == "displacement") {
      return SdrNodeContext->Displacement;
    }
  }
  return SdrNodeContext->Pattern;
}

/* "principled_bsdf" -> "Principled Bsdf", as the Houdini package's shader dump. */
std::string LabelOf(const std::string &name)
{
  std::string label;
  bool upper = true;
  for (const char c : name) {
    if (c == '_') {
      label += ' ';
      upper = true;
    }
    else {
      label += upper ? char(std::toupper(static_cast<unsigned char>(c))) : c;
      upper = false;
    }
  }
  return label;
}

/* Node name -> menu category, from the plugin's resources/sdr/categories.json. */
const std::unordered_map<std::string, std::string> &Categories()
{
  static const std::unordered_map<std::string, std::string> categories = [] {
    std::unordered_map<std::string, std::string> result;
    const PlugPluginPtr plugin = PlugRegistry::GetInstance().GetPluginWithName("sdrCycles");
    if (!plugin) {
      return result;
    }
    const std::string path = TfStringCatPaths(plugin->GetResourcePath(), "sdr/categories.json");
    std::ifstream file(path);
    if (!file) {
      return result;
    }
    std::stringstream text;
    text << file.rdbuf();
    const JsValue json = JsParseString(text.str());
    if (json.IsObject()) {
      for (const auto &[name, category] : json.GetJsObject()) {
        if (category.IsString()) {
          result[name] = category.GetString();
        }
      }
    }
    return result;
  }();
  return categories;
}

}  // namespace

SdrCyclesDiscoveryPlugin::SdrCyclesDiscoveryPlugin() = default;
SdrCyclesDiscoveryPlugin::~SdrCyclesDiscoveryPlugin() = default;

SdrShaderNodeDiscoveryResultVec SdrCyclesDiscoveryPlugin::DiscoverShaderNodes(const Context & /*context*/)
{
  /* Shader node types register themselves from static initializers: referencing one of each object file keeps it
   * when linking the static libraries (the delegate's own nodes too). */
  (void)ccl::ImageTextureNode::get_node_type();
  (void)ccl::DomeTextureNode::get_node_type();
  (void)ccl::ParallaxOcclusionNode::get_node_type();

  SdrShaderNodeDiscoveryResultVec results;
  for (const ccl::ustring &typeName : ccl::NodeType::type_names()) {
    const ccl::NodeType *nodeType = ccl::NodeType::find(typeName);
    if (!nodeType || nodeType->type != ccl::NodeType::SHADER) {
      continue;
    }
    const std::string name = nodeType->name.string();
    if (name.rfind("convert_", 0) == 0) {
      continue; /* Cycles' implicit conversions, not authored. */
    }
    results.emplace_back(TfToken(_tokens->identifierPrefix.GetString() + name),
                         SdrVersion(1, 0).GetAsDefault(),
                         name,
                         _tokens->cycles,
                         _tokens->cycles,
                         _tokens->cycles,
                         std::string(),
                         std::string());
  }
  return results;
}

const SdrStringVec &SdrCyclesDiscoveryPlugin::GetSearchURIs() const
{
  static const SdrStringVec none;
  return none;
}

SdrCyclesParserPlugin::SdrCyclesParserPlugin() = default;
SdrCyclesParserPlugin::~SdrCyclesParserPlugin() = default;

SdrShaderNodeUniquePtr SdrCyclesParserPlugin::ParseShaderNode(const SdrShaderNodeDiscoveryResult &discoveryResult)
{
  const ccl::NodeType *nodeType = ccl::NodeType::find(ccl::ustring(discoveryResult.name));
  if (!nodeType) {
    return GetInvalidShaderNode(discoveryResult);
  }

  SdrTokenMap metadata;
  metadata[SdrNodeMetadata->Label] = LabelOf(discoveryResult.name);
  const auto category = Categories().find(discoveryResult.name);
  metadata[SdrNodeMetadata->Category] = category != Categories().end() ? category->second : "Cycles";

  SdrShaderPropertyUniquePtrVec properties;
  Types types;
  for (const ccl::SocketType &socket : nodeType->inputs) {
    /* Internal sockets are Cycles' own, not the user's. */
    if (!(socket.flags & ccl::SocketType::INTERNAL) && TypesOf(socket, types)) {
      properties.push_back(PropertyOf(socket, types, false));
    }
  }
  for (const ccl::SocketType &socket : nodeType->outputs) {
    if (TypesOf(socket, types)) {
      properties.push_back(PropertyOf(socket, types, true));
    }
  }

  return std::make_unique<SdrShaderNode>(discoveryResult.identifier,
                                         discoveryResult.version,
                                         discoveryResult.name,
                                         discoveryResult.family,
                                         ContextOf(*nodeType),
                                         discoveryResult.sourceType,
                                         discoveryResult.uri,
                                         discoveryResult.resolvedUri,
                                         std::move(properties),
                                         SdrShaderNodeMetadata(metadata));
}

const SdrTokenVec &SdrCyclesParserPlugin::GetDiscoveryTypes() const
{
  static const SdrTokenVec types = {_tokens->cycles};
  return types;
}

const TfToken &SdrCyclesParserPlugin::GetSourceType() const
{
  return _tokens->cycles;
}

SDR_REGISTER_DISCOVERY_PLUGIN(SdrCyclesDiscoveryPlugin)
SDR_REGISTER_PARSER_PLUGIN(SdrCyclesParserPlugin)

PXR_NAMESPACE_CLOSE_SCOPE
