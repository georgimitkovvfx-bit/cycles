/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/session.h"
#include "hydra/bake.h"
#include "hydra/material.h"
#include "hydra/render_buffer.h"
#include "scene/object.h"
#include "scene/shader.h"
// Have to include shader.h before background.h so that 'set_shader' uses the correct 'set'
// overload taking a 'Node *', rather than the one taking a 'bool'
#include "scene/background.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "scene/light.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"

#include "hydra/dome_texture.h"
#include "session/session.h"
#include "util/path.h"
#include "util/string.h"

#include <pxr/imaging/hd/renderBuffer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

const std::unordered_map<TfToken, PassType, TfToken::HashFunctor> kAovToPass = {
    {HdAovTokens->color, PASS_COMBINED},
    {HdAovTokens->depth, PASS_DEPTH},
    {HdAovTokens->normal, PASS_NORMAL},
    // Written by every shader from object attributes (HdCyclesMaterial::AddIdAOVs)
    {HdAovTokens->primId, PASS_AOV_VALUE},
    {HdAovTokens->instanceId, PASS_AOV_VALUE},
    // Source names of Karma's render vars (Karma Render Settings LOP)
    {TfToken("ray:hitP"), PASS_POSITION},
    {TfToken("ray:hitPz"), PASS_DEPTH},
    {TfToken("ray:hitN"), PASS_NORMAL},
    {TfToken("ray:primid"), PASS_AOV_VALUE},
    {TfToken("st"), PASS_UV},
    {TfToken("AO"), PASS_AO},
    // Blender's view layer pass names where they differ from Cycles' (USD Nodes' render vars for Cycles; the
    // others, diffuse_direct, mist, ..., are Cycles' names too)
    {TfToken("z"), PASS_DEPTH},
    {TfToken("vector"), PASS_MOTION},
    {TfToken("object_index"), PASS_OBJECT_ID},
    {TfToken("material_index"), PASS_MATERIAL_ID},
    {TfToken("emit"), PASS_EMISSION},
    {TfToken("environment"), PASS_BACKGROUND},
    {TfToken("ambient_occlusion"), PASS_AO},
};

// Light path expressions of Karma's AOVs, matched to the Cycles light passes holding the same
// light paths. These passes include the albedo, like an LPE.
const std::unordered_map<string, PassType> kLpeToPass = {
    {"C.*[LO]", PASS_COMBINED},
    {"C<RD>.*L", PASS_DIFFUSE},
    {"C<RD>L", PASS_DIFFUSE_DIRECT},
    {"C<RD>.+L", PASS_DIFFUSE_INDIRECT},
    {"C<RG>.*L", PASS_GLOSSY},
    {"C<RG>L", PASS_GLOSSY_DIRECT},
    {"C<RG>.+L", PASS_GLOSSY_INDIRECT},
    {"C<TG>.*L", PASS_TRANSMISSION},
    {"C<TG>L", PASS_TRANSMISSION_DIRECT},
    {"C<TG>.+L", PASS_TRANSMISSION_INDIRECT},
    {"CV.*L", PASS_VOLUME},
    {"CVL", PASS_VOLUME_DIRECT},
    {"CV.+L", PASS_VOLUME_INDIRECT},
    {"CO", PASS_EMISSION},
    {"CDA", PASS_DIFFUSE_COLOR},
    {"C[DV]A", PASS_DIFFUSE_COLOR},
};

PassType LpeToPassType(string lpe)
{
  lpe.erase(std::remove(lpe.begin(), lpe.end(), ' '), lpe.end());
  const auto it = kLpeToPass.find(lpe);
  return it != kLpeToPass.end() ? it->second : PASS_NONE;
}

string GetAovSetting(const HdRenderPassAovBinding &aovBinding, const TfToken &key)
{
  const auto it = aovBinding.aovSettings.find(key);
  if (it == aovBinding.aovSettings.end()) {
    return string();
  }
  if (it->second.IsHolding<TfToken>()) {
    return it->second.UncheckedGet<TfToken>().GetString();
  }
  return it->second.IsHolding<string>() ? it->second.UncheckedGet<string>() : string();
}

// Light path expression of a render var: husk passes it in the AOV settings, others as an
// "lpe:" AOV name.
string GetLpe(const HdRenderPassAovBinding &aovBinding)
{
  if (GetAovSetting(aovBinding, TfToken("sourceType")) == "lpe") {
    return GetAovSetting(aovBinding, TfToken("sourceName"));
  }
  const string &name = aovBinding.aovName.GetString();
  return TfStringStartsWith(name, "lpe:") ? name.substr(4) : string();
}

// Source name of a render var: husk names AOVs after the render var prim and passes the source
// name in the AOV settings.
TfToken GetSourceName(const HdRenderPassAovBinding &aovBinding)
{
  const string sourceName = GetAovSetting(aovBinding, TfToken("sourceName"));
  return sourceName.empty() ? aovBinding.aovName : TfToken(sourceName);
}

// Cryptomatte render vars have Blender's layer names, "CryptoObject00", "CryptoMaterial01", ...
// with two ranks per layer.
const std::pair<const char *, CryptomatteType> kCryptomatteTypes[] = {
    {"CryptoObject", CRYPT_OBJECT},
    {"CryptoMaterial", CRYPT_MATERIAL},
    {"CryptoAsset", CRYPT_ASSET},
};

bool ParseCryptomatte(const string &name, CryptomatteType &type, int &index)
{
  for (const auto &[prefix, cryptomatteType] : kCryptomatteTypes) {
    const size_t length = strlen(prefix);
    if (name.size() == length + 2 && TfStringStartsWith(name, prefix) && isdigit(name[length]) &&
        isdigit(name[length + 1]))
    {
      type = cryptomatteType;
      index = std::stoi(name.substr(length));
      return true;
    }
  }
  return false;
}

// Render vars with the source name "lightgroup:<name>" are the combined pass of a light group:
// the lights and objects whose "lightgroup" setting is <name>. USD Nodes writes "lightgroup_<name>".
const char *const kLightgroupPrefixes[] = {"lightgroup:", "lightgroup_"};

// The light group a render var's source name selects, empty when none.
string LightgroupOf(const string &sourceName)
{
  for (const char *prefix : kLightgroupPrefixes) {
    if (TfStringStartsWith(sourceName, prefix)) {
      return sourceName.substr(strlen(prefix));
    }
  }
  return string();
}

