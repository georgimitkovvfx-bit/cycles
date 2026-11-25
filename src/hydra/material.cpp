/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/material.h"
#ifdef WITH_HYDRA_MATERIALX
#  include "hydra/materialx.h"
#endif
#ifdef WITH_HOUDINI_IMAGES
#  include "hydra/houdini_image.h"
#endif
#include "hydra/dome_texture.h"
#include "hydra/node_util.h"
#include "hydra/parallax_occlusion.h"
#include "hydra/session.h"
#include "hydra/util.h"
#ifdef WITH_OSL
#  include "scene/osl.h"
#endif
#include "scene/scene.h"
#include "scene/shader.h"
// After shader.h, so that Background::set_shader uses the correct set()
#include "scene/background.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "util/colorspace.h"
#include "util/md5.h"
#include "util/path.h"
#include "util/string.h"

#include <pxr/base/js/json.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/dataSourceHash.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>

#include <OpenImageIO/filesystem.h>

#include <algorithm>
#include <filesystem>
#include <set>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Normalize a material network node name to a full SdfPath. The schema may
 * provide either a full path or a bare identifier. */
static SdfPath MaterialNodeNameToSdfPath(const TfToken &nodeName)
{
  const std::string &s = nodeName.GetString();
  if (s.empty()) {
    return SdfPath::EmptyPath();
  }
  if (s[0] == '/' && SdfPath::IsValidPathString(s)) {
    return SdfPath(s);
  }
  return SdfPath::AbsoluteRootPath().AppendChild(nodeName);
}

/* Houdini's AOV Output node has a "shader" input passed through to its "out" output, see
 * shader2hda.py. */
static const TfToken kAovPassThroughInput("shader");

static bool IsAovPassThrough(const ShaderNode *node, const TfToken &socketName)
{
  return node && node->type->name == "aov_output" &&
         (socketName == kAovPassThroughInput || socketName == "out");
}

/* OSL script node, like Blender's Script node: "cycles_script" with the shader file (.osl or
 * .oso) in "filepath", its inputs and outputs are the shader parameters. */
static const TfToken kScriptFilepath("filepath");

static std::string GetFilepath(const VtValue &value)
{
  if (value.IsHolding<SdfAssetPath>()) {
    const SdfAssetPath &asset = value.UncheckedGet<SdfAssetPath>();
    return asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
  }
  return VtValue::Cast<std::string>(value).GetWithDefault(std::string());
}

/* OSL node from a shader file, or from its bytecode when given (the file is then only a name). */
static ShaderNode *CreateOSLNode(ShaderGraph *graph,
                                 Scene *scene,
                                 const std::string &filepath,
                                 const SdfPath &nodePath,
                                 const std::string &bytecode = "")
{
  if (filepath.empty()) {
    TF_WARN("Script node '%s' has no shader file", nodePath.GetText());
    return nullptr;
  }
#ifdef WITH_OSL
  if (!scene->shader_manager->use_osl()) {
    TF_WARN(
        "Script node '%s' needs Open Shading Language (render setting) and a CPU or OptiX "
        "device",
        nodePath.GetText());
    return nullptr;
  }
  ShaderNode *node;
  if (bytecode.empty()) {
    node = OSLShaderManager::osl_node(graph, scene, filepath, "");
  }
  else {
    MD5Hash md5;
    md5.append(bytecode);
    node = OSLShaderManager::osl_node(graph, scene, "", md5.get_hex(), bytecode);
  }
  if (!node) {
    TF_WARN("Script node '%s' could not load '%s'", nodePath.GetText(), filepath.c_str());
  }
  return node;
#else
  (void)graph;
  (void)scene;
  (void)bytecode;
  TF_WARN("Script node '%s' needs a build with OSL", nodePath.GetText());
  return nullptr;
#endif
}

static ShaderNode *CreateScriptNode(ShaderGraph *graph,
                                    Scene *scene,
                                    HdMaterialNodeParameterContainerSchema params,
                                    const SdfPath &nodePath)
{
  const auto valueDs = params.Get(kScriptFilepath).GetValue();
  return CreateOSLNode(
      graph, scene, valueDs ? GetFilepath(valueDs->GetValue(0.0f)) : std::string(), nodePath);
}

/* Raycast "Attributes" panel, like Blender's Raycast node: a string parameter "attribute<N>" names
 * an attribute to read at the hit point, through the outputs "attribute<N>" (color or vector),
 * "attribute<N>_fac" and "attribute<N>_alpha" (see shader2hda.py). The outputs depend on the
 * parameters, so they are added when the node is created, before the connections. */
static bool IsRaycastAttribute(const ShaderNode *node, const TfToken &name)
{
  return node->type == RaycastNode::get_node_type() && name.GetString().starts_with("attribute");
}

static void AddRaycastAttributeOutputs(ShaderNode *node,
                                       HdMaterialNodeParameterContainerSchema params)
{
  for (const TfToken &name : params.GetNames()) {
    if (!IsRaycastAttribute(node, name)) {
      continue;
    }
    const auto valueDs = params.Get(name).GetValue();
    const VtValue value = valueDs ? valueDs->GetValue(0.0f) : VtValue();
    if (!value.IsHolding<std::string>() || value.UncheckedGet<std::string>().empty()) {
      continue;
    }
    const ustring attribute(value.UncheckedGet<std::string>());
    RaycastNode *raycast = static_cast<RaycastNode *>(node);
    raycast->add_output_attribute_socket(
        attribute, RaycastNode::ATTR_OUTPUT_FLOAT3, ustring(name.GetString()));
    raycast->add_output_attribute_socket(
        attribute, RaycastNode::ATTR_OUTPUT_FLOAT, ustring(name.GetString() + "_fac"));
    raycast->add_output_attribute_socket(
        attribute, RaycastNode::ATTR_OUTPUT_FLOAT_ALPHA, ustring(name.GetString() + "_alpha"));
  }
}

/* A shader implemented by an OSL file (info:implementationSource = "sourceAsset" with
 * info:OSL:sourceAsset), e.g. Houdini's OSL shader nodes, is a script node too. */
static ShaderNode *CreateSourceAssetNode(ShaderGraph *graph,
                                         Scene *scene,
                                         const HdContainerDataSourceHandle &typeInfo,
                                         const SdfPath &nodePath)
{
  if (!typeInfo) {
    return nullptr;
  }
  const auto value = [&](const char *name) {
    const auto valueDs = HdSampledDataSource::Cast(typeInfo->Get(TfToken(name)));
    return valueDs ? valueDs->GetValue(0.0f) : VtValue();
  };

  const std::string implementation =
      VtValue::Cast<TfToken>(value("implementationSource")).GetWithDefault(TfToken()).GetString();
  if (implementation == "sourceAsset") {
    const std::string filepath = GetFilepath(value("OSL:sourceAsset"));
    if (filepath.empty() || path_is_file(filepath)) {
      return filepath.empty() ? nullptr : CreateOSLNode(graph, scene, filepath, nodePath);
    }
    /* Not a file, e.g. Houdini serves the compiled OSL of its shader nodes through its asset
     * resolver. */
    std::string bytecode;
    if (const std::shared_ptr<ArAsset> asset = ArGetResolver().OpenAsset(ArResolvedPath(filepath)))
    {
      if (const std::shared_ptr<const char> buffer = asset->GetBuffer()) {
        bytecode.assign(buffer.get(), asset->GetSize());
      }
    }
    if (bytecode.empty()) {
      TF_WARN("Script node '%s' could not read '%s'", nodePath.GetText(), filepath.c_str());
      return nullptr;
    }
    return CreateOSLNode(graph, scene, filepath, nodePath, bytecode);
  }
  if (implementation == "sourceCode") {
    const std::string code =
        VtValue::Cast<std::string>(value("OSL:sourceCode")).GetWithDefault(std::string());
    if (code.empty()) {
      return nullptr;
    }
    /* Cycles compiles shader files, so write the code to a file named by its hash. */
    MD5Hash md5;
    md5.append(code);
    const std::string dir = path_join(std::filesystem::temp_directory_path().string(),
                                      "hdCycles_osl");
    const std::string filepath = path_join(dir, md5.get_hex() + ".osl");
    if (!path_is_file(filepath) && !path_write_text(filepath, code)) {
      TF_WARN("Script node '%s' could not write '%s'", nodePath.GetText(), filepath.c_str());
      return nullptr;
    }
    return CreateOSLNode(graph, scene, filepath, nodePath);
  }
  return nullptr;
}

/* Default values for Cycles shader node sockets that differ from the Cycles socket defaults,
 * read from "shader_defaults.json" in the resource directory. Host integrations write this
 * when their shader node UI shows other defaults, e.g. the Houdini shader nodes show Blender's
 * defaults. Hosts only write values that differ from the UI default to USD, so the render only
 * matches the UI when these are applied first. */
class ShaderNodeDefaults {
  using Defaults = std::vector<std::pair<ustring, VtValue>>;

 public:
  ShaderNodeDefaults()
  {
    const string filepath = path_get("shader_defaults.json");
    if (!path_exists(filepath)) {
      return;
    }
    string text;
    if (!path_read_text(filepath, text)) {
      TF_WARN("Could not read shader node defaults from '%s'", filepath.c_str());
      return;
    }
    JsParseError error;
    const JsValue root = JsParseString(text, &error);
    if (!root.IsObject()) {
      TF_WARN("Could not parse shader node defaults from '%s', line %u: %s",
              filepath.c_str(),
              error.line,
              error.reason.c_str());
      return;
    }
    for (const auto &[nodeName, sockets] : root.GetJsObject()) {
      if (!sockets.IsObject()) {
        continue;
      }
      Defaults &defaults = _nodes[nodeName];
      for (const auto &[socketName, value] : sockets.GetJsObject()) {
        VtValue converted = ConvertValue(value);
        if (!converted.IsEmpty()) {
          defaults.emplace_back(ustring(socketName), std::move(converted));
        }
      }
    }
  }

