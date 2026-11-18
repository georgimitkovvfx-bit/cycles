/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/cylinder_light.h"
#include "hydra/session.h"
#include "hydra/util.h"
#include "kernel/types.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "util/hash.h"
#include "util/transform.h"

#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <pxr/imaging/hd/xformSchema.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

extern Transform convert_transform(const GfMatrix4d &matrix);

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (visibleInPrimaryRay)
    ((karmaRenderLightGeo, "inputs:karma:light:renderlightgeo"))
    ((visibleCamera, "inputs:cycles:light:visible_camera"))
    ((visibleDiffuse, "inputs:cycles:light:visible_diffuse"))
    ((visibleGlossy, "inputs:cycles:light:visible_glossy"))
    ((visibleTransmission, "inputs:cycles:light:visible_transmission"))
    ((visibleVolumeScatter, "inputs:cycles:light:visible_volume_scatter"))
    ((useMis, "inputs:cycles:light:use_mis"))
    ((lightgroup, "inputs:cycles:light:lightgroup"))
);
// clang-format on

namespace {

/* Segments around the tube: smooth normals hide the facets, the silhouette is within 0.1%. */
constexpr int kSegments = 64;

}  // namespace

HdCyclesCylinderLight::HdCyclesCylinderLight(const SdfPath &sprimId) : HdLight(sprimId) {}

HdCyclesCylinderLight::~HdCyclesCylinderLight() = default;

HdDirtyBits HdCyclesCylinderLight::GetInitialDirtyBitsMask() const
{
  return DirtyBits::DirtyTransform | DirtyBits::DirtyParams | DirtyBits::DirtyCollection;
}