// Light path expressions, light groups, Hydra AOVs, then Cycles pass names ("diffuse_direct",
// "mist", ...), then shader AOVs written by the AOV Output node, matched by name.
PassType AovToPassType(const HdRenderPassAovBinding &aovBinding)
{
  const string lpe = GetLpe(aovBinding);
  if (!lpe.empty()) {
    return LpeToPassType(lpe);
  }

  const TfToken sourceName = GetSourceName(aovBinding);
  const string &name = sourceName.GetString();
  if (!LightgroupOf(name).empty()) {
    return PASS_COMBINED;
  }

  const PassType type = HdCyclesSession::GetPassType(sourceName);
  if (type != PASS_NONE) {
    return type;
  }

  // Other Karma ray values are not supported
  if (TfStringStartsWith(name, "ray:")) {
    return PASS_NONE;
  }

  // Shader AOVs, and primvars written to an AOV by every shader
  const HdFormat format = aovBinding.renderBuffer ? aovBinding.renderBuffer->GetFormat() :
                                                    HdFormatInvalid;
  return HdGetComponentCount(format) == 1 ? PASS_AOV_VALUE : PASS_AOV_COLOR;
}

}  // namespace

string HdCyclesSession::GetPrimvar(const string &sourceName, const TfToken &sourceType)
{
  if (GetPassType(TfToken(sourceName)) != PASS_NONE) {
    return string();
  }
  if (TfStringStartsWith(sourceName, "primvars:")) {
    return sourceName.substr(strlen("primvars:"));
  }
  return sourceType == "primvar" ? sourceName : string();
}

PassType HdCyclesSession::GetPassType(const TfToken &aovName)
{
  const auto hydraAov = kAovToPass.find(aovName);
  if (hydraAov != kAovToPass.end()) {
    return hydraAov->second;
  }
  if (TfStringStartsWith(aovName.GetString(), "lpe:")) {
    return LpeToPassType(aovName.GetString().substr(4));
  }
  const NodeEnum *passTypes = Pass::get_type_enum();
  if (passTypes->exists(ustring(aovName.GetString()))) {
    const PassType type = PassType((*passTypes)[aovName.GetText()]);
    // Shader AOV passes need a name of their own, and cryptomatte needs film settings
    if (type != PASS_AOV_COLOR && type != PASS_AOV_VALUE && type != PASS_CRYPTOMATTE) {
      return type;
    }
  }
  return PASS_NONE;
}

namespace {

/* Session::is_session_thread_rendering is protected: called through a member pointer named in a derived class. */
struct SessionThreadState : Session {
  static bool Rendering(Session *session)
  {
    return (session->*&SessionThreadState::is_session_thread_rendering)();
  }
};

}  // namespace

bool HdCyclesSession::ExclusiveSync() const
{
  return _exclusiveSync || !SessionThreadState::Rendering(session);
}

SceneLock::SceneLock(const HdRenderParam *renderParam, const bool holdRender)
    : scene(static_cast<const HdCyclesSession *>(renderParam)->session->scene.get()),
      session(const_cast<HdCyclesSession *>(static_cast<const HdCyclesSession *>(renderParam))),
      sceneLock(scene->mutex)
{
  /* Hydra deletes or rewrites geometry while the session thread renders (viewports), and the CPU
   * BVH reads the meshes' arrays in place (Embree's shared buffers): stop rendering until the
   * render pass resets the session. Only for geometry: holding cancels the samples in flight, and
   * every frame takes the lock (CommitResources). Hosts with an exclusive sync never render
   * meanwhile. */
  if (holdRender && !session->ExclusiveSync()) {
    session->session->hold_render();
  }
  /* The session thread unlocks the scene while it loads kernels (Scene::device_update), then
   * uploads the shaders it compiled before and clears their update tags: changes made meanwhile
   * are lost (black surfaces, unwritten shader AOVs) unless it redoes the update. */
  scene->scene_updated_while_loading_kernels = true;
}

SceneLock::~SceneLock()
{
  /* Still locked: the session thread can clear the update tags as soon as the lock is released,
   * before the render pass checks them to reset the render. */
  if (scene->need_reset()) {
    session->sceneChanged = true;
  }
}

HdCyclesSession::HdCyclesSession(Session *session_, const bool keep_nodes)
    : session(session_), keep_nodes(keep_nodes), _ownCyclesSession(false)
{
}

HdCyclesSession::HdCyclesSession(const SessionParams &params, const SceneParams &scene_params)
    : keep_nodes(false), _ownCyclesSession(true)
{
  CreateSession(params, scene_params);
}

HdCyclesSession::~HdCyclesSession()
{
  if (_ownCyclesSession) {
    /* Scene destruction releases all remaining nodes in dependency order. */
    delete session;
  }
  else {
    const SceneLock lock(this);
    FlushDeletedNodes();
  }
}

void HdCyclesSession::CreateSession(const SessionParams &params, const SceneParams &scene_params)
{
  session = new Session(params, scene_params);

  /* A render in several tiles writes them to a file, then the full image goes to the output driver
   * from it (like Blender after the render). Called by the session thread when the last tile is
   * done. */
  session->full_buffer_written_cb = [this](string_view filename) {
    const string path(filename);
    session->process_full_buffer_from_disk(path);
    path_remove(path);
  };

  Scene *const scene = session->scene.get();

  // Create background with ambient light
  {
    unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();

    BackgroundNode *bgNode = graph->create_node<BackgroundNode>();
    bgNode->set_color(one_float3());

    graph->connect(bgNode->output("Background"), graph->output()->input("Surface"));

    scene->default_background->set_graph(std::move(graph));
    scene->default_background->tag_update(scene);
  }

  UpdateDefaultSurface();

  _numDefaultShaders = scene->shaders.size();
}

bool HdCyclesSession::IsSessionUnused() const
{
  const Scene *const scene = session->scene.get();
  /* Nodes waiting in FlushDeletedNodes() count as deleted. */
  return _ownCyclesSession && scene->geometry.size() == _deletedGeometry.size() &&
         scene->objects.size() == _deletedObjects.size() &&
         scene->shaders.size() == _numDefaultShaders + _deletedShaders.size() + _retiredShaders.size();
}

