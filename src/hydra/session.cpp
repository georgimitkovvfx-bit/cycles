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

#include <algorithm>

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

SceneLock::SceneLock(const HdRenderParam *renderParam)
    : scene(static_cast<const HdCyclesSession *>(renderParam)->session->scene.get()),
      sceneLock(scene->mutex)
{
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

  // Wire up object color in default surface material
  {
    unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();

    ObjectInfoNode *objectNode = graph->create_node<ObjectInfoNode>();

    DiffuseBsdfNode *diffuseNode = graph->create_node<DiffuseBsdfNode>();

    graph->connect(objectNode->output("Color"), diffuseNode->input("Color"));
    graph->connect(diffuseNode->output("BSDF"), graph->output()->input("Surface"));

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
}

HdCyclesSession::~HdCyclesSession()
{
  if (_ownCyclesSession) {
    delete session;
  }
}

void HdCyclesSession::UpdateScene()
{
  Scene *const scene = session->scene.get();

  // Update background depending on presence of a background light
  if (scene->light_manager->need_update()) {
    Light *background_light = nullptr;
    bool have_lights = false;
    for (Object *object : scene->objects) {
      if (!object->get_geometry()->is_light()) {
        continue;
      }

      have_lights = true;

      Light *light = static_cast<Light *>(object->get_geometry());
      if (light->is_background_light()) {
        background_light = light;
        break;
      }
    }

    if (!background_light) {
      scene->background->set_shader(scene->default_background);
      scene->background->set_transparent(true);

      /* Set background color depending to non-zero value if there are no
       * lights in the scene, to match behavior of other renderers. */
      for (ShaderNode *node : scene->default_background->graph->nodes) {
        if (node->is_a(BackgroundNode::get_node_type())) {
          BackgroundNode *bgNode = static_cast<BackgroundNode *>(node);
          bgNode->set_color((have_lights) ? zero_float3() : make_float3(0.5f));
        }
      }
    }
    else {
      scene->background->set_shader(background_light->get_shader());
      scene->background->set_transparent(false);
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
