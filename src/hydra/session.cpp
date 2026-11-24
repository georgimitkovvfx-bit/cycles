/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/session.h"
#include "scene/object.h"
#include "scene/shader.h"
// Have to include shader.h before background.h so that 'set_shader' uses the correct 'set'
// overload taking a 'Node *', rather than the one taking a 'bool'
#include "scene/background.h"
#include "scene/film.h"
#include "scene/light.h"
#include "scene/pass.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "session/session.h"
#include "util/string.h"

#include <pxr/base/tf/diagnostic.h>
#include <pxr/imaging/hd/collectionExpressionEvaluator.h>

#include <algorithm>
#include <map>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

const std::unordered_map<TfToken, PassType, TfToken::HashFunctor> kAovToPass = {
    {HdAovTokens->color, PASS_COMBINED},
    {HdAovTokens->depth, PASS_DEPTH},
    {HdAovTokens->normal, PASS_NORMAL},
    {HdAovTokens->primId, PASS_OBJECT_ID},
    {HdAovTokens->instanceId, PASS_AOV_VALUE},
    /* Cycles' own passes, by the names of Blender's view-layer passes (`use_pass_<name>`), which USD Nodes
     * also uses as renderer-neutral AOV names. */
    {TfToken("z"), PASS_DEPTH},
    {TfToken("mist"), PASS_MIST},
    {TfToken("position"), PASS_POSITION},
    {TfToken("uv"), PASS_UV},
    {TfToken("vector"), PASS_MOTION},
    {TfToken("object_index"), PASS_OBJECT_ID},
    {TfToken("material_index"), PASS_MATERIAL_ID},
    {TfToken("diffuse_direct"), PASS_DIFFUSE_DIRECT},
    {TfToken("diffuse_indirect"), PASS_DIFFUSE_INDIRECT},
    {TfToken("diffuse_color"), PASS_DIFFUSE_COLOR},
    {TfToken("glossy_direct"), PASS_GLOSSY_DIRECT},
    {TfToken("glossy_indirect"), PASS_GLOSSY_INDIRECT},
    {TfToken("glossy_color"), PASS_GLOSSY_COLOR},
    {TfToken("transmission_direct"), PASS_TRANSMISSION_DIRECT},
    {TfToken("transmission_indirect"), PASS_TRANSMISSION_INDIRECT},
    {TfToken("transmission_color"), PASS_TRANSMISSION_COLOR},
    {TfToken("volume_direct"), PASS_VOLUME_DIRECT},
    {TfToken("volume_indirect"), PASS_VOLUME_INDIRECT},
    {TfToken("emit"), PASS_EMISSION},
    {TfToken("environment"), PASS_BACKGROUND},
    {TfToken("ambient_occlusion"), PASS_AO},
    {TfToken("shadow_catcher"), PASS_SHADOW_CATCHER},
};

/* `lightgroup_<name>`: the combined pass of the lights and emitters whose light group is <name>. */
const std::string kLightgroupPrefix = "lightgroup_";

/* Cryptomatte layers as Blender names them: CryptoObject00, CryptoObject01, … (two ID / weight pairs each). The
 * kernel writes all object layers, then material, then asset ones, so the passes are created in that order. */
const std::pair<const char *, CryptomatteType> kCryptomatteTypes[] = {
    {"CryptoObject", CRYPT_OBJECT}, {"CryptoMaterial", CRYPT_MATERIAL}, {"CryptoAsset", CRYPT_ASSET}};

}  // namespace

SceneLock::SceneLock(const HdRenderParam *renderParam, const bool holdRender)
    : scene(static_cast<const HdCyclesSession *>(renderParam)->session->scene.get()),
      sceneLock(scene->mutex)
{
  /* Hydra deletes or rewrites geometry while the session thread renders (the viewport), and the CPU BVH reads the
   * meshes' arrays in place: stop rendering until the render pass resets the session. Only for geometry: holding
   * cancels the samples in flight, and every frame takes the lock (CommitResources). Hosts with an exclusive sync
   * never render meanwhile. */
  const HdCyclesSession *session = static_cast<const HdCyclesSession *>(renderParam);
  if (holdRender && !session->ExclusiveSync()) {
    session->session->hold_render();
  }
}

SceneLock::~SceneLock() = default;

