/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/mesh_light.h"
#include "hydra/session.h"
#include "hydra/util.h"
#include "kernel/types.h"
#include "scene/geometry.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "util/transform.h"

#include <pxr/imaging/hd/collectionsSchema.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/tokens.h>

#include <algorithm>

HDCYCLES_NAMESPACE_OPEN_SCOPE

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    ((childName, "__cyclesGeometryLight"))
    (noMaterialResponse)
    ((lightgroup, "inputs:cycles:light:lightgroup"))
    ((useMis, "inputs:cycles:light:use_mis"))
);
// clang-format on

namespace {

/* The light of a geometry light, null for other prims. */
HdContainerDataSourceHandle GetGeometryLight(const HdSceneIndexPrim &prim)
{
  if (!prim.dataSource || !HdPrimTypeIsGprim(prim.primType)) {
    return nullptr;
  }
  return HdLightSchema::GetFromParent(prim.dataSource).GetContainer();
}

/* World space area of the triangles, or of the faces of a subdivision mesh. */
float WorldArea(const Geometry *geom, const Transform &tfm)
{
  if (!geom->is_mesh()) {
    return 0.0f;
  }
  const Mesh *mesh = static_cast<const Mesh *>(geom);
  /* Subdivision meshes have their cage positions in the subd attributes, the triangles and their
   * positions only exist after tessellation. */
  const bool subdivision = mesh->get_num_subd_faces() > 0;
  const Attribute *attr_P = (subdivision ? mesh->subd_attributes : mesh->attributes)
                                .find(ATTR_STD_POSITION);
  if (!attr_P) {
    return 0.0f;
  }
  const packed_float3 *verts = attr_P->data<packed_float3>();
  const size_t numVerts = verts ? size_t(attr_P->size) : 0;
  const auto point = [&](const int i) { return transform_point(&tfm, float3(verts[i])); };
  const auto triangleArea = [&](const int a, const int b, const int c) {
    if (a < 0 || b < 0 || c < 0 || size_t(std::max({a, b, c})) >= numVerts) {
      return 0.0f;
    }
    const float3 pa = point(a);
    return 0.5f * len(cross(point(b) - pa, point(c) - pa));
  };
  float area = 0.0f;
  if (!subdivision) {
    const array<int> &triangles = mesh->get_triangles();
    for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
      area += triangleArea(triangles[i], triangles[i + 1], triangles[i + 2]);
    }
    return area;
  }
  /* The control cage of subdivision meshes, a fan per face. */
  const array<int> &corners = mesh->get_subd_face_corners();
  for (size_t f = 0; f < mesh->get_num_subd_faces(); ++f) {
    const Mesh::SubdFace face = mesh->get_subd_face(f);
    for (int c = 2; c < face.num_corners; ++c) {
      area += triangleArea(corners[face.start_corner],
                           corners[face.start_corner + c - 1],
                           corners[face.start_corner + c]);
    }
  }
  return area;
}

}  // namespace

const TfToken &GeometryLightChildName()
{
  return _tokens->childName;
}

HdCyclesGeometryLightSceneIndexRefPtr HdCyclesGeometryLightSceneIndex::New(
    const HdSceneIndexBaseRefPtr &inputSceneIndex)
{
  return TfCreateRefPtr(new HdCyclesGeometryLightSceneIndex(inputSceneIndex));
}

HdCyclesGeometryLightSceneIndex::HdCyclesGeometryLightSceneIndex(
    const HdSceneIndexBaseRefPtr &inputSceneIndex)
    : HdSingleInputFilteringSceneIndexBase(inputSceneIndex)
{
}

HdSceneIndexPrim HdCyclesGeometryLightSceneIndex::GetPrim(const SdfPath &primPath) const
{
  if (primPath.IsPrimPath() && primPath.GetNameToken() == _tokens->childName) {
    const HdSceneIndexPrim parent = _GetInputSceneIndex()->GetPrim(primPath.GetParentPath());
    if (const HdContainerDataSourceHandle light = GetGeometryLight(parent)) {
      const HdContainerDataSourceHandle collections =
          HdCollectionsSchema::GetFromParent(parent.dataSource).GetContainer();
      return {HdPrimTypeTokens->light,
              collections ?
                  HdRetainedContainerDataSource::New(HdLightSchema::GetSchemaToken(),
                                                     light,
                                                     HdCollectionsSchema::GetSchemaToken(),
                                                     collections) :
                  HdRetainedContainerDataSource::New(HdLightSchema::GetSchemaToken(), light)};
    }
  }
  return _GetInputSceneIndex()->GetPrim(primPath);
}

SdfPathVector HdCyclesGeometryLightSceneIndex::GetChildPrimPaths(const SdfPath &primPath) const
{
  SdfPathVector children = _GetInputSceneIndex()->GetChildPrimPaths(primPath);
  if (GetGeometryLight(_GetInputSceneIndex()->GetPrim(primPath))) {
    children.push_back(primPath.AppendChild(_tokens->childName));
  }
  return children;
}