  void Apply(ShaderNode *node, const string &nodeType) const
  {
    const auto it = _nodes.find(nodeType);
    if (it == _nodes.end()) {
      return;
    }
    for (const auto &[socketName, value] : it->second) {
      if (const SocketType *socket = node->type->find_input(socketName)) {
        SetNodeValue(node, *socket, value);
      }
    }
  }

 private:
  static VtValue ConvertValue(const JsValue &value)
  {
    if (value.IsBool()) {
      return VtValue(value.GetBool());
    }
    if (value.IsInt()) {
      return VtValue(value.GetInt());
    }
    if (value.IsReal()) {
      return VtValue(float(value.GetReal()));
    }
    if (value.IsString()) {
      return VtValue(value.GetString());
    }
    if (value.IsArrayOf<double>()) {
      const std::vector<double> array = value.GetArrayOf<double>();
      if (array.size() == 2) {
        return VtValue(GfVec2f(array[0], array[1]));
      }
      if (array.size() == 3) {
        return VtValue(GfVec3f(array[0], array[1], array[2]));
      }
    }
    return VtValue();
  }

  std::unordered_map<string, Defaults> _nodes;
};

static const ShaderNodeDefaults &GetShaderNodeDefaults()
{
  static const ShaderNodeDefaults defaults;
  return defaults;
}

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(CyclesMaterialTokens,
    (cycles)
    ((cyclesSurface, "cycles:surface"))
    ((cyclesDisplacement, "cycles:displacement"))
    ((cyclesVolume, "cycles:volume"))
    (UsdPreviewSurface)
    (UsdUVTexture)
    (UsdPrimvarReader_float)
    (UsdPrimvarReader_float2)
    (UsdPrimvarReader_float3)
    (UsdPrimvarReader_float4)
    (UsdPrimvarReader_int)
    (UsdPrimvarReader_normal)
    (UsdPrimvarReader_point)
    (UsdPrimvarReader_vector)
    (UsdTransform2d)
    (a)
    (rgb)
    (r)
    (g)
    (b)
    (result)
    (st)
    (wrapS)
    (wrapT)
    (periodic)
    (specularColor)
    (useSpecularWorkflow)
    (ior)
    (scale)
    (bias)
    (metallic)
    (fallback)
    (opacityThreshold)
    (kma_hair)
    (shift)
    (roughnessAnisotropic)
    (roughnessAzimuthal)
    (roughness_advanced)
    (base)
    (thicknessScale)
    (tint)
    (diffuse)
    (diffuseColor)
);
// clang-format on

/* The value of a node parameter, or `fallback` when it is not authored. */
template<typename T>
static T GetParameter(const HdMaterialNodeParameterContainerSchema &params,
                      const TfToken &name,
                      const T &fallback)
{
  const HdSampledDataSourceHandle ds = params.Get(name).GetValue();
  return ds ? VtValue::Cast<T>(ds->GetValue(0.0f)).GetWithDefault(fallback) : fallback;
}

/* USD's fallback input of texture and primvar readers (float, float2, float3, float4 or int), as
 * a color and alpha, or `value` when it is not authored. */
static GfVec4f GetFallback(const HdMaterialNodeParameterContainerSchema &params,
                           const GfVec4f &value)
{
  const HdSampledDataSourceHandle ds = params.Get(CyclesMaterialTokens->fallback).GetValue();
  const VtValue fallback = ds ? ds->GetValue(0.0f) : VtValue();
  if (fallback.IsHolding<float>() || fallback.IsHolding<int>()) {
    const float f = VtValue::Cast<float>(fallback).Get<float>();
    return GfVec4f(f, f, f, 1.0f);
  }
  if (fallback.IsHolding<GfVec2f>()) {
    const GfVec2f v = fallback.UncheckedGet<GfVec2f>();
    return GfVec4f(v[0], v[1], 0.0f, 1.0f);
  }
  if (fallback.IsHolding<GfVec3f>()) {
    const GfVec3f v = fallback.UncheckedGet<GfVec3f>();
    return GfVec4f(v[0], v[1], v[2], 1.0f);
  }
  return VtValue::Cast<GfVec4f>(fallback).GetWithDefault(value);
}

/* Simple class to handle remapping of USDPreviewSurface nodes and parameters to Cycles
 * equivalents. */
class UsdToCyclesMapping {
  using ParamMap = std::unordered_map<TfToken, ustring, TfToken::HashFunctor>;

 public:
  UsdToCyclesMapping(const char *nodeType, ParamMap paramMap)
      : _nodeType(nodeType), _paramMap(std::move(paramMap))
  {
  }

  ustring nodeType() const
  {
    return _nodeType;
  }

  bool hasParameterMapping(const TfToken &name) const
  {
    return _paramMap.find(name) != _paramMap.end();
  }

  /* Set the values where the USD and Cycles defaults differ, on a new node. */
  virtual void initialize(ShaderNode * /*node*/) const {}

  /* Whether a USD input applies, given the other parameters of the node. */
  virtual bool isInputUsed(const TfToken & /*name*/,
                           const HdMaterialNodeParameterContainerSchema & /*params*/) const
  {
    return true;
  }

  /* Add the nodes that an input needs besides its socket, once the graph is linked. A node whose
   * closure needs more nodes after it replaces `node` by the last one, for the terminals. */
  virtual void finalize(ShaderGraph * /*graph*/,
                        ShaderNode *& /*node*/,
                        const HdMaterialNodeParameterContainerSchema & /*params*/) const
  {
  }

  virtual std::string parameterName(const TfToken &name,
                                    const ShaderInput *inputConnection,
                                    VtValue * /*value*/ = nullptr) const
  {
    /* UsdNode.name -> Node.input. These all follow a simple pattern that we can just
     * remap based on the name or 'Node.input' type. */
    if (inputConnection) {
      if (name == CyclesMaterialTokens->a) {
        return "alpha";
      }
      if (name == CyclesMaterialTokens->rgb) {
        return "color";
      }
      /* TODO: Is there a better mapping than 'color'? */
      if (name == CyclesMaterialTokens->r || name == CyclesMaterialTokens->g ||
          name == CyclesMaterialTokens->b)
      {
        return "color";
      }

      if (name == CyclesMaterialTokens->result) {
        switch (inputConnection->socket_type.type) {
          case SocketType::BOOLEAN:
          case SocketType::FLOAT:
          case SocketType::INT:
          case SocketType::UINT:
            /* Alpha is 1 for attributes without alpha, Fac is the value (or average). */
            return "fac";
          case SocketType::COLOR:
          case SocketType::VECTOR:
          case SocketType::POINT:
          case SocketType::NORMAL:
          default:
            return "color";
        }
      }
    }

    /* Simple mapping case */
    const auto it = _paramMap.find(name);
    return it != _paramMap.end() ? it->second.string() : name.GetString();
  }

 private:
  const ustring _nodeType;
  ParamMap _paramMap;
};

class UsdToCyclesTexture : public UsdToCyclesMapping {
 public:
  using UsdToCyclesMapping::UsdToCyclesMapping;

  void initialize(ShaderNode *node) const override
  {
    /* USD's default wrap (useMetadata) is black outside the image, as Karma; Cycles' repeats. */
    static_cast<ImageTextureNode *>(node)->set_extension(EXTENSION_CLIP);
  }

  std::string parameterName(const TfToken &name,
                            const ShaderInput *inputConnection,
                            VtValue *value) const override
  {
    if (value) {
      /* Remap UsdUVTexture.wrapS and UsdUVTexture.wrapT to cycles_image_texture.extension. */
      if (name == CyclesMaterialTokens->wrapS || name == CyclesMaterialTokens->wrapT) {
        const std::string valueString = VtValue::Cast<std::string>(*value).Get<std::string>();

        /* A value of 'repeat' in USD is equivalent to 'periodic' in Cycles. */
        if (valueString == "repeat") {
          *value = VtValue(CyclesMaterialTokens->periodic);
        }
        /* The image's own wrap mode, black without one (Karma). Cycles reads none. */
        else if (valueString == "useMetadata") {
          *value = VtValue(TfToken("black"));
        }

        return "extension";
      }
    }

    return UsdToCyclesMapping::parameterName(name, inputConnection, value);
  }

  bool isInputUsed(const TfToken &name,
                   const HdMaterialNodeParameterContainerSchema & /*params*/) const override
  {
    /* Applied to the outputs in finalize(), they are not the texture mapping "Scale". */
    return name != CyclesMaterialTokens->scale && name != CyclesMaterialTokens->bias &&
           name != CyclesMaterialTokens->fallback;
  }