void HdCyclesSession::RecreateSession(const SessionParams &params, const SceneParams &scene_params)
{
  /* Scene destruction releases all nodes, also those waiting for deletion. */
  _deletedObjects.clear();
  _deletedGeometry.clear();
  _deletedShaders.clear();
  _retiredShaders.clear();
  delete session;
  CreateSession(params, scene_params);
}

void HdCyclesSession::DeleteNode(Object *node)
{
  _deletedObjects.insert(node);
}

void HdCyclesSession::DeleteNode(Geometry *node)
{
  _deletedGeometry.insert(node);
}

void HdCyclesSession::DeleteNode(Shader *node)
{
  _deletedShaders.insert(node);
}

Shader *HdCyclesSession::TakeShader(Scene *scene)
{
  if (_retiredShaders.empty()) {
    return scene->create_node<Shader>();
  }
  Shader *const shader = _retiredShaders.back();
  _retiredShaders.pop_back();
  return shader;
}

void HdCyclesSession::FlushDeletedNodes()
{
  Scene *const scene = session->scene.get();

  /* Batch deletion to avoid quadratic cost, in dependency order: objects reference
   * geometry, geometry references shaders. */
  if (!_deletedObjects.empty()) {
    scene->delete_nodes(_deletedObjects);
    _deletedObjects.clear();
  }
  if (!_deletedGeometry.empty()) {
    scene->delete_nodes(_deletedGeometry);
    _deletedGeometry.clear();
  }
  if (!_deletedShaders.empty()) {
    scene->delete_nodes(_deletedShaders); /* only clears their users: Cycles keeps them */
    for (Shader *shader : _deletedShaders) {
      for (const SocketType &socket : shader->type->inputs) {
        shader->set_default_value(socket);
      }
      shader->set_graph(make_unique<ShaderGraph>()); /* frees the old graph's nodes */
      shader->has_surface_spatial_varying = false;
      shader->name = ustring();
      shader->tag_update(scene);
      if (std::find(_retiredShaders.begin(), _retiredShaders.end(), shader) == _retiredShaders.end()) {
        _retiredShaders.push_back(shader);
      }
    }
    _deletedShaders.clear();
  }
}

void HdCyclesSession::UpdateScene()
{
  Scene *const scene = session->scene.get();

  FlushDeletedNodes();

  // Update background depending on presence of a background light. The session thread may have
  // cleared the light manager's tags since a light was added or removed.
  if (std::exchange(lightsChanged, false) || scene->light_manager->need_update()) {
    /* Cycles has one world: several dome lights are added into it, like Karma adds them. The
     * first dome light in scene order samples it and gives its light settings (ray visibility,
     * Multiple Importance, light group) to the whole world. */
    std::vector<Object *> domeObjects;
    for (Object *object : scene->objects) {
      if (!object->get_geometry()->is_light()) {
        continue;
      }

      if (static_cast<Light *>(object->get_geometry())->is_background_light()) {
        domeObjects.push_back(object);
      }
    }
    /* Hidden dome lights have zero strength: they add nothing and do not sample the world,
     * unless all of them are hidden (a black world, as before). */
    const auto isVisible = [](const Object *object) {
      return static_cast<Light *>(object->get_geometry())->get_strength() != zero_float3();
    };
    const bool anyVisible = std::any_of(domeObjects.begin(), domeObjects.end(), isVisible);
    std::vector<Light *> domes;
    const Object *firstDome = nullptr;
    for (const Object *object : domeObjects) {
      if (!anyVisible || isVisible(object)) {
        firstDome = firstDome ? firstDome : object;
        domes.push_back(static_cast<Light *>(object->get_geometry()));
      }
    }
    Light *background_light = domes.empty() ? nullptr : domes.front();

    for (const Object *object : domeObjects) {
      Light *light = static_cast<Light *>(object->get_geometry());
      const auto mis = _domeUseMis.find(light);
      const bool useMis = light == background_light &&
                          (mis == _domeUseMis.end() || mis->second);
      if (light->get_use_mis() != useMis) {
        light->set_use_mis(useMis);
        light->tag_update(scene);
      }
    }
    if (domes.size() < 2 && !_combinedWorldKey.empty()) {
      /* Release the images of the combined world. */
      _combinedWorldKey.clear();
      _combinedWorld->set_graph(make_unique<ShaderGraph>());
      _combinedWorld->tag_update(scene);
    }

    if (!background_light) {
      scene->background->set_shader(scene->default_background);
      scene->background->set_transparent(_transparentBackground.value_or(true));
      scene->background->set_visibility(PATH_RAY_VISIBILITY_ALL);
      scene->background->set_lightgroup(ustring());

      /* No dome light: a black world, as Karma renders a stage without lights (husk --headlight none). A grey
       * ambient for stages without lights lit what Karma leaves dark; hosts that want a light add one (husk's
       * headlight, Houdini's viewport). From hdcycles-blender patch 0017. */
      for (ShaderNode *node : scene->default_background->graph->nodes) {
        if (node->is_a(BackgroundNode::get_node_type())) {
          static_cast<BackgroundNode *>(node)->set_color(zero_float3());
        }
      }
      /* Recompile: the shader kept its color from before (white world light after removing a
       * dome light while other lights stay). */
      scene->default_background->tag_update(scene);
    }
    else {
      // The light manager enables the background light (MIS) by the background shader: decide
      // again, the session thread may have updated the lights before the shader was set here.
      Shader *shader = domes.size() > 1 ? UpdateCombinedWorld(domes) :
                                          GetWorldShader(background_light);
      for (const Object *object : domeObjects) {
        Light *light = static_cast<Light *>(object->get_geometry());
        SetLightShader(light, light == background_light ? shader : GetWorldShader(light));
      }
      if (scene->background->get_shader() != shader) {
        scene->light_manager->tag_update(scene, LightManager::SHADER_MODIFIED);
      }
      scene->background->set_shader(shader);
      /* Cycles takes the world's ray visibility from the background, not the light object. The
       * camera sees the world as it sees any light (light.cpp: the Cycles tab's Camera, else Karma's
       * Render Light Geometry, else not, as in Karma). A hidden world leaves the background
       * transparent, as in Karma; film transparency, when set, wins. */
      const bool seen = (firstDome->get_visibility() & PATH_RAY_VISIBILITY_CAMERA) != 0;
      scene->background->set_transparent(_transparentBackground.value_or(!seen));
      scene->background->set_visibility(firstDome->get_visibility());
      scene->background->set_lightgroup(firstDome->get_lightgroup());
    }

    scene->background->tag_update(scene);
  }

  UpdateLightLinking();
}