HdCyclesSession::HdCyclesSession(Session *session_, const bool keep_nodes)
    : session(session_), keep_nodes(keep_nodes), _ownCyclesSession(false)
{
}

HdCyclesSession::HdCyclesSession(const SessionParams &params)
    : session(new Session(params, SceneParams())), keep_nodes(false), _ownCyclesSession(true)
{
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

  SetDefaultSurface(scene);
}

void HdCyclesSession::SetDefaultSurface(Scene *scene)
{
  /* A diffuse BSDF before: without the Principled BSDF's specular layer, objects without a material rendered 6-8 %
   * brighter in the Blender runner than in cycles_usd_render and native Cycles. */
  unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();

  ObjectInfoNode *objectNode = graph->create_node<ObjectInfoNode>();
  PrincipledBsdfNode *bsdfNode = graph->create_node<PrincipledBsdfNode>();

  graph->connect(objectNode->output("Color"), bsdfNode->input("Base Color"));
  graph->connect(bsdfNode->output("BSDF"), graph->output()->input("Surface"));

  // Create the instanceId AOV output
  const ustring instanceId(HdAovTokens->instanceId.GetString());

  OutputAOVNode *aovNode = graph->create_node<OutputAOVNode>();
  aovNode->set_name(instanceId);

  AttributeNode *instanceIdNode = graph->create_node<AttributeNode>();
  instanceIdNode->set_attribute(instanceId);

  graph->connect(instanceIdNode->output("Fac"), aovNode->input("Value"));

  scene->default_surface->set_graph(std::move(graph));
  scene->default_surface->tag_update(scene);
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

void HdCyclesSession::RetireShader(Scene *scene, Shader *shader)
{
  if (shader == nullptr || keep_nodes) {
    return;
  }
  scene->delete_node(shader); /* Only clears its users: Cycles keeps it. */
  for (const SocketType &socket : shader->type->inputs) {
    shader->set_default_value(socket);
  }
  shader->set_graph(make_unique<ShaderGraph>()); /* Frees the old graph's nodes. */
  shader->has_surface_spatial_varying = false;
  shader->tag_update(scene);
  if (std::find(_retiredShaders.begin(), _retiredShaders.end(), shader) == _retiredShaders.end()) {
    _retiredShaders.push_back(shader);
  }
}

HdCyclesSession::~HdCyclesSession()
{
  if (_ownCyclesSession) {
    delete session;
  }
}

void HdCyclesSession::SetLinkGeometry(const void *owner,
                                      const SdfPath &id,
                                      const std::vector<Object *> *objects)
{
  const std::lock_guard<std::mutex> guard(_linksMutex);
  _linkedGeometry[owner] = LinkedGeometry{id, objects};
  /* new or changed instances need their sets: only when links are in use */
  _linksDirty = _linksDirty || !_lightLinks.empty();
}

void HdCyclesSession::RemoveLinkGeometry(const void *owner)
{
  const std::lock_guard<std::mutex> guard(_linksMutex);
  _linksDirty = (_linkedGeometry.erase(owner) > 0 && !_lightLinks.empty()) || _linksDirty;
}

void HdCyclesSession::SetLightLinks(Object *light,
                                    const HdSceneIndexBaseRefPtr &sceneIndex,
                                    const SdfPath &prefix,
                                    const SdfPathExpression &lightLink,
                                    const SdfPathExpression &shadowLink)
{
  const std::lock_guard<std::mutex> guard(_linksMutex);
  _linkSceneIndex = sceneIndex;
  _linkPrefix = prefix;
  const auto found = _lightLinks.find(light);
  if (lightLink.IsEmpty() && shadowLink.IsEmpty()) {
    if (found != _lightLinks.end()) {
      _lightLinks.erase(found);
      _linksDirty = true;
    }
    return;
  }
  if (found != _lightLinks.end() && found->second.light == lightLink && found->second.shadow == shadowLink) {
    return;
  }
  _lightLinks[light] = LightLinks{lightLink, shadowLink};
  _linksDirty = true;
}

void HdCyclesSession::RemoveLightLinks(Object *light)
{
  const std::lock_guard<std::mutex> guard(_linksMutex);
  _linksDirty = _lightLinks.erase(light) > 0 || _linksDirty;
}

void HdCyclesSession::UpdateLinks()
{
  _linksDirty = false;
  if (_lightLinks.empty()) {
    if (_linksApplied) { /* the last link went: back to Cycles' defaults */
      for (const auto &[owner, geometry] : _linkedGeometry) {
        for (Object *object : *geometry.objects) {
          object->set_receiver_light_set(0);
          object->set_blocker_shadow_set(0);
        }
      }
      _linksApplied = false;
    }
    return;
  }
  /* Lights no longer linked keep a mask until here: reset every light, then set the linked ones. */
  for (Object *object : session->scene->objects) {
    if (object->get_geometry() && object->get_geometry()->is_light()) {
      object->set_light_set_membership(LIGHT_LINK_MASK_ALL);
      object->set_shadow_set_membership(LIGHT_LINK_MASK_ALL);
    }
  }
  std::vector<const LinkedGeometry *> geometry;
  geometry.reserve(_linkedGeometry.size());
  for (const auto &[owner, entry] : _linkedGeometry) {
    geometry.push_back(&entry);
  }
  for (const bool shadow : {false, true}) {
    /* Which geometry each linked light reaches; one that reaches all is no link. */
    std::vector<std::pair<Object *, std::vector<bool>>> linked;
    for (const auto &[light, links] : _lightLinks) {
      const SdfPathExpression &expression = shadow ? links.shadow : links.light;
      if (expression.IsEmpty()) {
        continue;
      }
      const HdCollectionExpressionEvaluator evaluator(_linkSceneIndex, expression);
      std::vector<bool> reached(geometry.size());
      bool all = true;
      for (size_t i = 0; i < geometry.size(); ++i) {
        reached[i] = bool(evaluator.Match(_linkPrefix.IsAbsoluteRootPath() ?
                                              geometry[i]->id :
                                              geometry[i]->id.ReplacePrefix(_linkPrefix,
                                                                            SdfPath::AbsoluteRootPath())));
        all = all && reached[i];
      }
      if (!all) {
        linked.emplace_back(light, std::move(reached));
      }
    }
    /* Set 0: geometry no linked light reaches (every unlinked light does). */
    std::map<std::vector<size_t>, uint> sets;
    bool overflow = false;
    for (size_t i = 0; i < geometry.size(); ++i) {
      std::vector<size_t> signature;
      for (size_t l = 0; l < linked.size(); ++l) {
        if (linked[l].second[i]) {
          signature.push_back(l);
        }
      }
      uint index = 0;
      if (!signature.empty()) {
        const auto found = sets.find(signature);
        if (found != sets.end()) {
          index = found->second;
        }
        else if (sets.size() + 1 < LIGHT_LINK_SET_MAX) {
          index = uint(sets.size() + 1);
          sets.emplace(signature, index);
        }
        else {
          overflow = true;
        }
      }
      for (Object *object : *geometry[i]->objects) {
        if (shadow) {
          object->set_blocker_shadow_set(index);
        }
        else {
          object->set_receiver_light_set(index);
        }
      }
    }
    if (overflow) {
      TF_WARN("Hydra Cycles: more than %d different %s link combinations; the rest use the default set",
              LIGHT_LINK_SET_MAX - 1,
              shadow ? "shadow" : "light");
    }
    for (size_t l = 0; l < linked.size(); ++l) {
      uint64_t mask = 0;
      for (const auto &[signature, index] : sets) {
        if (std::binary_search(signature.begin(), signature.end(), l)) {
          mask |= uint64_t(1) << index;
        }
      }
      if (shadow) {
        linked[l].first->set_shadow_set_membership(mask);
      }
      else {
        linked[l].first->set_light_set_membership(mask);
      }
    }
  }
  _linksApplied = true;
}

void HdCyclesSession::UpdateScene()
{
  Scene *const scene = session->scene.get();

  if (_linksDirty) {
    const std::lock_guard<std::mutex> guard(_linksMutex);
    UpdateLinks();
  }

  // Update background depending on presence of a background light
  if (scene->light_manager->need_update() || _backgroundDirty) {
    _backgroundDirty = false;
    Light *background_light = nullptr;
    for (Object *object : scene->objects) {
      if (!object->get_geometry()->is_light()) {
        continue;
      }

      Light *light = static_cast<Light *>(object->get_geometry());
      if (light->is_background_light()) {
        background_light = light;
        break;
      }
    }

    if (!background_light) {
      scene->background->set_shader(scene->default_background);
      scene->background->set_transparent(true);

      /* No dome light: a black background, as native Cycles and cycles_usd_render render it. A grey ambient for
       * stages without lights (usdview's habit) made the Blender runner light what the others leave dark. */
      for (ShaderNode *node : scene->default_background->graph->nodes) {
        if (node->is_a(BackgroundNode::get_node_type())) {
          static_cast<BackgroundNode *>(node)->set_color(zero_float3());
        }
      }
    }
    else {
      scene->background->set_shader(background_light->get_shader());
      const bool seen = _domeCameraVisible.value_or(_domeCameraVisibleDefault);
      const uint visibility = scene->background->get_visibility();
      scene->background->set_visibility(seen ? (visibility | PATH_RAY_VISIBILITY_CAMERA) :
                                               (visibility & ~PATH_RAY_VISIBILITY_CAMERA));
      scene->background->set_transparent(!seen);
    }
    if (_transparentBackground >= 0) {
      scene->background->set_transparent(_transparentBackground != 0);
    }

    scene->background->tag_update(scene);
  }
}

void HdCyclesSession::SyncAovBindings(const HdRenderPassAovBindingVector &aovBindings)
{
  Scene *const scene = session->scene.get();

  // Delete all existing passes
  const vector<Pass *> &scene_passes = scene->passes;
  scene->delete_nodes(set<Pass *>(scene_passes.begin(), scene_passes.end()));

  // Update passes with requested AOV bindings
  _aovBindings = aovBindings;
  int cryptomatteType = CRYPT_NONE;
  int cryptomatteDepth = 0;
  std::vector<std::pair<int, std::string>> cryptomatteLayers;
  for (const HdRenderPassAovBinding &aovBinding : aovBindings) {
    const std::string &name = aovBinding.aovName.GetString();
    if (string_startswith(name, kLightgroupPrefix)) {
      Pass *pass = scene->create_node<Pass>();
      pass->set_type(PASS_COMBINED);
      pass->set_mode(PassMode::NOISY); /* as Blender's sync: light groups are not denoised */
      pass->set_name(ustring(name));
      pass->set_lightgroup(ustring(name.substr(kLightgroupPrefix.size())));
      continue;
    }
    bool isCryptomatte = false;
    for (int rank = 0; rank < 3; ++rank) {
      const std::string prefix = kCryptomatteTypes[rank].first;
      if (string_startswith(name, prefix)) {
        cryptomatteType |= kCryptomatteTypes[rank].second;
        cryptomatteLayers.emplace_back(rank, name);
        isCryptomatte = true;
      }
    }
    if (isCryptomatte) {
      continue;
    }
    const auto cyclesAov = kAovToPass.find(aovBinding.aovName);
    if (cyclesAov == kAovToPass.end()) {
      // TODO: Use PASS_AOV_COLOR and PASS_AOV_VALUE for these?
      TF_WARN("Unknown pass %s", aovBinding.aovName.GetText());
      continue;
    }

    const PassType type = cyclesAov->second;
    const PassMode mode = PassMode::DENOISED;

    Pass *pass = scene->create_node<Pass>();
    pass->set_type(type);
    pass->set_mode(mode);
    pass->set_name(ustring(aovBinding.aovName.GetString()));
  }

  std::stable_sort(cryptomatteLayers.begin(), cryptomatteLayers.end());
  for (int rank = 0; rank < 3; ++rank) {
    cryptomatteDepth = std::max<int>(
        cryptomatteDepth,
        std::count_if(cryptomatteLayers.begin(), cryptomatteLayers.end(), [rank](const auto &layer) {
          return layer.first == rank;
        }));
  }
  for (const auto &layer : cryptomatteLayers) {
    Pass *pass = scene->create_node<Pass>();
    pass->set_type(PASS_CRYPTOMATTE);
    pass->set_mode(PassMode::DENOISED);
    pass->set_name(ustring(layer.second));
  }
  scene->film->set_cryptomatte_passes(CryptomatteType(cryptomatteType));
  scene->film->set_cryptomatte_depth(cryptomatteDepth);
}

void HdCyclesSession::RemoveAovBinding(HdRenderBuffer *renderBuffer)
{
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