  void finalize(ShaderGraph *graph,
                ShaderNode *&node,
                const HdMaterialNodeParameterContainerSchema &params) const override
  {
    /* A missing file or UDIM tile reads the fallback, (0, 0, 0, 1) by default, like in Karma and
     * Storm, instead of Cycles' pink. */
    ImageTextureNode *image = static_cast<ImageTextureNode *>(node);
    const GfVec4f fallback = GetFallback(params, GfVec4f(0.0f, 0.0f, 0.0f, 1.0f));
    image->set_missing(make_float3(fallback[0], fallback[1], fallback[2]));
    image->set_missing_alpha(fallback[3]);

    /* The outputs are the texture values times scale plus bias, e.g. to unpack normal maps.
     * shortcut: authored values only, a connected scale or bias is ignored; add it when an asset
     * needs it. */
    const GfVec4f scale = GetParameter(params, CyclesMaterialTokens->scale, GfVec4f(1.0f));
    const GfVec4f bias = GetParameter(params, CyclesMaterialTokens->bias, GfVec4f(0.0f));
    if (scale == GfVec4f(1.0f) && bias == GfVec4f(0.0f)) {
      return;
    }
    ShaderOutput *color = node->output("Color");
    if (!color->links.empty()) {
      VectorMathNode *math = graph->create_node<VectorMathNode>();
      math->set_math_type(NODE_VECTOR_MATH_MULTIPLY_ADD);
      math->set_vector2(make_float3(scale[0], scale[1], scale[2]));
      math->set_vector3(make_float3(bias[0], bias[1], bias[2]));
      graph->relink(color, math->output("Vector"));
      graph->connect(color, math->input("Vector1"));
    }
    ShaderOutput *alpha = node->output("Alpha");
    if (!alpha->links.empty()) {
      MathNode *math = graph->create_node<MathNode>();
      math->set_math_type(NODE_MATH_MULTIPLY_ADD);
      math->set_value2(scale[3]);
      math->set_value3(bias[3]);
      graph->relink(alpha, math->output("Value"));
      graph->connect(alpha, math->input("Value1"));
    }
  }
};

class UsdToCyclesPreviewSurface : public UsdToCyclesMapping {
 public:
  using UsdToCyclesMapping::UsdToCyclesMapping;

  void initialize(ShaderNode *node) const override
  {
    /* USD's diffuseColor default. emissiveColor is the emitted radiance and defaults to black. */
    PrincipledBsdfNode *bsdf = static_cast<PrincipledBsdfNode *>(node);
    bsdf->set_base_color(make_float3(0.18f));
    bsdf->set_emission_color(zero_float3());
    bsdf->set_emission_strength(1.0f);
    /* USD's clearcoatRoughness default, Cycles' is 0.03. */
    bsdf->set_coat_roughness(0.01f);
  }

  static bool isSpecularWorkflow(const HdMaterialNodeParameterContainerSchema &params)
  {
    return GetParameter(params, CyclesMaterialTokens->useSpecularWorkflow, 0) != 0;
  }

  bool isInputUsed(const TfToken &name,
                   const HdMaterialNodeParameterContainerSchema &params) const override
  {
    /* specularColor only applies in the specular workflow, like in USD and Karma (Blender's USD
     * import always uses it). There metallic does not apply, and the IOR only shapes the Fresnel
     * curve: keep the default, so that the Specular Tint scale in finalize() is fixed. */
    if (name == CyclesMaterialTokens->specularColor) {
      return isSpecularWorkflow(params);
    }
    if (name == CyclesMaterialTokens->ior || name == CyclesMaterialTokens->metallic) {
      return !isSpecularWorkflow(params);
    }
    /* Applied to the opacity in finalize(), and as the displacement terminal. */
    return name != CyclesMaterialTokens->opacityThreshold &&
           name != HdMaterialTerminalTokens->displacement;
  }

  void finalize(ShaderGraph *graph,
                ShaderNode *&node,
                const HdMaterialNodeParameterContainerSchema &params) const override
  {
    PrincipledBsdfNode *bsdf = static_cast<PrincipledBsdfNode *>(node);
    if (isSpecularWorkflow(params)) {
      /* specularColor is the reflectance at normal incidence. Cycles reflects the Specular Tint
       * times the reflectance of the IOR (0.04 for 1.5), so scale it by the inverse. */
      const float ior = bsdf->get_ior();
      const float scale = 1.0f / sqr((ior - 1.0f) / (ior + 1.0f));
      ShaderInput *tint = bsdf->input("Specular Tint");
      if (ShaderOutput *link = tint->link) {
        VectorMathNode *math = graph->create_node<VectorMathNode>();
        math->set_math_type(NODE_VECTOR_MATH_SCALE);
        math->set_scale(scale);
        graph->disconnect(tint);
        graph->connect(link, math->input("Vector1"));
        graph->connect(math->output("Vector"), tint);
      }
      else {
        /* An unauthored specularColor is black, unlike the white Specular Tint default. */
        const bool authored = bool(params.Get(CyclesMaterialTokens->specularColor).GetValue());
        bsdf->set_specular_tint(authored ? bsdf->get_specular_tint() * scale : zero_float3());
      }
    }

    /* An opacityThreshold above 0 makes a cutout: opacity below it is transparent, the rest
     * opaque. */
    const float threshold = GetParameter(params, CyclesMaterialTokens->opacityThreshold, 0.0f);
    if (threshold > 0.0f) {
      ShaderInput *alpha = bsdf->input("Alpha");
      if (ShaderOutput *link = alpha->link) {
        MathNode *below = graph->create_node<MathNode>();
        below->set_math_type(NODE_MATH_LESS_THAN);
        below->set_value2(threshold);
        MathNode *opaque = graph->create_node<MathNode>();
        opaque->set_math_type(NODE_MATH_SUBTRACT);
        opaque->set_value1(1.0f);
        graph->disconnect(alpha);
        graph->connect(link, below->input("Value1"));
        graph->connect(below->output("Value"), opaque->input("Value2"));
        graph->connect(opaque->output("Value"), alpha);
      }
      else {
        bsdf->set_alpha(bsdf->get_alpha() < threshold ? 0.0f : 1.0f);
      }
    }

    /* normal is in tangent space in [-1, 1] (a normal map unpacked with the texture's scale and
     * bias), the Principled Normal in world space: convert with a Normal Map node, which reads
     * the normal packed into [0, 1]. */
    ShaderInput *normal = bsdf->input("Normal");
    if (ShaderOutput *link = normal->link) {
      VectorMathNode *pack = graph->create_node<VectorMathNode>();
      pack->set_math_type(NODE_VECTOR_MATH_MULTIPLY_ADD);
      pack->set_vector2(make_float3(0.5f));
      pack->set_vector3(make_float3(0.5f));
      NormalMapNode *normalMap = graph->create_node<NormalMapNode>();
      graph->disconnect(normal);
      graph->connect(link, pack->input("Vector1"));
      graph->connect(pack->output("Vector"), normalMap->input("Color"));
      graph->connect(normalMap->output("Normal"), normal);
    }
  }
};

class UsdToCyclesPrimvarReader : public UsdToCyclesMapping {
 public:
  using UsdToCyclesMapping::UsdToCyclesMapping;

  bool isInputUsed(const TfToken &name,
                   const HdMaterialNodeParameterContainerSchema & /*params*/) const override
  {
    return name != CyclesMaterialTokens->fallback;
  }

  void finalize(ShaderGraph * /*graph*/,
                ShaderNode *&node,
                const HdMaterialNodeParameterContainerSchema &params) const override
  {
    /* Geometry without the primvar reads the fallback (e.g. 1 for a displayOpacity reader into
     * opacity), the Attribute node's Fac is its average. */
    AttributeNode *attribute = static_cast<AttributeNode *>(node);
    const GfVec4f fallback = GetFallback(params, GfVec4f(0.0f, 0.0f, 0.0f, 1.0f));
    attribute->set_missing(make_float3(fallback[0], fallback[1], fallback[2]));
    attribute->set_missing_alpha(fallback[3]);
  }
};

/* Karma Hair (kma_hair, Chiang et al.) as the Chiang Principled Hair BSDF with melanin, with
 * Karma's defaults. Base Color is a dye like the Principled Hair Tint, Base darkens it, Thickness
 * Scale scales the absorption, Diffuse mixes in a Diffuse BSDF and Tint weights the result.
 * Absorption and the hair ID and seed have no Cycles input and are ignored, Cycles' TT/TRT
 * roughness scales are fixed at Karma's defaults. */
class UsdToCyclesKarmaHair : public UsdToCyclesMapping {
 public:
  using UsdToCyclesMapping::UsdToCyclesMapping;

  void initialize(ShaderNode *node) const override
  {
    PrincipledHairBsdfNode *hair = static_cast<PrincipledHairBsdfNode *>(node);
    hair->set_model(NODE_PRINCIPLED_HAIR_CHIANG);
    hair->set_parametrization(NODE_PRINCIPLED_HAIR_PIGMENT_CONCENTRATION);
    hair->set_melanin(0.4f);
    hair->set_melanin_redness(0.1f);
    hair->set_offset(0.03f * M_PI_2_F);
  }

  bool isInputUsed(const TfToken &name,
                   const HdMaterialNodeParameterContainerSchema &params) const override
  {
    if (name == CyclesMaterialTokens->roughnessAzimuthal) {
      return GetParameter(params, CyclesMaterialTokens->roughnessAnisotropic, 0) != 0;
    }
    return hasParameterMapping(name);
  }

  std::string parameterName(const TfToken &name,
                            const ShaderInput *inputConnection,
                            VtValue *value) const override
  {
    /* Shift -1..1 is the cuticle angle -90..90 degrees, Offset is in radians. */
    if (value && name == CyclesMaterialTokens->shift) {
      *value = VtValue(VtValue::Cast<float>(*value).GetWithDefault(0.03f) * M_PI_2_F);
    }
    return UsdToCyclesMapping::parameterName(name, inputConnection, value);
  }