void HdCyclesSession::SetWorldMaterial(Light *light, const WorldMaterial &world)
{
  const std::string &material = world.material;
  const auto it = _worldMaterials.find(light);
  if (it != _worldMaterials.end() && it->second.material == material &&
      (material.empty() ||
       (it->second.rotation == world.rotation && it->second.exposure == world.exposure &&
        it->second.saturation == world.saturation)))
  {
    return;
  }
  const std::string oldMaterial = it != _worldMaterials.end() ? it->second.material : "";
  _worldMaterials[light] = world;
  lightsChanged = true;
  /* Materials orient their sky and environment textures by the light they are the world of:
   * only the old and the new world material change. */
  RequestMaterialResync(oldMaterial);
  RequestMaterialResync(material);
}

void HdCyclesSession::RemoveWorldMaterial(Light *light)
{
  const auto it = _worldMaterials.find(light);
  if (it == _worldMaterials.end()) {
    return;
  }
  RequestMaterialResync(it->second.material);
  RemoveWorldSource(it->second.lightShader);
  _domeUseMis.erase(light);
  _worldMaterials.erase(it);
  lightsChanged = true;
}

void HdCyclesSession::ReleaseWorldShader(Shader *shader)
{
  Scene *const scene = session->scene.get();
  RemoveWorldSource(shader);
  if (scene->background->get_shader() == shader) {
    scene->background->set_shader(scene->default_background);
    scene->background->tag_update(scene);
    lightsChanged = true;
  }
  for (const auto &[light, world] : _worldMaterials) {
    if (light->get_shader() == shader) {
      SetLightShader(light, world.lightShader);
      lightsChanged = true;
    }
  }
}

void HdCyclesSession::SetLightShader(Light *light, Shader *shader)
{
  if (light->get_shader() == shader) {
    return;
  }
  array<Node *> usedShaders(1);
  usedShaders[0] = shader;
  shader->tag_used(session->scene.get());
  light->set_used_shaders(usedShaders);
  light->tag_update(session->scene.get());
}

const HdCyclesSession::WorldMaterial *HdCyclesSession::GetWorldOf(
    const std::string &material) const
{
  for (const auto &[light, world] : _worldMaterials) {
    if (world.material == material) {
      return &world;
    }
  }
  return nullptr;
}

void HdCyclesSession::AdjustWorld(ShaderGraph *graph, const float strength, const float saturation)
{
  if (strength == 1.0f && saturation == 1.0f) {
    return;
  }
  const vector<ShaderNode *> nodes(graph->nodes.begin(), graph->nodes.end());
  for (ShaderNode *node : nodes) {
    if (node->type != BackgroundNode::get_node_type()) {
      continue;
    }
    BackgroundNode *background = static_cast<BackgroundNode *>(node);
    if (saturation != 1.0f) {
      ShaderInput *color = background->input("Color");
      HSVNode *hsv = graph->create_node<HSVNode>();
      hsv->set_saturation(saturation);
      if (ShaderOutput *source = color->link) {
        graph->disconnect(color);
        graph->connect(source, hsv->input("Color"));
      }
      else {
        hsv->set_color(background->get_color());
      }
      graph->connect(hsv->output("Color"), color);
    }
    ShaderInput *strengthIn = background->input("Strength");
    if (ShaderOutput *source = strengthIn->link) {
      MathNode *multiply = graph->create_node<MathNode>();
      multiply->set_math_type(NODE_MATH_MULTIPLY);
      multiply->set_value2(strength);
      graph->disconnect(strengthIn);
      graph->connect(source, multiply->input("Value1"));
      graph->connect(multiply->output("Value"), strengthIn);
    }
    else {
      background->set_strength(background->get_strength() * strength);
    }
  }
}

namespace {

/* Clones the nodes of src, except its output node, into dst. Returns the outputs in dst that
 * fed src's Surface and Volume. */
std::pair<ShaderOutput *, ShaderOutput *> CopyGraphNodes(ShaderGraph &src, ShaderGraph *dst)
{
  ShaderNode *const srcOutput = src.output();
  std::unordered_map<const ShaderNode *, ShaderNode *> nodes;
  for (ShaderNode *node : src.nodes) {
    if (node != srcOutput) {
      nodes[node] = node->clone(dst);
    }
  }
  /* shortcut: sockets that nodes add at runtime (OSL scripts, Raycast attribute outputs) are not
   * cloned and lose their links; build such worlds from the Hydra network if it matters. */
  for (ShaderNode *node : src.nodes) {
    if (node == srcOutput) {
      continue;
    }
    for (ShaderInput *input : node->inputs) {
      if (input->link) {
        ShaderOutput *from = nodes[input->link->parent]->output(input->link->name());
        ShaderInput *to = nodes[node]->input(input->name());
        if (from && to) {
          dst->connect(from, to);
        }
      }
    }
  }
  const auto mapped = [&](const char *name) -> ShaderOutput * {
    ShaderInput *input = srcOutput->input(name);
    return input && input->link ? nodes[input->link->parent]->output(input->link->name()) :
                                  nullptr;
  };
  return {mapped("Surface"), mapped("Volume")};
}

void AddClosure(ShaderGraph *graph, ShaderOutput *&sum, ShaderOutput *closure)
{
  if (!closure) {
    return;
  }
  if (!sum) {
    sum = closure;
    return;
  }
  AddClosureNode *add = graph->create_node<AddClosureNode>();
  graph->connect(sum, add->input("Closure1"));
  graph->connect(closure, add->input("Closure2"));
  sum = add->output("Closure");
}

}  // namespace