void HdCyclesCylinderLight::Sync(HdSceneDelegate *sceneDelegate,
                                 HdRenderParam *renderParam,
                                 HdDirtyBits *dirtyBits)
{
  if (*dirtyBits == DirtyBits::Clean) {
    return;
  }

  auto *const session = static_cast<HdCyclesSession *>(renderParam);
  const SceneLock lock(renderParam);

  if (!_object) {
    _mesh = lock.scene->create_node<Mesh>();
    _mesh->name = GetId().GetString();
    _shader = lock.scene->create_node<Shader>();
    array<Node *> usedShaders;
    usedShaders.push_back_slow(_shader);
    _mesh->set_used_shaders(usedShaders);

    _object = lock.scene->create_node<Object>();
    _object->name = GetId().GetString();
    _object->set_geometry(_mesh);
    _object->set_random_id(hash_uint2(hash_string(_object->name.c_str()), 0));
  }

  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdContainerDataSourceHandle &primDs = prim.dataSource;
  const HdContainerDataSourceHandle lightDs = HdLightSchema::GetFromParent(primDs).GetContainer();

  if (*dirtyBits & DirtyBits::DirtyTransform) {
    GfMatrix4d xform(1.0);
    if (auto matrixDs = HdXformSchema::GetFromParent(primDs).GetMatrix()) {
      xform = matrixDs->GetTypedValue(0.0f);
    }
    _object->set_tfm(convert_transform(xform));
  }

  if (*dirtyBits & (DirtyBits::DirtyParams | DirtyBits::DirtyCollection)) {
    session->SetLightLinks(_object->name.string(),
                           GetTypedValue<TfToken>(lightDs, HdTokens->lightLink, TfToken()),
                           GetTypedValue<TfToken>(lightDs, HdTokens->shadowLink, TfToken()));
  }

  if (*dirtyBits & (DirtyBits::DirtyParams | DirtyBits::DirtyTransform)) {
    const float radius = std::max(GetTypedValue<float>(lightDs, HdLightTokens->radius, 0.5f),
                                  0.0f);
    const float length = std::max(GetTypedValue<float>(lightDs, HdLightTokens->length, 1.0f),
                                  0.0f);

    /* The tube: kSegments quads around the X axis, outward normals. */
    _mesh->clear(true);
    _mesh->resize_mesh(2 * kSegments, 2 * kSegments);
    packed_float3 *P = _mesh->attributes.add(ATTR_STD_POSITION)->data_for_write<packed_float3>();
    for (int i = 0; i < kSegments; ++i) {
      const float angle = 2.0f * M_PI_F * float(i) / float(kSegments);
      const float y = radius * cosf(angle);
      const float z = radius * sinf(angle);
      P[2 * i + 0] = make_float3(-0.5f * length, y, z);
      P[2 * i + 1] = make_float3(0.5f * length, y, z);
    }
    int *triangles = _mesh->get_triangles().data();
    for (int i = 0; i < kSegments; ++i) {
      const int a = 2 * i;
      const int b = 2 * ((i + 1) % kSegments);
      /* Counter-clockwise seen from outside. */
      const int tri[6] = {a, b, a + 1, a + 1, b, b + 1};
      std::copy_n(tri, 6, triangles + 6 * i);
    }
    std::fill(_mesh->get_shader().begin(), _mesh->get_shader().end(), 0);
    std::fill(_mesh->get_smooth().begin(), _mesh->get_smooth().end(), true);
    _mesh->tag_position_modified();
    _mesh->tag_triangles_modified();
    _mesh->tag_shader_modified();
    _mesh->tag_smooth_modified();

    /* Radiance like the other USD lights; Cycles' emission strength is radiance. */
    const GfVec3f color = GetTypedValue<GfVec3f>(lightDs, HdLightTokens->color, GfVec3f(1.0f));
    float3 strength = make_float3(color[0], color[1], color[2]) *
                      exp2f(GetTypedValue<float>(lightDs, HdLightTokens->exposure, 0.0f)) *
                      GetTypedValue<float>(lightDs, HdLightTokens->intensity, 1.0f);
    if (GetTypedValue<bool>(lightDs, HdLightTokens->normalize, false)) {
      /* The world space area of the side. */
      const Transform &tfm = _object->get_tfm();
      const float scaleX = len(transform_get_column(&tfm, 0));
      const float scaleR = 0.5f * (len(transform_get_column(&tfm, 1)) +
                                   len(transform_get_column(&tfm, 2)));
      const float area = 2.0f * M_PI_F * radius * scaleR * length * scaleX;
      if (area > 0.0f) {
        strength /= area;
      }
    }
    bool visible = true;
    if (auto ds = HdVisibilitySchema::GetFromParent(primDs).GetVisibility()) {
      visible = ds->GetTypedValue(0.0f);
    }

    unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();
    EmissionNode *emission = graph->create_node<EmissionNode>();
    emission->set_color(visible ? strength : zero_float3());
    emission->set_strength(1.0f);
    graph->connect(emission->output("Emission"), graph->output()->input("Surface"));
    _shader->set_emission_sampling_method(GetTypedValue<bool>(lightDs, _tokens->useMis, true) ?
                                              EMISSION_SAMPLING_FRONT :
                                              EMISSION_SAMPLING_NONE);
    _shader->set_graph(std::move(graph));
    _shader->tag_update(lock.scene);
    _shader->tag_used(lock.scene);

    /* Like the other lights: invisible to the camera unless asked to, and it casts no shadows
     * of other lights. */
    uint visibility = 0;
    if (visible) {
      visibility = PATH_RAY_VISIBILITY_ALL &
                   ~(PATH_RAY_VISIBILITY_CAMERA | PATH_RAY_VISIBILITY_SHADOW);
      static const std::pair<TfToken, uint> rays[] = {
          {_tokens->visibleDiffuse, PATH_RAY_VISIBILITY_DIFFUSE},
          {_tokens->visibleGlossy, PATH_RAY_VISIBILITY_GLOSSY},
          {_tokens->visibleTransmission, PATH_RAY_VISIBILITY_TRANSMIT},
          {_tokens->visibleVolumeScatter, PATH_RAY_VISIBILITY_VOLUME_SCATTER},
      };
      for (const auto &[token, ray] : rays) {
        if (!GetTypedValue<bool>(lightDs, token, true)) {
          visibility &= ~ray;
        }
      }
      visibility &= LightMultiplierVisibility(lightDs);
      if (GetTypedValue<bool>(
              lightDs,
              _tokens->visibleCamera,
              GetTypedValue<bool>(
                  lightDs,
                  _tokens->visibleInPrimaryRay,
                  GetTypedValue<bool>(lightDs, _tokens->karmaRenderLightGeo, false))))
      {
        visibility |= PATH_RAY_VISIBILITY_CAMERA;
      }
    }
    _object->set_visibility(visibility);
    _object->set_lightgroup(
        ustring(GetTypedValue<std::string>(lightDs, _tokens->lightgroup, std::string())));
  }

  if (_mesh->is_modified()) {
    _mesh->tag_update(lock.scene, true);
  }
  if (_object->is_modified()) {
    _object->tag_update(lock.scene);
  }

  *dirtyBits = DirtyBits::Clean;
}

void HdCyclesCylinderLight::Finalize(HdRenderParam *renderParam)
{
  if (!_object) {
    return;
  }

  const SceneLock lock(renderParam);
  auto *const session = static_cast<HdCyclesSession *>(renderParam);
  session->RemoveLinking(_object->name.string());
  if (!session->keep_nodes) {
    session->DeleteNode(_object);
    session->DeleteNode(_mesh);
    session->DeleteNode(_shader);
  }
  _object = nullptr;
  _mesh = nullptr;
  _shader = nullptr;
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