  void finalize(ShaderGraph *graph,
                ShaderNode *&node,
                const HdMaterialNodeParameterContainerSchema &params) const override
  {
    PrincipledHairBsdfNode *hair = static_cast<PrincipledHairBsdfNode *>(node);
    /* Without Roughness Anisotropic, Roughness is also the azimuthal roughness. */
    if (GetParameter(params, CyclesMaterialTokens->roughnessAnisotropic, 0) == 0) {
      hair->set_radial_roughness(hair->get_roughness());
      if (ShaderOutput *link = hair->input("Roughness")->link) {
        graph->connect(link, hair->input("Radial Roughness"));
      }
    }
    if (GetParameter(params, CyclesMaterialTokens->roughness_advanced, 0) != 0) {
      TF_WARN("Karma Hair TT and TRT roughness scales are fixed at 0.25 and 4 in Cycles");
    }

    /* Base scales the dye towards black. Thickness Scale k scales the absorption, which is
     * -log(1 - melanin) for the melanin and log(dye)^2 / f(roughness) for the dye: melanin
     * becomes 1 - (1 - melanin)^k, the dye dye^sqrt(k).
     * shortcut: authored values only, Base and Thickness Scale leave a linked Base Color or
     * Melanin as is; add math nodes when an asset needs it. */
    const float base = GetParameter(params, CyclesMaterialTokens->base, 1.0f);
    const float thickness = std::max(
        GetParameter(params, CyclesMaterialTokens->thicknessScale, 1.0f), 0.0f);
    if (!hair->input("Tint")->link) {
      const float3 dye = max(hair->get_tint() * base, zero_float3());
      const float e = sqrtf(thickness);
      hair->set_tint(make_float3(powf(dye.x, e), powf(dye.y, e), powf(dye.z, e)));
    }
    if (!hair->input("Melanin")->link) {
      const float melanin = std::clamp(hair->get_melanin(), 0.0f, 1.0f);
      hair->set_melanin(1.0f - powf(1.0f - melanin, thickness));
    }

    /* Diffuse moves the hair towards a Diffuse BSDF of Diffuse Color.
     * shortcut: authored values only for Diffuse, Diffuse Color and Tint, like Base. */
    ShaderOutput *closure = hair->output("BSDF");
    const float diffuse = std::clamp(
        GetParameter(params, CyclesMaterialTokens->diffuse, 0.0f), 0.0f, 1.0f);
    if (diffuse > 0.0f) {
      const GfVec3f color = GetParameter(
          params, CyclesMaterialTokens->diffuseColor, GfVec3f(1.0f));
      DiffuseBsdfNode *bsdf = graph->create_node<DiffuseBsdfNode>();
      bsdf->set_color(make_float3(color[0], color[1], color[2]));
      MixClosureNode *mix = graph->create_node<MixClosureNode>();
      mix->set_fac(diffuse);
      graph->connect(closure, mix->input("Closure1"));
      graph->connect(bsdf->output("BSDF"), mix->input("Closure2"));
      closure = mix->output("Closure");
      node = mix;
    }

    /* Tint multiplies the lighting.
     * shortcut: closures only have scalar weights in Cycles, so the average of Tint weights the
     * closure and values above 1 are clamped; a colored Tint only darkens. */
    const GfVec3f tint = GetParameter(params, CyclesMaterialTokens->tint, GfVec3f(1.0f));
    const float weight = std::clamp((tint[0] + tint[1] + tint[2]) / 3.0f, 0.0f, 1.0f);
    if (weight < 1.0f) {
      MixClosureNode *mix = graph->create_node<MixClosureNode>();
      mix->set_fac(weight);
      graph->connect(closure, mix->input("Closure2"));
      node = mix;
    }
  }
};

namespace {

class UsdToCycles {
  const UsdToCyclesPreviewSurface UsdPreviewSurface = {
      "principled_bsdf",
      {
          {TfToken("diffuseColor"), ustring("base_color")},
          {TfToken("emissiveColor"), ustring("emission_color")},
          {CyclesMaterialTokens->specularColor, ustring("specular_tint")},
          {TfToken("clearcoat"), ustring("coat_weight")},
          {TfToken("clearcoatRoughness"), ustring("coat_roughness")},
          {TfToken("opacity"), ustring("alpha")},
          /* occlusion */
          /* displacement */
      }};
  const UsdToCyclesTexture UsdUVTexture = {
      "image_texture",
      {
          {CyclesMaterialTokens->st, ustring("vector")},
          {CyclesMaterialTokens->wrapS, ustring("extension")},
          {CyclesMaterialTokens->wrapT, ustring("extension")},
          {TfToken("file"), ustring("filename")},
          {TfToken("sourceColorSpace"), ustring("colorspace")},
      }};
  const UsdToCyclesPrimvarReader UsdPrimvarReader = {
      "attribute", {{TfToken("varname"), ustring("attribute")}}};
  const UsdToCyclesKarmaHair KarmaHair = {
      "principled_hair_bsdf",
      {
          {TfToken("baseColor"), ustring("tint")},
          {TfToken("melanin"), ustring("melanin")},
          {TfToken("melaninRedness"), ustring("melanin_redness")},
          {TfToken("melaninRandomize"), ustring("random_color")},
          {CyclesMaterialTokens->shift, ustring("offset")},
          {CyclesMaterialTokens->ior, ustring("ior")},
          {TfToken("roughness"), ustring("roughness")},
          {CyclesMaterialTokens->roughnessAzimuthal, ustring("radial_roughness")},
          {TfToken("roughnessRandomize"), ustring("random_roughness")},
          {TfToken("coat"), ustring("coat")},
      }};

 public:
  const UsdToCyclesMapping *findUsd(const TfToken &usdNodeType)
  {
    if (usdNodeType == CyclesMaterialTokens->UsdPreviewSurface) {
      return &UsdPreviewSurface;
    }
    if (usdNodeType == CyclesMaterialTokens->UsdUVTexture) {
      return &UsdUVTexture;
    }
    if (usdNodeType == CyclesMaterialTokens->kma_hair) {
      return &KarmaHair;
    }
    if (usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float2 ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float3 ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float4 ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_int ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_normal ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_point ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_vector)
    {
      return &UsdPrimvarReader;
    }

    return nullptr;
  }
  const UsdToCyclesMapping *findCycles(const ustring & /*cyclesNodeType*/)
  {
    return nullptr;
  }
};
TfStaticData<UsdToCycles> sUsdToCyles;

/* Socket name for comparison, ignoring case, spaces, underscores and dots. Houdini parameter names
 * can't contain dots, so e.g. "tex_mapping.scale" becomes "tex_mapping_scale". */
static std::string NormalizeName(const std::string &name)
{
  std::string result;
  for (char c : name) {
    if (c != '_' && c != ' ' && c != '.') {
      result += std::tolower(static_cast<unsigned char>(c));
    }
  }
  if (result == "vectorin" || result == "vectorout") {
    return "vector";
  }
  return result;
}

/* Find a socket by either its identifier (e.g. "emission", as used by the Houdini shader nodes)
 * or its UI name (e.g. "Emission", as exported by Blender). Identifiers take precedence, as UI
 * names are ambiguous: e.g. texture nodes have both a "scale" input and a "tex_mapping.scale"
 * input with UI name "Scale". */
static bool SocketNameMatches(const SocketType &socket,
                              const std::string &normName,
                              const bool use_ui_name)
{
  return NormalizeName(use_ui_name ? socket.ui_name.string() : socket.name.string()) == normName;
}

static const SocketType *FindSocketType(const NodeType *type, const std::string &name)
{
  const std::string normName = NormalizeName(name);
  for (const bool use_ui_name : {false, true}) {
    for (const SocketType &socket : type->inputs) {
      if (SocketNameMatches(socket, normName, use_ui_name)) {
        return &socket;
      }
    }
  }
  return nullptr;
}

static ShaderInput *FindInput(ShaderNode *node, const std::string &name)
{
  const std::string normName = NormalizeName(name);
  for (const bool use_ui_name : {false, true}) {
    for (ShaderInput *in : node->inputs) {
      if (SocketNameMatches(in->socket_type, normName, use_ui_name)) {
        return in;
      }
    }
  }
  return nullptr;
}

static ShaderOutput *FindOutput(ShaderNode *node, const std::string &name)
{
  const std::string normName = NormalizeName(name);
  for (const bool use_ui_name : {false, true}) {
    for (ShaderOutput *out : node->outputs) {
      if (SocketNameMatches(out->socket_type, normName, use_ui_name)) {
        return out;
      }
    }
  }
  return nullptr;
}

}  // namespace

/* Settings like Blender's Material Properties > Settings, from "config:cycles:<socket>"
 * attributes of the material prim (Houdini's Cycles Material Builder authors them). Unset
 * settings get the Cycles defaults, which are Blender's, except that the displacement of
 * non-Cycles networks (MaterialX, UsdPreviewSurface) displaces the surface as in Karma (with
 * bump mapping for details finer than the dicing). */
static void ApplyMaterialSettings(Shader *shader,
                                  const HdMaterialNetworkSchema &network,
                                  const bool cyclesNetwork)
{
  static const char *const settings[] = {"pass_id",
                                         "volume_stack_priority",
                                         "displacement_method",
                                         "emission_sampling_method",
                                         "use_transparent_shadow",
                                         "use_bump_map_correction",
                                         "volume_sampling_method",
                                         "volume_interpolation_method",
                                         "volume_step_rate"};
  const HdSampledDataSourceContainerSchema config = network.GetConfig();
  for (const char *name : settings) {
    const SocketType *socket = shader->type->find_input(ustring(name));
    const HdSampledDataSourceHandle value = config ?
                                                config.Get(TfToken(string("cycles:") + name)) :
                                                nullptr;
    if (value) {
      SetNodeValue(shader, *socket, value->GetValue(0.0f));
    }
    else if (!cyclesNetwork && socket->name == "displacement_method") {
      shader->set_displacement_method(DISPLACE_BOTH);
    }
    else {
      shader->set_default_value(*socket);
    }
  }
}