void HdCyclesSession::SetWorldSource(Shader *shader, ShaderGraph &graph)
{
  auto copy = std::make_shared<ShaderGraph>();
  const auto [surface, volume] = CopyGraphNodes(graph, copy.get());
  if (surface) {
    copy->connect(surface, copy->output()->input("Surface"));
  }
  if (volume) {
    copy->connect(volume, copy->output()->input("Volume"));
  }
  _worldSources[shader] = {std::move(copy), ++_worldSourceVersion};
  if (!_combinedWorldKey.empty()) {
    lightsChanged = true;
  }
  for (const auto &[light, world] : _worldMaterials) {
    if (world.lightShader == shader && _domeTextureMaterials.count(world.material)) {
      RequestMaterialResync(world.material);
    }
  }
}

void HdCyclesSession::ExpandDomeTextures(ShaderGraph *graph, const WorldMaterial &world)
{
  std::vector<ShaderNode *> domeTextures;
  for (ShaderNode *node : graph->nodes) {
    if (node->type == DomeTextureNode::get_node_type()) {
      domeTextures.push_back(node);
    }
  }
  if (domeTextures.empty()) {
    _domeTextureMaterials.erase(world.material);
    return;
  }
  _domeTextureMaterials.insert(world.material);

  /* The Background color of the light's own graph, before the HSV node of AdjustWorld. */
  ShaderOutput *color = nullptr;
  float3 constant = zero_float3();
  const auto source = _worldSources.find(world.lightShader);
  if (source != _worldSources.end()) {
    ShaderOutput *surface = CopyGraphNodes(*source->second.graph, graph).first;
    if (surface && surface->parent->type == BackgroundNode::get_node_type()) {
      BackgroundNode *background = static_cast<BackgroundNode *>(surface->parent);
      color = background->input("Color")->link;
      constant = background->get_color();
      if (color && color->parent->type == HSVNode::get_node_type()) {
        constant = static_cast<HSVNode *>(color->parent)->get_color();
        color = color->parent->input("Color")->link;
      }
    }
  }
  if (!color) {
    ColorNode *node = graph->create_node<ColorNode>();
    node->set_value(constant);
    color = node->output("Color");
  }
  for (ShaderNode *node : domeTextures) {
    const vector<ShaderInput *> links = node->output("Color")->links;
    for (ShaderInput *to : links) {
      graph->disconnect(to);
      graph->connect(color, to);
    }
  }
}

void HdCyclesSession::RemoveWorldSource(Shader *shader)
{
  if (_worldSources.erase(shader)) {
    lightsChanged = true;
  }
}

void HdCyclesSession::SetDomeUseMis(Light *light, const bool useMis)
{
  const auto it = _domeUseMis.find(light);
  if (it == _domeUseMis.end() || it->second != useMis) {
    _domeUseMis[light] = useMis;
    lightsChanged = true;
  }
}

Shader *HdCyclesSession::GetWorldShader(Light *light) const
{
  const auto world = _worldMaterials.find(light);
  if (world == _worldMaterials.end()) {
    return light->get_shader();
  }
  if (!world->second.material.empty()) {
    for (Shader *material : session->scene->shaders) {
      if (material != world->second.lightShader && material->name == world->second.material) {
        return material;
      }
    }
  }
  return world->second.lightShader;
}

Shader *HdCyclesSession::UpdateCombinedWorld(const std::vector<Light *> &domes)
{
  Scene *const scene = session->scene.get();

  std::vector<uint64_t> key;
  std::vector<ShaderGraph *> sources;
  for (Light *light : domes) {
    const Shader *const shader = GetWorldShader(light);
    const auto source = _worldSources.find(shader);
    key.push_back(uint64_t(uintptr_t(shader)));
    key.push_back(source != _worldSources.end() ? source->second.version : 0);
    if (source != _worldSources.end()) {
      sources.push_back(source->second.graph.get());
    }
    else {
      TF_WARN("Dome light %s: no shader graph to add to the world", light->name.c_str());
    }
  }
  if (_combinedWorld && key == _combinedWorldKey) {
    return _combinedWorld;
  }
  _combinedWorldKey = std::move(key);

  unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();
  ShaderOutput *surface = nullptr;
  ShaderOutput *volume = nullptr;
  for (ShaderGraph *source : sources) {
    const auto [domeSurface, domeVolume] = CopyGraphNodes(*source, graph.get());
    AddClosure(graph.get(), surface, domeSurface);
    AddClosure(graph.get(), volume, domeVolume);
  }
  if (surface) {
    graph->connect(surface, graph->output()->input("Surface"));
  }
  if (volume) {
    graph->connect(volume, graph->output()->input("Volume"));
  }

  if (!_combinedWorld) {
    _combinedWorld = scene->create_node<Shader>();
    _combinedWorld->name = "hdCycles combined world";
  }
  _combinedWorld->set_graph(std::move(graph));
  _combinedWorld->tag_update(scene);
  return _combinedWorld;
}

void HdCyclesSession::SetLightLinks(const std::string &light,
                                    const TfToken &lightLink,
                                    const TfToken &shadowLink)
{
  std::pair<TfToken, TfToken> links(lightLink, shadowLink);
  const auto it = _lightLinks.find(light);
  if (it == _lightLinks.end() ? !links.first.IsEmpty() || !links.second.IsEmpty() :
                                it->second != links)
  {
    _lightLinks[light] = std::move(links);
    _lightLinkingDirty = true;
  }
}

void HdCyclesSession::SetCategories(const std::string &object, const VtArray<TfToken> &categories)
{
  const auto it = _categories.find(object);
  if (it == _categories.end() ? !categories.empty() : it->second != categories) {
    _categories[object] = categories;
    _lightLinkingDirty = true;
  }
}

void HdCyclesSession::SetInstanceCategories(
    const std::string &object, std::vector<std::pair<const Object *, VtArray<TfToken>>> categories)
{
  const auto it = _instanceCategories.find(object);
  if (categories.empty()) {
    if (it != _instanceCategories.end()) {
      _instanceCategories.erase(it);
      _lightLinkingDirty = true;
    }
    return;
  }
  if (it == _instanceCategories.end() || it->second != categories) {
    _instanceCategories[object] = std::move(categories);
    _lightLinkingDirty = true;
  }
}

void HdCyclesSession::RemoveLinking(const std::string &name)
{
  _lightLinkingDirty |= _lightLinks.erase(name) + _categories.erase(name) + _instanceCategories.erase(name) != 0;
}