void HdCyclesGeometryLightSceneIndex::_PrimsAdded(
    const HdSceneIndexBase & /*sender*/, const HdSceneIndexObserver::AddedPrimEntries &entries)
{
  HdSceneIndexObserver::AddedPrimEntries lights;
  HdSceneIndexObserver::RemovedPrimEntries removed;
  for (const HdSceneIndexObserver::AddedPrimEntry &entry : entries) {
    if (HdPrimTypeIsGprim(entry.primType) &&
        GetGeometryLight(_GetInputSceneIndex()->GetPrim(entry.primPath)))
    {
      _lights.insert(entry.primPath);
      lights.emplace_back(entry.primPath.AppendChild(_tokens->childName), HdPrimTypeTokens->light);
    }
    else if (_lights.erase(entry.primPath)) {
      /* Added again, no longer a light. */
      removed.emplace_back(entry.primPath.AppendChild(_tokens->childName));
    }
  }
  if (lights.empty()) {
    _SendPrimsAdded(entries);
  }
  else {
    HdSceneIndexObserver::AddedPrimEntries added = entries;
    added.insert(added.end(), lights.begin(), lights.end());
    _SendPrimsAdded(added);
  }
  if (!removed.empty()) {
    _SendPrimsRemoved(removed);
  }
}

void HdCyclesGeometryLightSceneIndex::_PrimsRemoved(
    const HdSceneIndexBase & /*sender*/, const HdSceneIndexObserver::RemovedPrimEntries &entries)
{
  /* Removing a prim removes its subtree, the child lights too. */
  for (const HdSceneIndexObserver::RemovedPrimEntry &entry : entries) {
    for (auto it = _lights.lower_bound(entry.primPath);
         it != _lights.end() && it->HasPrefix(entry.primPath);)
    {
      it = _lights.erase(it);
    }
  }
  _SendPrimsRemoved(entries);
}

void HdCyclesGeometryLightSceneIndex::_PrimsDirtied(
    const HdSceneIndexBase & /*sender*/, const HdSceneIndexObserver::DirtiedPrimEntries &entries)
{
  static const HdDataSourceLocatorSet childLocators = {HdLightSchema::GetDefaultLocator(),
                                                       HdCollectionsSchema::GetDefaultLocator()};
  HdSceneIndexObserver::DirtiedPrimEntries lights;
  for (const HdSceneIndexObserver::DirtiedPrimEntry &entry : entries) {
    if (entry.dirtyLocators.Intersects(childLocators) && _lights.count(entry.primPath)) {
      lights.emplace_back(entry.primPath.AppendChild(_tokens->childName), childLocators);
    }
  }
  if (lights.empty()) {
    _SendPrimsDirtied(entries);
    return;
  }
  HdSceneIndexObserver::DirtiedPrimEntries dirtied = entries;
  dirtied.insert(dirtied.end(), lights.begin(), lights.end());
  _SendPrimsDirtied(dirtied);
}