void HdCyclesMaterial::AddIdAOVs(ShaderGraph *graph)
{
  /* Instance and prim ID AOVs are provided for every network, to make picking work. They are
   * object attributes (HdCyclesGeometry::InitializeInstance). */
  for (const TfToken &aov : {HdAovTokens->instanceId, HdAovTokens->primId}) {
    const ustring name(aov.GetString());

    OutputAOVNode *aovNode = graph->create_node<OutputAOVNode>();
    aovNode->set_name(name);

    AttributeNode *attributeNode = graph->create_node<AttributeNode>();
    attributeNode->set_attribute(name);

    graph->connect(attributeNode->output("Fac"), aovNode->input("Value"));
  }
}

void HdCyclesMaterial::AddPrimvarAOVs(ShaderGraph *graph,
                                      const std::vector<std::pair<string, string>> &aovs)
{
  for (const auto &[passName, primvar] : aovs) {
    OutputAOVNode *aovNode = graph->create_node<OutputAOVNode>();
    aovNode->set_name(ustring(passName));

    AttributeNode *attributeNode = graph->create_node<AttributeNode>();
    attributeNode->set_attribute(ustring(primvar));

    graph->connect(attributeNode->output("Color"), aovNode->input("Color"));
    graph->connect(attributeNode->output("Fac"), aovNode->input("Value"));
  }
}

/* Image textures with "<UDIM>" in the file name (native and MaterialX) read the tiles found next
 * to it on disk. shortcut: only the <UDIM> token, add <UVTILE> when a scene uses it. */
void HdCyclesMaterial::SetUdimTiles(ShaderGraph *graph)
{
#ifdef WITH_HOUDINI_IMAGES
  /* Houdini's .rat textures, as files OpenImageIO reads (before the UDIM tiles are listed). */
  for (ShaderNode *node : graph->nodes) {
    if (node->type == ImageTextureNode::get_node_type()) {
      ImageTextureNode *image = static_cast<ImageTextureNode *>(node);
      image->set_filename(ustring(HdCycles::ResolveHoudiniImage(image->get_filename().string())));
    }
    else if (node->type == EnvironmentTextureNode::get_node_type()) {
      EnvironmentTextureNode *image = static_cast<EnvironmentTextureNode *>(node);
      image->set_filename(ustring(HdCycles::ResolveHoudiniImage(image->get_filename().string())));
    }
  }
#endif
  for (ShaderNode *node : graph->nodes) {
    if (node->type != ImageTextureNode::get_node_type()) {
      continue;
    }
    ImageTextureNode *image = static_cast<ImageTextureNode *>(node);
    const string filename = image->get_filename().string();
    /* Mari's <UDIM> (1001, 1002, ...) or Mudbox's <UVTILE> (u1_v1, u2_v1, ...) tiles. */
    const bool uvtile = filename.find("<UVTILE>") != string::npos;
    const string token = uvtile ? "<UVTILE>" : "<UDIM>";
    const size_t udim = filename.find(token);
    if (udim == string::npos) {
      continue;
    }
    /* Windows file names are case insensitive. */
    auto fold = [](const string &name) {
#ifdef _WIN32
      return string_to_lower(name);
#else
      return name;
#endif
    };
    /* The prefix is empty for a file name that starts with the token. */
    const size_t slash = filename.find_last_of("/\\", udim);
    const size_t start = (slash == string::npos) ? 0 : slash + 1;
    const string prefix = fold(filename.substr(start, udim - start));
    const string suffix = fold(filename.substr(udim + token.size()));
    std::vector<string> files;
    OIIO::Filesystem::get_directory_entries(path_dirname(filename), files);

    array<int> tiles;
    for (const string &file : files) {
      const string name = fold(path_filename(file));
      if (uvtile) {
        /* u<U>_v<V>: tile 1001 + (U - 1) + 10 (V - 1), U in 1..10. */
        if (name.size() <= prefix.size() + suffix.size() || !string_startswith(name, prefix) ||
            !string_endswith(name, suffix))
        {
          continue;
        }
        const string middle = name.substr(prefix.size(),
                                          name.size() - prefix.size() - suffix.size());
        int u = 0;
        int v = 0;
        char end = 0;
        if (std::sscanf(middle.c_str(), "u%d_v%d%c", &u, &v, &end) == 2 && u >= 1 && u <= 10 &&
            v >= 1)
        {
          tiles.push_back_slow(1001 + (u - 1) + 10 * (v - 1));
        }
        continue;
      }
      if (name.size() != prefix.size() + 4 + suffix.size() || !string_startswith(name, prefix) ||
          !string_endswith(name, suffix))
      {
        continue;
      }
      const string number = name.substr(prefix.size(), 4);
      if (std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
          number >= "1001")
      {
        tiles.push_back_slow(std::stoi(number));
      }
    }
    /* Keep the tiles a native network authored when none are found. */
    if (!tiles.empty()) {
      std::sort(tiles.begin(), tiles.end());
      image->set_tiles(tiles);
    }
  }
}

HdCyclesMaterial::HdCyclesMaterial(const SdfPath &sprimId) : HdMaterial(sprimId) {}

HdCyclesMaterial::~HdCyclesMaterial() = default;

HdDirtyBits HdCyclesMaterial::GetInitialDirtyBitsMask() const
{
  return DirtyBits::DirtyResource | DirtyBits::DirtyParams;
}

/* Cycles' sky and environment textures have their pole on +Z and their center on +X, USD dome
 * lights on their +Y and +Z, like the dome light textures (light.cpp): look up light space
 * (x, y, z) as (z, x, y), in the texture mapping, so that Cycles still samples the sun of a sky
 * texture directly (LightManager::device_update_background).
 * Texture Mapping parameters set on such a node apply after this, in the texture's space (like
 * Blender): the rotation then goes into a Mapping node before the node instead. */
static float3 EulerAngles(const Transform &r)
{
  /* Euler angles of transform_euler() (Z * Y * X) and the Mapping node. */
  return make_float3(
      atan2f(r.z.y, r.z.z), asinf(clamp(-r.z.x, -1.0f, 1.0f)), atan2f(r.y.x, r.x.x));
}

static void OrientWorldTextures(ShaderGraph *graph, const Transform &lightRotation)
{
  const Transform swap = make_transform(0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0);
  /* swap * inverse(light) = r * swap. */
  const Transform r = swap * transform_inverse(lightRotation) * transform_inverse(swap);
  const float3 euler = EulerAngles(r);
  const vector<ShaderNode *> nodes(graph->nodes.begin(), graph->nodes.end());
  for (ShaderNode *node : nodes) {
    if (node->type != SkyTextureNode::get_node_type() &&
        node->type != EnvironmentTextureNode::get_node_type())
    {
      continue;
    }
    TextureMapping &mapping = static_cast<TextureNode *>(node)->tex_mapping;
    if (!mapping.skip()) {
      /* The node's default vector (sky: Generated, environment: Position, both the direction in
       * the world) through the rotation. Sky sun sampling then falls back to the map. */
      ShaderInput *vectorIn = node->input("Vector");
      ShaderOutput *source = vectorIn->link;
      if (source) {
        graph->disconnect(vectorIn);
      }
      else if (vectorIn->flags() & SocketType::LINK_POSITION) {
        source = graph->create_node<GeometryNode>()->output("Position");
      }
      else {
        source = graph->create_node<TextureCoordinateNode>()->output("Generated");
      }
      MappingNode *orient = graph->create_node<MappingNode>();
      orient->set_mapping_type(NODE_MAPPING_TYPE_VECTOR);
      orient->set_rotation(EulerAngles(r * swap));
      graph->connect(source, orient->input("Vector"));
      graph->connect(orient->output("Vector"), vectorIn);
      continue;
    }
    mapping.type = TextureMapping::VECTOR;
    mapping.translation = zero_float3();
    mapping.rotation = euler;
    mapping.scale = one_float3();
    mapping.x_mapping = TextureMapping::Z;
    mapping.y_mapping = TextureMapping::X;
    mapping.z_mapping = TextureMapping::Y;
    mapping.use_minmax = false;
  }
}