void HdCyclesSession::UpdateLightLinking()
{
  if (!_lightLinkingDirty) {
    return;
  }
  _lightLinkingDirty = false;
  Scene *const scene = session->scene.get();

  /* Like Blender's light linking. The link category of a light is the set of objects it lights
   * (light link) or that cast its shadows (shadow link). Objects that belong to the same link
   * categories share a receiver (blocker) set, and a light is a member of the sets that hold its
   * category. Set 0 is the default of objects without linking, an empty category links all. */
  std::array<std::set<TfToken>, 2> linkCategories;
  for (const auto &[light, links] : _lightLinks) {
    for (const auto &[kind, category] : {std::pair(0, links.first), std::pair(1, links.second)}) {
      if (!category.IsEmpty()) {
        linkCategories[kind].insert(category);
      }
    }
  }

  std::array<std::map<std::vector<TfToken>, uint>, 2> sets = {{{{{}, 0}}, {{{}, 0}}}};
  const auto setsOf = [&](const VtArray<TfToken> &categories) {
    std::array<uint, 2> objectSet;
    for (int kind = 0; kind < 2; kind++) {
      std::vector<TfToken> linked;
      for (const TfToken &category : categories) {
        if (linkCategories[kind].count(category)) {
          linked.push_back(category);
        }
      }
      std::sort(linked.begin(), linked.end());
      linked.erase(std::unique(linked.begin(), linked.end()), linked.end());
      objectSet[kind] = sets[kind].emplace(std::move(linked), uint(sets[kind].size())).first->second;
    }
    return objectSet;
  };
  std::unordered_map<std::string, std::array<uint, 2>> objectSets;
  for (const auto &[object, categories] : _categories) {
    objectSets[object] = setsOf(categories);
  }
  /* Instances with categories of their own (SetInstanceCategories), by object. */
  std::unordered_map<const Object *, std::array<uint, 2>> instanceSets;
  for (const auto &[object, instances] : _instanceCategories) {
    for (const auto &[instance, categories] : instances) {
      instanceSets[instance] = setsOf(categories);
    }
  }
  if (sets[0].size() > LIGHT_LINK_SET_MAX || sets[1].size() > LIGHT_LINK_SET_MAX) {
    TF_WARN("Cycles supports %d light and %d shadow linking sets, the scene needs %d and %d",
            LIGHT_LINK_SET_MAX,
            LIGHT_LINK_SET_MAX,
            int(sets[0].size()),
            int(sets[1].size()));
  }

  const auto membership = [&sets](const int kind, const TfToken &category) {
    if (category.IsEmpty()) {
      return uint64_t(LIGHT_LINK_MASK_ALL);
    }
    uint64_t bits = 0;
    for (const auto &[categories, set] : sets[kind]) {
      if (set < LIGHT_LINK_SET_MAX &&
          std::binary_search(categories.begin(), categories.end(), category))
      {
        bits |= uint64_t(1) << set;
      }
    }
    return bits;
  };

  for (Object *object : scene->objects) {
    std::array<uint, 2> objectSet = {0, 0};
    std::pair<TfToken, TfToken> links;
    /* Lights, and mesh lights (cylinder lights, geometry lights) by their name. Geometry lights
     * are linked to other lights too. */
    if (const auto it = _lightLinks.find(object->name.string()); it != _lightLinks.end()) {
      links = it->second;
    }
    if (object->get_geometry()->is_light()) {
    }
    else if (const auto it = instanceSets.find(object); it != instanceSets.end()) {
      objectSet = it->second;
    }
    else if (const auto it = objectSets.find(object->name.string()); it != objectSets.end()) {
      objectSet = it->second;
    }

    object->set_receiver_light_set(objectSet[0]);
    object->set_blocker_shadow_set(objectSet[1]);
    object->set_light_set_membership(membership(0, links.first));
    object->set_shadow_set_membership(membership(1, links.second));
    if (object->is_modified()) {
      object->tag_update(scene);
    }
  }
}

void HdCyclesSession::SetTransparentBackground(const std::optional<bool> transparent)
{
  _transparentBackground = transparent;

  // Re-evaluate the automatic transparency on the next scene update
  session->scene->light_manager->tag_update(session->scene.get(), LightManager::UPDATE_ALL);
}

