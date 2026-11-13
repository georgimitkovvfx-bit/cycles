/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/session.h"
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
#include "session/session.h"
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
    {HdAovTokens->primId, PASS_OBJECT_ID},
    {HdAovTokens->instanceId, PASS_AOV_VALUE},
    // Source names of Karma's render vars (Karma Render Settings LOP)
    {TfToken("ray:hitP"), PASS_POSITION},
    {TfToken("ray:hitPz"), PASS_DEPTH},
    {TfToken("ray:hitN"), PASS_NORMAL},
    {TfToken("ray:primid"), PASS_OBJECT_ID},
    {TfToken("st"), PASS_UV},
    {TfToken("AO"), PASS_AO},
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
// the lights and objects whose "lightgroup" setting is <name>.
const char *const kLightgroupPrefix = "lightgroup:";

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
  if (TfStringStartsWith(name, kLightgroupPrefix)) {
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

SceneLock::SceneLock(const HdRenderParam *renderParam)
    : scene(static_cast<const HdCyclesSession *>(renderParam)->session->scene.get()),
      session(const_cast<HdCyclesSession *>(static_cast<const HdCyclesSession *>(renderParam))),
      sceneLock(scene->mutex)
{
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
    delete session;
  }
}

void HdCyclesSession::CreateSession(const SessionParams &params, const SceneParams &scene_params)
{
  session = new Session(params, scene_params);

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
  return _ownCyclesSession && scene->geometry.empty() && scene->objects.empty() &&
         scene->shaders.size() == _numDefaultShaders;
}

void HdCyclesSession::RecreateSession(const SessionParams &params, const SceneParams &scene_params)
{
  delete session;
  CreateSession(params, scene_params);
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
      scene->background->set_transparent(_transparentBackground.value_or(true));

      /* Set background color depending to non-zero value if there are no
       * lights in the scene, to match behavior of other renderers. */
      for (ShaderNode *node : scene->default_background->graph->nodes) {
        if (node->is_a(BackgroundNode::get_node_type())) {
          BackgroundNode *bgNode = static_cast<BackgroundNode *>(node);
          bgNode->set_color((have_lights) ? zero_float3() : make_float3(0.5f));
        }
      }
      /* Recompile: the shader kept its color from before (white world light after removing a
       * dome light while other lights stay). */
      scene->default_background->tag_update(scene);
    }
    else {
      scene->background->set_shader(background_light->get_shader());
      scene->background->set_transparent(_transparentBackground.value_or(false));
    }

    scene->background->tag_update(scene);
  }

  UpdateLightLinking();
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

void HdCyclesSession::RemoveLinking(const std::string &name)
{
  _lightLinkingDirty |= _lightLinks.erase(name) + _categories.erase(name) != 0;
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
  std::unordered_map<std::string, std::array<uint, 2>> objectSets;
  for (const auto &[object, categories] : _categories) {
    for (int kind = 0; kind < 2; kind++) {
      std::vector<TfToken> linked;
      for (const TfToken &category : categories) {
        if (linkCategories[kind].count(category)) {
          linked.push_back(category);
        }
      }
      std::sort(linked.begin(), linked.end());
      linked.erase(std::unique(linked.begin(), linked.end()), linked.end());
      objectSets[object][kind] =
          sets[kind].emplace(std::move(linked), uint(sets[kind].size())).first->second;
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
    if (object->get_geometry()->is_light()) {
      if (const auto it = _lightLinks.find(object->name.string()); it != _lightLinks.end()) {
        links = it->second;
      }
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
    const string lightgroup = TfStringStartsWith(sourceName, kLightgroupPrefix) ?
                                  sourceName.substr(strlen(kLightgroupPrefix)) :
                                  string();

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
      else if (sourceName != aovBinding.aovName) {
        pass->set_name(ustring(sourceName.GetString()));
        _passNames[aovBinding.aovName] = sourceName.GetString();
      }
    }
    if (lightgroup.empty()) {
      passOfType[type] = pass;
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
                                                   const float shutterClose) const
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
  const int numSteps = (1 << _motionSteps) + 1;
  if (_motionBlurDisabled || !(shutterClose > shutterOpen)) {
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
  if (!_ownCyclesSession) {
    return;  // The application owns the scene and its default shaders
  }

  Scene *const scene = session->scene.get();
  unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();

  // Wire up object color in default surface material
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

  HdCyclesMaterial::AddPrimvarAOVs(graph.get(), _primvarAovs);
  scene->default_surface->set_graph(std::move(graph));
  scene->default_surface->tag_update(scene);
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