void HdCyclesMaterial::Sync(HdSceneDelegate *sceneDelegate,
                            HdRenderParam *renderParam,
                            HdDirtyBits *dirtyBits)
{
  if (*dirtyBits == DirtyBits::Clean) {
    return;
  }

  const bool dirtyParams = (*dirtyBits & DirtyBits::DirtyParams);
  const bool dirtyResource = (*dirtyBits & DirtyBits::DirtyResource);

  const SdfPath &id = GetId();
  HdCyclesSession *const session = static_cast<HdCyclesSession *>(renderParam);

  if (dirtyResource || dirtyParams) {
    /* Prefer cycles network if it exists, then MaterialX, otherwise use universal network. */
    const auto getNetwork = [](const HdMaterialSchema &matSchema) {
      HdMaterialNetworkSchema network = matSchema.GetMaterialNetwork(CyclesMaterialTokens->cycles);
#ifdef WITH_HYDRA_MATERIALX
      for (const TfToken &context : MaterialXRenderContexts()) {
        if (!network) {
          network = matSchema.GetMaterialNetwork(context);
        }
      }
#endif
      if (!network) {
        network = matSchema.GetMaterialNetwork();
      }
      return network;
    };

    const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
    HdMaterialSchema matSchema = HdMaterialSchema::GetFromParent(prim.dataSource);
    HdMaterialNetworkSchema network = getNetwork(matSchema);

    const SceneLock lock(renderParam);

    const auto buildGraph = [&](const SdfPath &path,
                                const HdMaterialNetworkSchema &pathNetwork,
                                const HdContainerDataSourceHandle &primDs) {
      unique_ptr<ShaderGraph> graph;
      if (!pathNetwork) {
        TF_WARN("Could not get a material network for %s.", path.GetText());
        return graph;
      }
#ifdef WITH_HYDRA_MATERIALX
      if (IsMaterialXNetwork(pathNetwork)) {
        /* The node map is only for native networks. */
        _nodes.clear();
        graph = BuildMaterialXShaderGraph(path, pathNetwork, primDs);
      }
      else
#endif
      {
        graph = BuildShaderGraph(path, pathNetwork, primDs, lock.scene);
      }
      if (!graph) {
        TF_WARN("Could not build a shader graph for %s.", path.GetText());
      }
      return graph;
    };
    unique_ptr<ShaderGraph> graph = buildGraph(id, network, prim.dataSource);

    if (!graph) {
      /* Default shader if translation failed, so something is visible. */
      graph = make_unique<ShaderGraph>();
      PrincipledBsdfNode *bsdf = graph->create_node<PrincipledBsdfNode>();
      graph->connect(bsdf->output("BSDF"), graph->output()->input("Surface"));
    }

    const HdCyclesSession::WorldMaterial *const world = session->GetWorldOf(id.GetString());
    const Transform *const worldRotation = world ? &world->rotation : nullptr;

    /* The cycles:material_override render setting: surfaces render as clay or as another
     * material, like Blender's View Layer Material Override. World materials and materials
     * without a surface (volumes) keep theirs. */
    const std::string materialOverride = session->GetMaterialOverride();
    if (materialOverride == id.GetString()) {
      /* The other materials build this graph: rebuild them when it changed (a sync without a
       * change, e.g. of an unchanged time sample, rebuilds none), not in the sync of the resync
       * this requests, which would request another one. */
      const size_t hash = HdDataSourceHash(matSchema.GetContainer(), 0.0f, 0.0f);
      if (hash != _overrideHash && !session->materialsResyncing) {
        session->RequestResync(true, false);
      }
      /* Prims without a material too (in the resync after an override change, which reset the
       * default surface). Its own graph: a graph belongs to one shader. */
      if (hash != _overrideHash || session->materialsResyncing) {
        session->UpdateDefaultSurface(buildGraph(id, network, prim.dataSource));
      }
      _overrideHash = hash;
    }
    ShaderInput *const surface = graph->output()->input("Surface");
    if (!materialOverride.empty() && materialOverride != id.GetString() && !worldRotation &&
        surface->link)
    {
      if (materialOverride == "clay") {
        /* Only the surface: displacement and volumes keep the shapes of the beauty render. */
        graph->disconnect(surface);
        graph->connect(graph->create_node<DiffuseBsdfNode>()->output("BSDF"), surface);
      }
      else if (SdfPath::IsValidPathString(materialOverride)) {
        const SdfPath overrideId(materialOverride);
        const HdSceneIndexPrim overridePrim = GetPrim(sceneDelegate, overrideId);
        if (overridePrim.primType == HdPrimTypeTokens->material) {
          const HdMaterialSchema overrideSchema = HdMaterialSchema::GetFromParent(
              overridePrim.dataSource);
          const HdMaterialNetworkSchema overrideNetwork = getNetwork(overrideSchema);
          if (unique_ptr<ShaderGraph> overrideGraph = buildGraph(
                  overrideId, overrideNetwork, overridePrim.dataSource))
          {
            graph = std::move(overrideGraph);
            matSchema = overrideSchema;
            network = overrideNetwork;
          }
        }
      }
    }

    SetUdimTiles(graph.get());
    if (worldRotation) {
      OrientWorldTextures(graph.get(), *worldRotation);
      HdCyclesSession::AdjustWorld(graph.get(), exp2f(world->exposure), world->saturation);
      session->ExpandDomeTextures(graph.get(), *world);
    }
    AddIdAOVs(graph.get());
    AddPrimvarAOVs(graph.get(), static_cast<HdCyclesSession *>(renderParam)->GetPrimvarAovs());

    const bool existed = _shader != nullptr;
    if (!_shader) {
      _shader = static_cast<HdCyclesSession *>(renderParam)->TakeShader(lock.scene);
      // Cryptomatte material name
      _shader->name = id.GetString();
      /* A dome light may use the material as world since before it existed. */
      if (worldRotation) {
        static_cast<HdCyclesSession *>(renderParam)->lightsChanged = true;
      }
    }

    const AttributeRequestSet attributes = _shader->attributes;
    /* Before the graph: set_graph() hashes the displacement for the displacement method. */
    ApplyMaterialSettings(
        _shader, network, bool(matSchema.GetMaterialNetwork(CyclesMaterialTokens->cycles)));
    if (worldRotation) {
      static_cast<HdCyclesSession *>(renderParam)->SetWorldSource(_shader, *graph);
    }
    _shader->set_graph(std::move(graph));
    _shader->tag_update(lock.scene);

    /* Geometry only exports the primvars that shaders read when it is synced. */
    if (existed && _shader->attributes.modified(attributes)) {
      static_cast<HdCyclesSession *>(renderParam)->RequestResync(false, true);
    }
  }

  *dirtyBits = DirtyBits::Clean;
}

void HdCyclesMaterial::UpdateParameters(NodeDesc &nodeDesc,
                                        HdMaterialNodeParameterContainerSchema params,
                                        const SdfPath &nodePath)
{
  for (const TfToken &paramName : params.GetNames()) {
    auto valueDs = params.Get(paramName).GetValue();
    if (!valueDs) {
      continue;
    }
    if (paramName == kScriptFilepath && nodeDesc.node->special_type == SHADER_SPECIAL_TYPE_OSL) {
      continue; /* Used to create the node. */
    }
    if (IsRaycastAttribute(nodeDesc.node, paramName)) {
      continue; /* Used to create the outputs. */
    }
    VtValue value = valueDs->GetValue(0.0f);

    /* See if the parameter name is in USDPreviewSurface terms, and needs to be converted .*/
    const UsdToCyclesMapping *inputMapping = nodeDesc.mapping;
    if (inputMapping && !inputMapping->isInputUsed(paramName, params)) {
      continue;
    }
    const std::string inputName = inputMapping ?
                                      inputMapping->parameterName(paramName, nullptr, &value) :
                                      paramName.GetString();

    /* Find the input to write the parameter value to. */
    const SocketType *input = FindSocketType(nodeDesc.node->type, inputName);

    if (!input) {
      if (inputMapping) {
        if (inputMapping->hasParameterMapping(paramName)) {
          TF_WARN("USD input '%s' on node '%s' maps to unavailable Cycles input '%s' on '%s'",
                  paramName.GetText(),
                  nodePath.GetText(),
                  inputName.c_str(),
                  nodeDesc.node->name.c_str());
        }
        else {
          TF_WARN("Unsupported USD input '%s' on node '%s' ('%s'); value ignored",
                  paramName.GetText(),
                  nodePath.GetText(),
                  nodeDesc.node->name.c_str());
        }
      }
      else {
        TF_WARN("Could not find parameter '%s' on node '%s' ('%s')",
                paramName.GetText(),
                nodePath.GetText(),
                nodeDesc.node->name.c_str());
      }
      continue;
    }

    SetNodeValue(nodeDesc.node, *input, value);

    /* UsdImaging passes UsdUVTexture's sourceColorSpace as the color space of the file
     * parameter, like the colorSpace metadata of asset inputs. Other parameters can have one too
     * (inherited, or empty), which is not the image's. An authored colorspace wins. */
    if (input->name == "filename" && !params.Get(TfToken("colorspace")).GetValue()) {
      const HdTokenDataSourceHandle colorSpaceDs = params.Get(paramName).GetColorSpace();
      const TfToken colorSpaceName = colorSpaceDs ? colorSpaceDs->GetTypedValue(0.0f) : TfToken();
      const SocketType *colorspace = nodeDesc.node->type->find_input(ustring("colorspace"));
      if (!colorSpaceName.IsEmpty() && colorspace) {
        nodeDesc.node->set(*colorspace, ustring(colorSpaceName.GetString()));
        input = colorspace;
      }
    }

    /* An empty color space (and UsdUVTexture's "auto") is automatic detection, not a color
     * space of that name. UsdUVTexture's "sRGB" is Cycles' built-in sRGB, which every OCIO
     * config has (Houdini's has no color space of that name), also for float images. */
    if (input->type == SocketType::STRING && input->name == "colorspace") {
      const ustring colorspace = nodeDesc.node->get_string(*input);
      if (colorspace.empty() || colorspace == "auto") {
        nodeDesc.node->set(*input, u_colorspace_auto);
      }
      else if (colorspace == "sRGB") {
        nodeDesc.node->set(*input, u_colorspace_srgb);
      }
      else if (colorspace == "raw") {
        nodeDesc.node->set(*input, u_colorspace_data);
      }
    }
  }
}