bool HdCyclesSession::SyncAovBindings(const HdRenderPassAovBindingVector &aovBindings)
{
  const auto lock = LockAovBindings();
  Scene *const scene = session->scene.get();
  const bool neededUV = scene->need_global_attribute(ATTR_STD_UV);

  // Delete all existing passes
  const vector<Pass *> &scene_passes = scene->passes;
  scene->delete_nodes(set<Pass *>(scene_passes.begin(), scene_passes.end()));

  // Update passes with requested AOV bindings
  _aovBindings = aovBindings;
  _passNames.clear();

  // Cryptomatte: the kernel writes every rank of the enabled types, in this order, so create all
  // those passes, also the ones no AOV reads.
  int cryptomatteTypes = CRYPT_NONE;
  int cryptomattePasses = 0;
  for (const HdRenderPassAovBinding &aovBinding : aovBindings) {
    CryptomatteType cryptomatteType;
    int index;
    if (ParseCryptomatte(GetSourceName(aovBinding).GetString(), cryptomatteType, index)) {
      cryptomatteTypes |= cryptomatteType;
      cryptomattePasses = std::max(cryptomattePasses, index + 1);
    }
  }
  scene->film->set_cryptomatte_passes(CryptomatteType(cryptomatteTypes));
  scene->film->set_cryptomatte_depth(cryptomattePasses);
  for (const auto &[prefix, cryptomatteType] : kCryptomatteTypes) {
    if (cryptomatteTypes & cryptomatteType) {
      for (int index = 0; index < cryptomattePasses; index++) {
        Pass *pass = scene->create_node<Pass>();
        pass->set_type(PASS_CRYPTOMATTE);
        pass->set_mode(PassMode::NOISY);
        pass->set_name(ustring(string_printf("%s%02d", prefix, index)));
      }
    }
  }

  std::unordered_map<int, const Pass *> passOfType;
  std::vector<std::pair<string, string>> primvarAovs;
  bool motionPassBound = false;
  for (const HdRenderPassAovBinding &aovBinding : aovBindings) {
    const string sourceName = GetSourceName(aovBinding).GetString();
    CryptomatteType cryptomatteType;
    int index;
    if (ParseCryptomatte(sourceName, cryptomatteType, index)) {
      _passNames[aovBinding.aovName] = sourceName;
      continue;
    }

    const PassType type = AovToPassType(aovBinding);
    if (type == PASS_NONE) {
      TF_WARN("Unknown pass %s", aovBinding.aovName.GetText());
      continue;
    }
    if (type == PASS_MOTION) {
      /* Cycles writes four floats per pixel, more than a smaller buffer holds. */
      if (aovBinding.renderBuffer &&
          HdGetComponentCount(aovBinding.renderBuffer->GetFormat()) != 4)
      {
        TF_WARN("Motion pass %s needs a 4 channel format", aovBinding.aovName.GetText());
        continue;
      }
      if (scene->integrator->get_motion_blur()) {
        TF_WARN("Motion pass %s is empty with motion blur, like in Blender",
                aovBinding.aovName.GetText());
      }
      motionPassBound = true;
    }

    const bool includeAlbedo = !GetLpe(aovBinding).empty();
    const string lightgroup = LightgroupOf(sourceName);

    // The kernel writes one buffer per pass type, so AOVs of the same type (the beauty and a
    // "C.*[LO]" LPE, for example) read the first pass of that type. Shader AOVs and light groups
    // have a pass each.
    if (type != PASS_AOV_COLOR && type != PASS_AOV_VALUE && lightgroup.empty()) {
      const auto existing = passOfType.find(type);
      if (existing != passOfType.end()) {
        if (existing->second->get_include_albedo() != includeAlbedo) {
          TF_WARN("AOV %s uses the same Cycles pass as %s, with%s albedo",
                  aovBinding.aovName.GetText(),
                  existing->second->get_name().c_str(),
                  existing->second->get_include_albedo() ? "" : "out");
        }
        _passNames[aovBinding.aovName] = existing->second->get_name().string();
        continue;
      }
    }

    // Light group passes are not denoised (like in Blender)
    const PassMode mode = lightgroup.empty() ? PassMode::DENOISED : PassMode::NOISY;

    Pass *pass = scene->create_node<Pass>();
    pass->set_type(type);
    pass->set_mode(mode);
    pass->set_name(ustring(aovBinding.aovName.GetString()));
    pass->set_include_albedo(includeAlbedo);
    pass->set_lightgroup(ustring(lightgroup));
    // Shader AOV passes are matched to AOV Output nodes by name, primvar AOVs get their own
    if (type == PASS_AOV_COLOR || type == PASS_AOV_VALUE) {
      const TfToken sourceName = GetSourceName(aovBinding);
      const string primvar = GetPrimvar(sourceName.GetString(),
                                        TfToken(GetAovSetting(aovBinding, TfToken("sourceType"))));
      if (!primvar.empty()) {
        primvarAovs.emplace_back(aovBinding.aovName.GetString(), primvar);
      }
      else {
        // Karma's prim ID is the primId AOV every shader writes
        const TfToken &passName = sourceName == TfToken("ray:primid") ? HdAovTokens->primId :
                                                                         sourceName;
        if (passName != aovBinding.aovName) {
          pass->set_name(ustring(passName.GetString()));
          _passNames[aovBinding.aovName] = passName.GetString();
        }
      }
    }
    if (lightgroup.empty()) {
      passOfType[type] = pass;
    }
  }

  /* Texture baking reads the bake input passes the output driver writes, and the UV map of the
   * target, which the geometry exports for a UV pass. */
  const BakeSettings bake = GetBake();
  if (!bake.object.empty()) {
    const std::pair<PassType, const char *> bakePasses[] = {
        {PASS_BAKE_PRIMITIVE, kBakePrimitivePass},
        {PASS_BAKE_DIFFERENTIAL, kBakeDifferentialPass},
        {PASS_UV, "BakeUV"},
        {PASS_BAKE_OBJECT, kBakeObjectPass},
    };
    for (const auto &[type, name] : bakePasses) {
      if ((type == PASS_UV && passOfType.count(PASS_UV)) ||
          (type == PASS_BAKE_OBJECT && !bake.useSources()))
      {
        continue;
      }
      Pass *pass = scene->create_node<Pass>();
      pass->set_type(type);
      pass->set_mode(PassMode::NOISY);
      pass->set_name(ustring(name));
    }
  }

  /* A motion pass added or removed while rendering, which the render settings did not have. */
  const std::vector<float> motionTimes = GetMotionTimes();
  _motionPassBound = motionPassBound;
  if (GetMotionTimes() != motionTimes) {
    RequestResync(false, true);
  }

  /* Geometry only exports the UV map for the UV pass when synced with the pass, which husk binds
   * after the first sync (Scene::need_global_attribute). */
  if (scene->need_global_attribute(ATTR_STD_UV) && !neededUV) {
    RequestResync(false, true);
  }

  const bool primvarAovsChanged = primvarAovs != _primvarAovs;
  _primvarAovs = std::move(primvarAovs);
  return primvarAovsChanged;
}

std::vector<float> HdCyclesSession::GetMotionTimes(const float shutterOpen,
                                                   const float shutterClose,
                                                   const int steps,
                                                   const int samples) const
{
  if (!session->scene->integrator->get_motion_blur()) {
    /* Motion pass: the previous and next frame, like Blender (Scene::motion_shutter_time() is 2
     * frames). Cycles writes no motion pass with motion blur. */
    const auto active = std::find_if(_motionPassSettings.begin(),
                                     _motionPassSettings.end(),
                                     [](const auto &settings) { return settings.second.first; });
    const bool settingsMotionPass = active != _motionPassSettings.end() ?
                                        active->second.second :
                                        std::any_of(_motionPassSettings.begin(),
                                                    _motionPassSettings.end(),
                                                    [](const auto &settings) {
                                                      return settings.second.second;
                                                    });
    if (!settingsMotionPass && !_motionPassBound) {
      return {};
    }
    return {-1.0f, 0.0f, 1.0f};
  }

  // Three steps for Blender's default of one motion step for objects
  const int numSteps = samples > 0 ? (std::min(samples, 129) | 1) :
                                     (1 << (steps > 0 ? std::min(steps, 7) : _motionSteps)) + 1;
  if (_motionBlurDisabled || !(shutterClose > shutterOpen) || numSteps < 3) {
    return {};
  }
  std::vector<float> times(numSteps);
  for (int i = 0; i < numSteps; i++) {
    times[i] = shutterOpen + (shutterClose - shutterOpen) * float(i) / float(numSteps - 1);
  }
  return times;
}