bool HdCyclesGeometryLight::Sync(HdCyclesSession *session,
                                 Scene *scene,
                                 HdSceneDelegate *sceneDelegate,
                                 const SdfPath &id,
                                 const HdContainerDataSourceHandle &primDs,
                                 Geometry *geom,
                                 const std::vector<Object *> &instances)
{
  /* The child light also has the light link categories (HdCyclesGeometryLightSceneIndex). */
  HdContainerDataSourceHandle lightDs = HdLightSchema::GetFromParent(primDs).GetContainer();
  if (lightDs) {
    if (HdContainerDataSourceHandle childDs =
            HdLightSchema::GetFromParent(
                GetPrim(sceneDelegate, id.AppendChild(_tokens->childName)).dataSource)
                .GetContainer())
    {
      lightDs = std::move(childDs);
    }
  }

  const bool isLight = lightDs != nullptr;
  if (isLight != _isLight) {
    _isLight = isLight;
    /* A light: no default lighting (HdCyclesSession::UpdateScene). */
    session->meshLights += isLight ? 1 : -1;
    session->lightsChanged = true;
    if (!isLight) {
      session->SetLightLinks(id.GetString(), TfToken(), TfToken());
    }
  }
  if (isLight) {
    session->SetLightLinks(id.GetString(),
                           GetTypedValue<TfToken>(lightDs, HdTokens->lightLink, TfToken()),
                           GetTypedValue<TfToken>(lightDs, HdTokens->shadowLink, TfToken()));
  }

  /* The materials of the geometry, as set before the light shader replaced them. */
  array<Node *> shaders = geom->get_used_shaders();
  _materialShaders.resize(shaders.size(), nullptr);
  for (size_t i = 0; i < shaders.size(); ++i) {
    if (shaders[i] != _shader) {
      _materialShaders[i] = shaders[i];
    }
  }
  const bool hasMaterial = std::any_of(
      _materialShaders.begin(), _materialShaders.end(), [&](const Node *shader) {
        return shader && shader != scene->default_surface;
      });

  /* Like Karma's Material Sync Mode: No Material Response emits the light instead of the
   * material. The other modes keep the material: its emission lights the scene. A geometry light
   * without a material emits the light. Volumes keep their shader. */
  const TfToken mode = GetTypedValue<TfToken>(lightDs, HdTokens->materialSyncMode, TfToken());
  const bool emit = isLight && !geom->is_volume() &&
                    (mode == _tokens->noMaterialResponse || !hasMaterial);

  if (emit) {
    /* Radiance like the other USD lights. */
    const GfVec3f color = GetTypedValue<GfVec3f>(lightDs, HdLightTokens->color, GfVec3f(1.0f));
    float3 strength = make_float3(color[0], color[1], color[2]) *
                      exp2f(GetTypedValue<float>(lightDs, HdLightTokens->exposure, 0.0f)) *
                      GetTypedValue<float>(lightDs, HdLightTokens->intensity, 1.0f);
    if (GetTypedValue<bool>(lightDs, HdLightTokens->normalize, false)) {
      /* shortcut: all instances are normalized by the area of the first one (the shader is
       * shared), give scaled instances shaders of their own if needed. Curves and points are not
       * normalized. */
      const float area = WorldArea(
          geom, instances.empty() ? transform_identity() : instances[0]->get_tfm());
      if (area > 0.0f) {
        strength /= area;
      }
    }
    const float temperature =
        GetTypedValue<bool>(lightDs, HdLightTokens->enableColorTemperature, false) ?
            GetTypedValue<float>(lightDs, HdLightTokens->colorTemperature, 6500.0f) :
            0.0f;
    const bool useMis = GetTypedValue<bool>(lightDs, _tokens->useMis, true);
    const float key[5] = {strength.x, strength.y, strength.z, temperature, useMis ? 1.0f : 0.0f};

    if (!_shader) {
      _shader = session->TakeShader(scene);
      _shader->name = id.GetString();
    }
    if (!std::equal(std::begin(key), std::end(key), std::begin(_key))) {
      std::copy(std::begin(key), std::end(key), std::begin(_key));
      unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();
      EmissionNode *emission = graph->create_node<EmissionNode>();
      emission->set_color(strength);
      emission->set_strength(1.0f);
      if (temperature > 0.0f) {
        /* Color Temperature multiplies the color, like the other lights. */
        BlackbodyNode *blackbody = graph->create_node<BlackbodyNode>();
        blackbody->set_temperature(temperature);
        VectorMathNode *multiply = graph->create_node<VectorMathNode>();
        multiply->set_math_type(NODE_VECTOR_MATH_MULTIPLY);
        multiply->set_vector2(strength);
        graph->connect(blackbody->output("Color"), multiply->input("Vector1"));
        graph->connect(multiply->output("Vector"), emission->input("Color"));
      }
      graph->connect(emission->output("Emission"), graph->output()->input("Surface"));
      _shader->set_emission_sampling_method(useMis ? EMISSION_SAMPLING_AUTO :
                                                     EMISSION_SAMPLING_NONE);
      _shader->set_graph(std::move(graph));
      _shader->tag_update(scene);
    }
    _shader->tag_used(scene);
  }

  bool changed = false;
  for (size_t i = 0; i < shaders.size(); ++i) {
    Node *const shader = emit ? _shader : _materialShaders[i];
    if (shader && shaders[i] != shader) {
      shaders[i] = shader;
      static_cast<Shader *>(shader)->tag_used(scene);
      changed = true;
    }
  }
  if (changed) {
    geom->set_used_shaders(shaders);
  }

  /* The light's light group (Cycles tab of the light), over the object's. */
  const std::string lightgroup = isLight ? GetTypedValue<std::string>(
                                               lightDs, _tokens->lightgroup, std::string()) :
                                           std::string();
  const bool restoreObjectSettings = lightgroup.empty() && _lightGroup;
  if (!lightgroup.empty()) {
    for (Object *instance : instances) {
      instance->set_lightgroup(ustring(lightgroup));
    }
  }
  _lightGroup = !lightgroup.empty();
  return restoreObjectSettings;
}

void HdCyclesGeometryLight::Finalize(HdCyclesSession *session, const bool keepNodes)
{
  if (_isLight) {
    --session->meshLights;
    session->lightsChanged = true;
    _isLight = false;
  }
  if (_shader && !keepNodes) {
    session->DeleteNode(_shader);
  }
  _shader = nullptr;
  _materialShaders.clear();
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