void HdCyclesMaterial::UpdateConnections(
    NodeDesc &nodeDesc,
    HdMaterialNodeSchema nodeSchema,
    const SdfPath &nodePath,
    ShaderGraph *shaderGraph,
    const std::function<bool(const TfToken &, const TfToken &, ShaderInput *)> &linkOther)
{
  HdMaterialConnectionVectorContainerSchema conns = nodeSchema.GetInputConnections();
  for (const TfToken &dstSocketName : conns.GetNames()) {
    HdMaterialConnectionVectorSchema connVec = conns.Get(dstSocketName);
    const size_t count = connVec.GetNumElements();
    if (count == 0) {
      continue;
    }

    /* The shader passed through an AOV Output node is resolved at the terminals. */
    if (IsAovPassThrough(nodeDesc.node, dstSocketName)) {
      continue;
    }

    const UsdToCyclesMapping *inputMapping = nodeDesc.mapping;
    if (inputMapping && !inputMapping->isInputUsed(dstSocketName, nodeSchema.GetParameters())) {
      continue;
    }
    const std::string inputName = inputMapping ?
                                      inputMapping->parameterName(dstSocketName, nullptr) :
                                      dstSocketName.GetString();

    /* Find the input to connect to on the passed in node. */
    ShaderInput *input = FindInput(nodeDesc.node, inputName);

    if (!input) {
      if (inputMapping) {
        if (inputMapping->hasParameterMapping(dstSocketName)) {
          TF_WARN("USD input '%s' on node '%s' maps to unavailable Cycles input '%s' on '%s'",
                  dstSocketName.GetText(),
                  nodePath.GetText(),
                  inputName.c_str(),
                  nodeDesc.node->name.c_str());
        }
        else {
          TF_WARN("Unsupported USD input '%s' on node '%s' ('%s'); connection ignored",
                  dstSocketName.GetText(),
                  nodePath.GetText(),
                  nodeDesc.node->name.c_str());
        }
      }
      else {
        TF_WARN("Ignoring connection on '%s.%s', input '%s' was not found",
                nodePath.GetText(),
                dstSocketName.GetText(),
                inputName.c_str());
      }
      continue;
    }

    /* USD allows N connections per input (MaterialX <switch>, <combine>, struct
     * inputs etc). Cycles inputs are single-connection, and the right lowering
     * depends on the node type, so just take the first and warn. */
    if (count > 1) {
      TF_WARN(
          "Ignoring multiple connections to '%s.%s'", nodePath.GetText(), dstSocketName.GetText());
    }

    HdMaterialConnectionSchema connSchema = connVec.GetElement(0);
    const SdfPath upstreamNodePath =
        connSchema.GetUpstreamNodePath() ?
            MaterialNodeNameToSdfPath(connSchema.GetUpstreamNodePath()->GetTypedValue(0.0f)) :
            SdfPath();
    const TfToken upstreamOutputName = connSchema.GetUpstreamNodeOutputName() ?
                                           connSchema.GetUpstreamNodeOutputName()->GetTypedValue(
                                               0.0f) :
                                           TfToken();

    const auto srcNodeIt = _nodes.find(upstreamNodePath);
    if (srcNodeIt == _nodes.end() && connSchema.GetUpstreamNodePath() &&
        linkOther(
            connSchema.GetUpstreamNodePath()->GetTypedValue(0.0f), upstreamOutputName, input))
    {
      continue;
    }
    if (srcNodeIt == _nodes.end()) {
      TF_WARN("Ignoring connection from '%s.%s' to '%s.%s', node '%s' was not found",
              upstreamNodePath.GetText(),
              upstreamOutputName.GetText(),
              nodePath.GetText(),
              dstSocketName.GetText(),
              upstreamNodePath.GetText());
      continue;
    }

    const UsdToCyclesMapping *outputMapping = srcNodeIt->second.mapping;
    const std::string outputName = outputMapping ?
                                       outputMapping->parameterName(upstreamOutputName, input) :
                                       upstreamOutputName.GetString();

    ShaderOutput *output = FindOutput(srcNodeIt->second.node, outputName);

    if (!output) {
      TF_WARN("Ignoring connection from '%s.%s' to '%s.%s', output '%s' was not found",
              upstreamNodePath.GetText(),
              upstreamOutputName.GetText(),
              nodePath.GetText(),
              dstSocketName.GetText(),
              outputName.c_str());
      continue;
    }

    shaderGraph->connect(output, input);
  }
}