void HdCyclesSession::SetRenderSettingsPrimvarAovs(const SdfPath &id,
                                                   const bool active,
                                                   std::vector<std::pair<string, string>> &&aovs)
{
  if (aovs.empty() && !active) {
    _renderSettingsPrimvarAovs.erase(id);
  }
  else {
    _renderSettingsPrimvarAovs[id] = {active, std::move(aovs)};
  }

  std::vector<std::pair<string, string>> primvarAovs;
  for (const auto &[path, settings] : _renderSettingsPrimvarAovs) {
    if (settings.first) {
      primvarAovs = settings.second;
      break;
    }
    for (const auto &aov : settings.second) {
      if (std::find(primvarAovs.begin(), primvarAovs.end(), aov) == primvarAovs.end()) {
        primvarAovs.push_back(aov);
      }
    }
  }

  if (primvarAovs != _primvarAovs) {
    _primvarAovs = std::move(primvarAovs);
    UpdateDefaultSurface();
    /* Every shader writes the primvar AOVs, and geometry exports the primvars shaders read. */
    RequestResync(true, true);
  }
}

void HdCyclesSession::SetRenderSettingsMotionPass(const SdfPath &id,
                                                  const bool active,
                                                  const bool motionPass)
{
  const std::vector<float> motionTimes = GetMotionTimes();
  if (active || motionPass) {
    _motionPassSettings[id] = {active, motionPass};
  }
  else {
    _motionPassSettings.erase(id);
  }
  if (GetMotionTimes() != motionTimes) {
    RequestResync(false, true);
  }
}

void HdCyclesSession::UpdateDefaultSurface()
{
  UpdateDefaultSurface(nullptr);
}

void HdCyclesSession::UpdateDefaultSurface(unique_ptr<ShaderGraph> graph)
{
  if (!_ownCyclesSession) {
    return;  // The application owns the scene and its default shaders
  }

  Scene *const scene = session->scene.get();
  if (!graph) {
    graph = make_unique<ShaderGraph>();
    /* The clay material override is the gray Diffuse BSDF, like in HdCyclesMaterial::Sync. */
    if (GetMaterialOverride() == "clay") {
      DiffuseBsdfNode *diffuseNode = graph->create_node<DiffuseBsdfNode>();
      graph->connect(diffuseNode->output("BSDF"), graph->output()->input("Surface"));
    }
    else {
      /* A Principled BSDF, as Cycles' own default surface: a Diffuse BSDF rendered geometry
       * without a material 6 % brighter than an application's own session (cycles_usd_render) and
       * native Cycles, which keep Cycles' default. Karma's fallback has a highlight too. */
      PrincipledBsdfNode *bsdfNode = graph->create_node<PrincipledBsdfNode>();
      graph->connect(bsdfNode->output("BSDF"), graph->output()->input("Surface"));

      // Wire up object color in default surface material
      ObjectInfoNode *objectNode = graph->create_node<ObjectInfoNode>();

      /* A displayColor (or Cd) primvar of any interpolation, like Karma and Storm; the object
       * color (constant or per-instance displayColor, else 0.8 gray) without one. */
      AttributeNode *colorNode = graph->create_node<AttributeNode>();
      colorNode->set_attribute(ustring(HdTokens->displayColor.GetString()));

      graph->connect(objectNode->output("Color"), colorNode->input("Missing"));
      graph->connect(colorNode->output("Color"), bsdfNode->input("Base Color"));
    }
  }

  HdCyclesMaterial::AddIdAOVs(graph.get());
  HdCyclesMaterial::AddPrimvarAOVs(graph.get(), _primvarAovs);
  const AttributeRequestSet attributes = scene->default_surface->attributes;
  scene->default_surface->set_graph(std::move(graph));
  scene->default_surface->tag_update(scene);

  /* Geometry only exports the primvars that shaders read when it is synced. */
  if (scene->default_surface->attributes.modified(attributes)) {
    RequestResync(false, true);
  }
}

string HdCyclesSession::GetPassName(const TfToken &aovName) const
{
  const auto lock = LockAovBindings();
  const auto it = _passNames.find(aovName);
  return it != _passNames.end() ? it->second : aovName.GetString();
}

void HdCyclesSession::ResetConverged()
{
  const auto lock = LockAovBindings();
  for (const HdRenderPassAovBinding &aovBinding : _aovBindings) {
    if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(aovBinding.renderBuffer)) {
      renderBuffer->SetConverged(false);
    }
  }
}

void HdCyclesSession::RequestResync(const bool materials, const bool rprims)
{
  if (!_resyncEnabled) {
    return;
  }
  if (materials) {
    resyncMaterials = true;
  }
  if (rprims && !rprimsResyncing) {
    resyncRprims = true;
  }
  /* husk and the viewport only sync again while the render is not converged. */
  ResetConverged();
}

void HdCyclesSession::RequestMaterialResync(const std::string &material)
{
  if (!_resyncEnabled || material.empty()) {
    return;
  }
  {
    const std::lock_guard<thread_mutex> lock(_resyncMaterialIdsMutex);
    _resyncMaterialIds.insert(material);
  }
  ResetConverged();
}

std::set<std::string> HdCyclesSession::TakeMaterialResyncs()
{
  const std::lock_guard<thread_mutex> lock(_resyncMaterialIdsMutex);
  return std::exchange(_resyncMaterialIds, {});
}

void HdCyclesSession::RemoveAovBinding(HdRenderBuffer *renderBuffer)
{
  // Waits for the output driver to stop writing to the buffer, which is deleted after this
  const auto lock = LockAovBindings();
  for (HdRenderPassAovBinding &aovBinding : _aovBindings) {
    if (renderBuffer == aovBinding.renderBuffer) {
      aovBinding.renderBuffer = nullptr;
      break;
    }
  }

  if (renderBuffer == _displayAovBinding.renderBuffer) {
    _displayAovBinding.renderBuffer = nullptr;
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