unique_ptr<ShaderGraph> HdCyclesMaterial::BuildShaderGraph(
    const SdfPath &materialId,
    HdMaterialNetworkSchema network,
    const HdContainerDataSourceHandle &primContainer,
    Scene *scene)
{
  _nodes.clear();

  unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();
  /* Register the delegate's node types for NodeType::find. */
  ParallaxOcclusionNode::get_node_type();
  DomeTextureNode::get_node_type();

  HdMaterialNodeContainerSchema nodes = network.GetNodes();

  /* Iterate all the nodes first and build a complete but unconnected graph with parameters set. */
  std::unordered_map<SdfPath, TfToken, SdfPath::Hash> nodeNames;
  std::set<TfToken> materialXNodes;
  for (const TfToken &nodeName : nodes.GetNames()) {
    HdMaterialNodeSchema nodeSchema = nodes.Get(nodeName);
    const SdfPath nodePath = MaterialNodeNameToSdfPath(nodeName);
    nodeNames[nodePath] = nodeName;

    NodeDesc nodeDesc = {};

    const auto nodeIt = _nodes.find(nodePath);
    /* Create new node only if it does not exist yet. */
    if (nodeIt != _nodes.end()) {
      nodeDesc = nodeIt->second;
    }
    else {
      /* E.g. cycles_principled_bsdf or UsdPreviewSurface. */
      const TfToken nodeTypeIdToken = nodeSchema.GetNodeIdentifier() ?
                                          nodeSchema.GetNodeIdentifier()->GetTypedValue(0.0f) :
                                          TfToken();
      const std::string &nodeTypeId = nodeTypeIdToken.GetString();
      /* MaterialX nodes in a network of other nodes (a kma_hair in a Karma Material Builder)
       * are translated where they are linked. */
      if (nodeTypeId.starts_with("ND_")) {
        materialXNodes.insert(nodeName);
        continue;
      }

      ustring cyclesType(nodeTypeId);
      const bool isNativeNode = nodeTypeId.starts_with("cycles_") ||
                                nodeTypeId.starts_with("cycles:");
      if (isNativeNode) {
        /* Native Cycles note embedded in USDShade. */
        cyclesType = nodeTypeId.substr(strlen("cycles_"));
        nodeDesc.mapping = sUsdToCyles->findCycles(cyclesType);
      }
      else {
        /* Check if any remapping is needed (e.g. for USDPreviewSurface to Cycles nodes). */
        nodeDesc.mapping = sUsdToCyles->findUsd(nodeTypeIdToken);
        if (nodeDesc.mapping) {
          cyclesType = nodeDesc.mapping->nodeType();
        }
      }

      if (isNativeNode && cyclesType == "script") {
        nodeDesc.node = CreateScriptNode(graph.get(), scene, nodeSchema.GetParameters(), nodePath);
        if (!nodeDesc.node) {
          continue;
        }
        _nodes.emplace(nodePath, nodeDesc);
      }
      /* If it's a native Cycles' node-type, just do the lookup now. */
      else if (const NodeType *nodeType = NodeType::find(cyclesType)) {
        nodeDesc.node = graph->create_node(nodeType);
        AddRaycastAttributeOutputs(nodeDesc.node, nodeSchema.GetParameters());
        if (nodeDesc.mapping) {
          nodeDesc.mapping->initialize(nodeDesc.node);
        }
        /* Image nodes start with scene linear, detect the color space unless one is set. */
        if (const SocketType *colorspace = nodeType->find_input(ustring("colorspace"))) {
          nodeDesc.node->set(*colorspace, u_colorspace_auto);
        }
        if (isNativeNode) {
          GetShaderNodeDefaults().Apply(nodeDesc.node, cyclesType.string());
        }
        _nodes.emplace(nodePath, nodeDesc);
      }
      else if (ShaderNode *oslNode = CreateSourceAssetNode(
                   graph.get(), scene, nodeSchema.GetNodeTypeInfo(), nodePath))
      {
        nodeDesc.node = oslNode;
        _nodes.emplace(nodePath, nodeDesc);
      }
      else {
        TF_RUNTIME_ERROR("Could not create node '%s'", nodePath.GetText());
        continue;
      }
    }

    UpdateParameters(nodeDesc, nodeSchema.GetParameters(), nodePath);
  }

#ifdef WITH_HYDRA_MATERIALX
  std::shared_ptr<MaterialXLinkCache> materialXCache;
#endif
  const auto linkMaterialX =
      [&](const TfToken &upstreamNode, const TfToken &output, ShaderInput *input) {
#ifdef WITH_HYDRA_MATERIALX
        return materialXNodes.count(upstreamNode) && LinkMaterialXNode(graph.get(),
                                                                       materialId,
                                                                       network,
                                                                       primContainer,
                                                                       upstreamNode,
                                                                       output,
                                                                       input,
                                                                       materialXCache);
#else
        (void)upstreamNode, (void)output, (void)input, (void)primContainer, (void)materialId;
        return false;
#endif
      };

  /* Now that all nodes have been constructed, iterate the network again and build up any
   * connections between nodes. */
  for (const TfToken &nodeName : nodes.GetNames()) {
    const SdfPath nodePath = MaterialNodeNameToSdfPath(nodeName);

    const auto nodeIt = _nodes.find(nodePath);
    if (materialXNodes.count(nodeName)) {
      continue;
    }
    if (nodeIt == _nodes.end()) {
      TF_RUNTIME_ERROR("Could not find node '%s' to connect", nodePath.GetText());
      continue;
    }

    UpdateConnections(nodeIt->second, nodes.Get(nodeName), nodePath, graph.get(), linkMaterialX);
  }

  /* UsdPreviewSurface's displacement is a height along the normal, like MaterialX's displacement
   * (Midlevel 0, Scale 1). Returns no node when it is unauthored or 0. */
  auto previewSurfaceDisplacement = [&](HdMaterialNodeSchema surface) -> ShaderNode * {
    const TfToken &name = HdMaterialTerminalTokens->displacement;
    const float height = GetParameter(surface.GetParameters(), name, 0.0f);
    HdMaterialConnectionVectorSchema connVec = surface.GetInputConnections().Get(name);
    if (connVec.GetNumElements() == 0 && height == 0.0f) {
      return nullptr;
    }
    DisplacementNode *displacement = graph->create_node<DisplacementNode>();
    displacement->set_space(NODE_NORMAL_MAP_OBJECT);
    displacement->set_height(height);
    displacement->set_midlevel(0.0f);
    displacement->set_scale(1.0f);
    if (connVec.GetNumElements() > 0) {
      HdMaterialConnectionSchema conn = connVec.GetElement(0);
      const auto srcIt = _nodes.find(
          conn.GetUpstreamNodePath() ?
              MaterialNodeNameToSdfPath(conn.GetUpstreamNodePath()->GetTypedValue(0.0f)) :
              SdfPath());
      ShaderInput *input = displacement->input("Height");
      if (srcIt != _nodes.end()) {
        const TfToken output = conn.GetUpstreamNodeOutputName() ?
                                   conn.GetUpstreamNodeOutputName()->GetTypedValue(0.0f) :
                                   TfToken();
        const std::string outputName =
            srcIt->second.mapping ? srcIt->second.mapping->parameterName(output, input) :
                                    output.GetString();
        if (ShaderOutput *link = FindOutput(srcIt->second.node, outputName)) {
          graph->connect(link, input);
        }
      }
    }
    return displacement;
  };

  const UsdToCyclesMapping *previewSurface = sUsdToCyles->findUsd(
      CyclesMaterialTokens->UsdPreviewSurface);
  /* Before finalize(), which applies UsdUVTexture's scale and bias to the linked outputs. A node
   * without a displacement terminal stays unlinked and the graph removes it. */
  std::unordered_map<SdfPath, ShaderNode *, SdfPath::Hash> previewDisplacements;
  for (const TfToken &nodeName : nodes.GetNames()) {
    const SdfPath nodePath = MaterialNodeNameToSdfPath(nodeName);
    const auto nodeIt = _nodes.find(nodePath);
    if (nodeIt != _nodes.end() && nodeIt->second.mapping == previewSurface) {
      previewDisplacements[nodePath] = previewSurfaceDisplacement(nodes.Get(nodeName));
    }
  }

  /* USD inputs that need more nodes than the one their node maps to. */
  for (const TfToken &nodeName : nodes.GetNames()) {
    const auto nodeIt = _nodes.find(MaterialNodeNameToSdfPath(nodeName));
    if (nodeIt != _nodes.end() && nodeIt->second.mapping) {
      nodeIt->second.mapping->finalize(
          graph.get(), nodeIt->second.node, nodes.Get(nodeName).GetParameters());
    }
  }

  /* Finally connect the terminals to the graph output (Surface, Volume, Displacement). */
  HdMaterialConnectionContainerSchema terminals = network.GetTerminals();
  for (const TfToken &terminalName : terminals.GetNames()) {
    HdMaterialConnectionSchema termSchema = terminals.Get(terminalName);
    SdfPath upstreamNodePath = termSchema.GetUpstreamNodePath() ?
                                   MaterialNodeNameToSdfPath(
                                       termSchema.GetUpstreamNodePath()->GetTypedValue(0.0f)) :
                                   SdfPath();
    TfToken upstreamOutputName = termSchema.GetUpstreamNodeOutputName() ?
                                     termSchema.GetUpstreamNodeOutputName()->GetTypedValue(0.0f) :
                                     TfToken();

    /* Houdini only exports nodes linked to the material output, so its AOV Output node passes the
     * shader connected to its "shader" input through to its output. */
    for (auto aovIt = _nodes.find(upstreamNodePath);
         aovIt != _nodes.end() && IsAovPassThrough(aovIt->second.node, upstreamOutputName);
         aovIt = _nodes.find(upstreamNodePath))
    {
      HdMaterialConnectionVectorSchema connVec =
          nodes.Get(nodeNames[upstreamNodePath]).GetInputConnections().Get(kAovPassThroughInput);
      if (connVec.GetNumElements() == 0) {
        upstreamNodePath = SdfPath();
        break;
      }
      HdMaterialConnectionSchema connSchema = connVec.GetElement(0);
      upstreamNodePath = connSchema.GetUpstreamNodePath() ?
                             MaterialNodeNameToSdfPath(
                                 connSchema.GetUpstreamNodePath()->GetTypedValue(0.0f)) :
                             SdfPath();
      upstreamOutputName = connSchema.GetUpstreamNodeOutputName() ?
                               connSchema.GetUpstreamNodeOutputName()->GetTypedValue(0.0f) :
                               TfToken();
    }
    if (upstreamNodePath.IsEmpty()) {
      continue; /* Only an AOV Output node, nothing to shade with. */
    }

    const auto nodeIt = _nodes.find(upstreamNodePath);
    if (nodeIt == _nodes.end()) {
      TF_RUNTIME_ERROR("Could not find terminal node '%s'", upstreamNodePath.GetText());
      continue;
    }

    ShaderNode *node = nodeIt->second.node;

    const char *inputName = nullptr;
    const char *outputName = nullptr;
    if (terminalName == HdMaterialTerminalTokens->surface ||
        terminalName == CyclesMaterialTokens->cyclesSurface)
    {
      inputName = "Surface";
      /* Find default output name based on the node if none is provided. */
      if (node->type->name == "add_closure" || node->type->name == "mix_closure") {
        outputName = "Closure";
      }
      else if (node->type->name == "emission") {
        outputName = "Emission";
      }
      else if (node->special_type == SHADER_SPECIAL_TYPE_OSL) {
        /* Script node: its first closure output. */
        for (const ShaderOutput *out : node->outputs) {
          if (out->type() == SocketType::CLOSURE) {
            outputName = out->name().c_str();
            break;
          }
        }
      }
      else {
        outputName = "BSDF";
      }
    }
    else if (terminalName == HdMaterialTerminalTokens->displacement ||
             terminalName == CyclesMaterialTokens->cyclesDisplacement)
    {
      if (nodeIt->second.mapping == previewSurface) {
        node = previewDisplacements[upstreamNodePath];
        if (!node) {
          continue;
        }
      }
      else if (nodeIt->second.mapping) {
        /* Unsupported mapped USD inputs and connections are reported while
         * processing the node. */
        continue;
      }
      inputName = outputName = "Displacement";
    }
    else if (terminalName == HdMaterialTerminalTokens->volume ||
             terminalName == CyclesMaterialTokens->cyclesVolume)
    {
      inputName = outputName = "Volume";
    }
    else {
      /* Other terminals only pull nodes into the network, like an AOV Output node, which
       * writes its pass without being linked to the material output. */
      continue;
    }

    /* For native Cycles nodes we use the upstream output name as is, for
     * mapping from e.g. UsdPreviewSurface we need to use the default output
     * name that is known to exist. */
    if (!upstreamOutputName.IsEmpty() && nodeIt->second.mapping == nullptr) {
      outputName = upstreamOutputName.GetText();
    }

    /* Houdini's material builder authors every Cycles shader as surface terminal, also volume
     * shaders, which would leave volumes without a shader. */
    if (string(inputName) == "Surface" && string_iequals(outputName, "volume")) {
      inputName = "Volume";
    }

    ShaderInput *const input = inputName ? graph->output()->input(inputName) : nullptr;
    if (!input) {
      TF_WARN("Could not find terminal input '%s.%s'",
                       upstreamNodePath.GetText(),
                       inputName ? inputName : "<null>");
      continue;
    }

    ShaderOutput *const output = outputName ? FindOutput(node, outputName) : nullptr;
    if (!output) {
      TF_WARN("Could not find terminal output '%s.%s'",
                       upstreamNodePath.GetText(),
                       outputName ? outputName : "<null>");
      continue;
    }

    graph->connect(output, input);
  }

  expand_parallax_occlusion(graph.get());

  /* Return no graph if nothing got successfully linked due unsupported nodes,
   * avoids the surface becoming transparent then. */
  bool any_output_linked = false;
  for (ShaderInput *input : graph->output()->inputs) {
    any_output_linked |= input->link != nullptr;
  }
  if (!any_output_linked) {
    _nodes.clear();
    return nullptr;
  }

  return graph;
}

void HdCyclesMaterial::Finalize(HdRenderParam *renderParam)
{
  if (!_shader) {
    return;
  }

  const SceneLock lock(renderParam);
  HdCyclesSession *const session = static_cast<HdCyclesSession *>(renderParam);
  const bool keep_nodes = session->keep_nodes;

  _nodes.clear();

  /* The world material of a dome light: UpdateScene() picks the light's own shader again. */
  session->ReleaseWorldShader(_shader);
  /* Materials rendered as this override material go back to their own. */
  if (session->GetMaterialOverride() == GetId().GetString()) {
    session->RequestResync(true, false);
    session->UpdateDefaultSurface();
  }

  if (!keep_nodes) {
    static_cast<HdCyclesSession *>(renderParam)->DeleteNode(_shader);
    /* Cycles keeps deleted shaders: without its name, world material lookups (by path) do not
     * find it, also not instead of a new material at the same path. */
    _shader->name = ustring();
  }
  _shader = nullptr;
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
