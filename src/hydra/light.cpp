/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/light.h"
#include "hydra/instancer.h"
#ifdef WITH_HOUDINI_IMAGES
#  include "hydra/houdini_image.h"
#endif
#include "hydra/session.h"
#include "hydra/util.h"
#include "kernel/types.h"
#include "scene/background.h"
#include "scene/light.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "util/hash.h"
#include "util/ies.h"
#include "util/path.h"
#include "util/transform.h"

#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/visibilitySchema.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usdLux/tokens.h>

#include <algorithm>

HDCYCLES_NAMESPACE_OPEN_SCOPE

extern Transform convert_transform(const GfMatrix4d &matrix);

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (visibleInPrimaryRay)
    ((karmaRenderLightGeo, "inputs:karma:light:renderlightgeo"))
    ((karmaSpread, "inputs:karma:light:spread"))
    (falloff)
    ((visibleCamera, "inputs:cycles:light:visible_camera"))
    ((useMis, "inputs:cycles:light:use_mis"))
    ((useCaustics, "inputs:cycles:light:use_caustics"))
    ((maxBounces, "inputs:cycles:light:max_bounces"))
    ((lightgroup, "inputs:cycles:light:lightgroup"))
    ((karmaLpeTag, "inputs:karma:light:lpetag"))
    ((worldMaterial, "inputs:cycles:light:world_material"))
    ((spread, "inputs:cycles:light:spread"))
    ((isPortal, "inputs:cycles:light:is_portal"))
    ((softFalloff, "inputs:cycles:light:use_soft_falloff"))
    ((mapResolution, "inputs:cycles:light:map_resolution"))
    ((worldExposure, "inputs:cycles:light:world_exposure"))
    ((worldSaturation, "inputs:cycles:light:world_saturation"))
    ((worldRotation, "inputs:cycles:light:world_rotation"))
    (cycles)
);
// clang-format on

/* A material bound to the light (Assign Material) with a Cycles network, empty for none. Others
 * are skipped, so a material assigned to the whole stage does not become the world. */
static std::string BoundWorldMaterial(HdSceneDelegate *sceneDelegate,
                                      const HdContainerDataSourceHandle &primDs)
{
  const HdMaterialBindingsSchema bindings = HdMaterialBindingsSchema::GetFromParent(primDs);
  HdMaterialBindingSchema binding = bindings.GetMaterialBinding(HdTokens->full);
  if (!binding) {
    binding = bindings.GetMaterialBinding();
  }
  const HdPathDataSourceHandle pathDs = binding.GetPath();
  if (!pathDs) {
    return "";
  }
  const SdfPath path = pathDs->GetTypedValue(0.0f);
  const HdSceneIndexPrim material = path.IsPrimPath() ? GetPrim(sceneDelegate, path) :
                                                        HdSceneIndexPrim();
  if (!HdMaterialSchema::GetFromParent(material.dataSource).GetMaterialNetwork(_tokens->cycles)) {
    return "";
  }
  return path.GetString();
}

HdCyclesLight::HdCyclesLight(const SdfPath &sprimId, const TfToken &lightType)
    : HdLight(sprimId), _lightType(lightType)
{
}

HdCyclesLight::~HdCyclesLight() = default;

HdDirtyBits HdCyclesLight::GetInitialDirtyBitsMask() const
{
  return DirtyBits::DirtyTransform | DirtyBits::DirtyParams | DirtyBits::DirtyCollection;
}

void HdCyclesLight::Sync(HdSceneDelegate *sceneDelegate,
                         HdRenderParam *renderParam,
                         HdDirtyBits *dirtyBits)
{
  if (*dirtyBits == DirtyBits::Clean) {
    return;
  }

  Initialize(renderParam);

  const SceneLock lock(renderParam);

  const SdfPath &id = GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
  const HdContainerDataSourceHandle &primDs = prim.dataSource;
  const HdContainerDataSourceHandle lightDs = HdLightSchema::GetFromParent(primDs).GetContainer();

  _instances.Sync(sceneDelegate, id, primDs, dirtyBits);
  if (*dirtyBits & DirtyBits::DirtyTransform) {
    _object->set_tfm(_instances.GetTransform());
  }

  // Light and shadow linking categories (HdsiLightLinkingSceneIndex), empty for all objects
  if (*dirtyBits & (DirtyBits::DirtyParams | DirtyBits::DirtyCollection)) {
    static_cast<HdCyclesSession *>(renderParam)
        ->SetLightLinks(_object->name.string(),
                        GetTypedValue<TfToken>(lightDs, HdTokens->lightLink, TfToken()),
                        GetTypedValue<TfToken>(lightDs, HdTokens->shadowLink, TfToken()));
  }

  /* Normalization depends on the light size in world space. */
  if (*dirtyBits & (DirtyBits::DirtyParams | DirtyBits::DirtyTransform)) {
    float3 strength = make_float3(1.0f, 1.0f, 1.0f);

    {
      const GfVec3f color = GetTypedValue<GfVec3f>(
          lightDs, HdLightTokens->color, GfVec3f(1.0f, 1.0f, 1.0f));
      strength = make_float3(color[0], color[1], color[2]);
    }

    strength *= exp2(GetTypedValue<float>(lightDs, HdLightTokens->exposure, 0.0f));
    strength *= GetTypedValue<float>(lightDs, HdLightTokens->intensity, 1.0f);
    if (_lightType == HdPrimTypeTokens->distantLight) {
      strength *= static_cast<HdCyclesSession *>(renderParam)->GetSunIntensityScale();
    }

    if (_lightType != HdPrimTypeTokens->domeLight && _lightType != HdPrimTypeTokens->distantLight)
    {
      /* Convert from intensity to radiant flux. */
      strength *= M_PI_F;
    }

    /* We dont use Blender's normalization, but calculate our own values to match USDLux.
    Normalization code is taken from:
    https://openusd.org/dev/user_guides/schemas/usdLux/LightAPI.html#lightapi-inputs-normalize */
    bool normalize = GetTypedValue<bool>(lightDs, HdLightTokens->normalize, false);

    float area = 1.0f;

    _light->set_normalize(false);

    // Cycles light settings from "inputs:cycles:light:<setting>" attributes, like Blender's light
    // and object Visibility panels; unauthored settings use Blender's defaults, except that the
    // camera doesn't see lights unless asked to, like with Karma.
    static const std::pair<TfToken, uint> rayVisibility[] = {
        {TfToken("inputs:cycles:light:visible_diffuse"), PATH_RAY_VISIBILITY_DIFFUSE},
        {TfToken("inputs:cycles:light:visible_glossy"), PATH_RAY_VISIBILITY_GLOSSY},
        {TfToken("inputs:cycles:light:visible_transmission"), PATH_RAY_VISIBILITY_TRANSMIT},
        {TfToken("inputs:cycles:light:visible_volume_scatter"),
         PATH_RAY_VISIBILITY_VOLUME_SCATTER},
    };
    uint visibility = PATH_RAY_VISIBILITY_ALL & ~PATH_RAY_VISIBILITY_CAMERA;
    if (GetTypedValue<bool>(
            lightDs,
            _tokens->visibleCamera,
            GetTypedValue<bool>(
                lightDs,
                _tokens->visibleInPrimaryRay,
                GetTypedValue<bool>(
                    lightDs,
                    _tokens->karmaRenderLightGeo,
                    _lightType == HdPrimTypeTokens->domeLight &&
                        static_cast<HdCyclesSession *>(renderParam)
                            ->GetDomeCameraVisibleDefault()))))
    {
      visibility |= PATH_RAY_VISIBILITY_CAMERA;
    }
    // Karma's light Contributions for the rays without a Cycles setting
    const uint karmaRays = KarmaLightContributions(lightDs);
    for (const auto &[token, ray] : rayVisibility) {
      if (!GetTypedValue<bool>(lightDs, token, (karmaRays & ray) != 0)) {
        visibility &= ~ray;
      }
    }
    visibility &= LightMultiplierVisibility(lightDs);
    _object->set_visibility(visibility);
    const bool useMis = GetTypedValue<bool>(lightDs, _tokens->useMis, true);
    if (_lightType == HdPrimTypeTokens->domeLight) {
      static_cast<HdCyclesSession *>(renderParam)->SetDomeUseMis(_light, useMis);
    }
    else {
      _light->set_use_mis(useMis);
    }
    _light->set_use_caustics(GetTypedValue<bool>(lightDs, _tokens->useCaustics, false));
    _light->set_max_bounces(GetTypedValue<int>(lightDs, _tokens->maxBounces, 1024));

    // Karma's LPE tag (light group AOVs) when there is no Cycles light group, as Karma's other light
    // settings
    const ustring lightgroup(GetTypedValue<std::string>(
        lightDs, _tokens->lightgroup, GetTypedValue<std::string>(lightDs, _tokens->karmaLpeTag, "")));
    _object->set_lightgroup(lightgroup);
    /* UpdateScene() gives the world the light group of the first dome light. */

    if (auto ds = GetTypedDataSource<bool>(lightDs, HdLightTokens->shadowEnable)) {
      _light->set_cast_shadow(ds->GetTypedValue(0.0f));
    }

    if (_lightType == HdPrimTypeTokens->domeLight) {
      /* Blender's world Map Resolution, 0 is Auto (from the environment texture). */
      static_cast<BackgroundLight *>(_light)->set_map_resolution(
          max(GetTypedValue<int>(lightDs, _tokens->mapResolution, 0), 0));
      _worldExposure = GetTypedValue<float>(lightDs, _tokens->worldExposure, 0.0f);
      _worldSaturation = max(GetTypedValue<float>(lightDs, _tokens->worldSaturation, 1.0f), 0.0f);
      _worldRotation = GfDegreesToRadians(
          GetTypedValue<float>(lightDs, _tokens->worldRotation, 0.0f));
    }
    else if (_lightType == HdPrimTypeTokens->distantLight) {
      const float angleDeg = GetTypedValue<float>(lightDs, HdLightTokens->angle, 0.53f);
      const float angleRad = GfDegreesToRadians(angleDeg);
      static_cast<SunLight *>(_light)->set_angle(angleRad);

      const float max_theta = clamp(angleRad / 2.0f, 0.0f, M_PI_F);

      if (max_theta == 0.0f) {
        area = 1.0f;
      }
      else if (max_theta <= M_PI_2_F) {
        area = powf(sinf(max_theta), 2.0f) * M_PI_F;
      }
      else {
        area = (2.0f - powf(sinf(max_theta), 2.0f)) * M_PI_F;
      }
    }
    else if (_lightType == HdPrimTypeTokens->diskLight ||
             _lightType == HdPrimTypeTokens->rectLight)
    {
      AreaLight *area_light = static_cast<AreaLight *>(_light);

      /* Without a Cycles spread, Karma's 0 (parallel beams) to 1 (diffuse) as 0 to 180 degrees.
       * shortcut: linear, Karma's own curve between the ends is not documented. */
      area_light->set_spread(GfDegreesToRadians(clamp(
          GetTypedValue<float>(lightDs,
                               _tokens->spread,
                               180.0f * GetTypedValue<float>(lightDs, _tokens->karmaSpread, 1.0f)),
          0.0f,
          180.0f)));
      area_light->set_is_portal(GetTypedValue<bool>(lightDs, _tokens->isPortal, false));

      if (_lightType == HdPrimTypeTokens->diskLight) {
        const float radius = GetTypedValue<float>(lightDs, HdLightTokens->radius, 0.5f);
        const float size = radius * 2.0f;
        area_light->set_sizeu(size);
        area_light->set_sizev(size);
      }
      else {
        const float width = GetTypedValue<float>(lightDs, HdLightTokens->width, 1.0f);
        const float height = GetTypedValue<float>(lightDs, HdLightTokens->height, 1.0f);
        area_light->set_sizeu(width);
        area_light->set_sizev(height);
      }

      area = area_light->area(_object->get_tfm());
    }
    else if (_lightType == HdPrimTypeTokens->sphereLight) {
      SpotLight *spot_light = static_cast<SpotLight *>(_light);
      const bool treatAsPoint = GetTypedValue<bool>(lightDs, UsdLuxTokens->treatAsPoint, false);
      const float radius = GetTypedValue<float>(lightDs, HdLightTokens->radius, 0.5f);

      const Transform &tfm = _object->get_tfm();
      const float scale_x = len(transform_get_column(&tfm, 0));
      const float scale_y = len(transform_get_column(&tfm, 1));
      const float scale_z = len(transform_get_column(&tfm, 2));
      const float avg_scale = (scale_x + scale_y + scale_z) / 3.0f;

      if (treatAsPoint || radius == 0.0f) {
        spot_light->set_radius(0.0f);
        area = 1.0f;
      }
      else {
        const float world_radius = radius * avg_scale;
        spot_light->set_radius(world_radius);
        area = 4.0f * M_PI_F * powf(world_radius, 2.0f);
      }

      spot_light->set_smooth(0.0f);
      /* Blender's Soft Falloff, off by default: a USD sphere light is a sphere, like in Karma. */
      spot_light->set_is_sphere(!GetTypedValue<bool>(lightDs, _tokens->softFalloff, false));
      _light->set_light_type(LIGHT_POINT);
    }

    if (normalize) {
      if (area != 0) {
        strength /= area;
      }
    }

    bool visible = true;
    if (auto ds = HdVisibilitySchema::GetFromParent(primDs).GetVisibility()) {
      visible = ds->GetTypedValue(0.0f);
    }
    visible &= !_instances.IsEmpty();
    /* Disable invisible lights by zeroing the strength
    So 'LightManager::test_enabled_lights' updates the enabled flag correctly */
    if (!visible) {
      strength = zero_float3();
    }

    if (_lightType == HdPrimTypeTokens->domeLight) {
      /* The world shader holds the dome's color and intensity. The kernel also multiplies the
       * light strength into sampled world light, so keep it at one (zero: hidden). */
      _domeStrength = strength;
      _light->set_strength(strength != zero_float3() ? one_float3() : zero_float3());
    }
    else {
      _light->set_strength(strength);
    }
    _light->set_is_enabled(visible);

    PopulateShaderGraph(lightDs, renderParam);
  }
  /* Need to update shader graph when transform changes in case transform was baked into it */
  else if (_object->tfm_is_modified() && (_lightType == HdPrimTypeTokens->domeLight ||
                                          _shader->has_surface_spatial_varying))
  {
    PopulateShaderGraph(lightDs, renderParam);
  }

  if (_lightType == HdPrimTypeTokens->domeLight &&
      (*dirtyBits & (DirtyBits::DirtyTransform | DirtyBits::DirtyParams)))
  {
    /* A material as the world, like in Blender: its surface is the background, its volume fills
     * the scene. Sky and environment textures follow the light's rotation. The World Material
     * setting wins over a material bound to the light. */
    std::string material = GetTypedValue<std::string>(lightDs, _tokens->worldMaterial, "");
    if (material.empty()) {
      material = BoundWorldMaterial(sceneDelegate, primDs);
    }
    Transform rotation = _object->get_tfm();
    for (int i = 0; i < 3; i++) {
      transform_set_column(&rotation, i, normalize(transform_get_column(&rotation, i)));
    }
    transform_set_column(&rotation, 3, zero_float3());
    rotation = rotation * transform_rotate(_worldRotation, make_float3(0.0f, 1.0f, 0.0f));
    static_cast<HdCyclesSession *>(renderParam)
        ->SetWorldMaterial(_light,
                           {SdfPath::IsValidPathString(material) ? material : "",
                            rotation,
                            _shader,
                            _worldExposure,
                            _worldSaturation});
  }

  if (_light->is_modified()) {
    _light->tag_update(lock.scene);
  }
  _instances.Update(lock.scene,
                    static_cast<HdCyclesSession *>(renderParam),
                    _object,
                    GetTypedValue<bool>(lightDs, HdLightTokens->normalize, false));
  // Light position and direction come from the object transform
  if (_object->is_modified()) {
    _object->tag_update(lock.scene);
  }

  *dirtyBits = DirtyBits::Clean;
}

static void BuildIESNodes(ShaderGraph *graph,
                          IESLightNode *iesNode,
                          float angleScale,
                          ShaderNode *outputNode)
{
  /* Transform Incoming to local light space and normalize */
  GeometryNode *geomNode = graph->create_node<GeometryNode>();
  VectorTransformNode *xformNode = graph->create_node<VectorTransformNode>();
  xformNode->set_transform_type(NODE_VECTOR_TRANSFORM_TYPE_VECTOR);
  xformNode->set_convert_from(NODE_VECTOR_TRANSFORM_CONVERT_SPACE_WORLD);
  xformNode->set_convert_to(NODE_VECTOR_TRANSFORM_CONVERT_SPACE_OBJECT);
  graph->connect(geomNode->output("Incoming"), xformNode->input("Vector"));

  VectorMathNode *normNode = graph->create_node<VectorMathNode>();
  normNode->set_math_type(NODE_VECTOR_MATH_NORMALIZE);
  graph->connect(xformNode->output("Vector"), normNode->input("Vector1"));

  SeparateXYZNode *sepNode = graph->create_node<SeparateXYZNode>();
  graph->connect(normNode->output("Vector"), sepNode->input("Vector"));

  ShaderOutput *outZ = sepNode->output("Z");
  ShaderOutput *iesFacOutput = iesNode->output("Fac");

  if (angleScale != 0.0f && angleScale != 1.0f) {
    /* In local light space, emission axis is -Z -> cos(theta) = -Z */
    MathNode *negZNode = graph->create_node<MathNode>();
    negZNode->set_math_type(NODE_MATH_MULTIPLY);
    negZNode->set_value2(-1.0f);
    graph->connect(sepNode->output("Z"), negZNode->input("Value1"));

    ClampNode *clampCosNode = graph->create_node<ClampNode>();
    clampCosNode->set_min(-1.0f);
    clampCosNode->set_max(1.0f);
    graph->connect(negZNode->output("Value"), clampCosNode->input("Value"));

    MathNode *acosNode = graph->create_node<MathNode>();
    acosNode->set_math_type(NODE_MATH_ARCCOSINE);
    graph->connect(clampCosNode->output("Result"), acosNode->input("Value1"));

    ShaderOutput *scaledThetaOutput = nullptr;
    if (angleScale > 0.0f) {
      MathNode *scaleThetaNode = graph->create_node<MathNode>();
      scaleThetaNode->set_math_type(NODE_MATH_MULTIPLY);
      scaleThetaNode->set_value2(1.0f / angleScale);
      graph->connect(acosNode->output("Value"), scaleThetaNode->input("Value1"));
      scaledThetaOutput = scaleThetaNode->output("Value");
    }
    else {
      MathNode *scaleThetaNode = graph->create_node<MathNode>();
      scaleThetaNode->set_math_type(NODE_MATH_MULTIPLY);
      scaleThetaNode->set_value2(1.0f / -angleScale);
      graph->connect(acosNode->output("Value"), scaleThetaNode->input("Value1"));

      MathNode *addThetaNode = graph->create_node<MathNode>();
      addThetaNode->set_math_type(NODE_MATH_ADD);
      addThetaNode->set_value2(M_PI_F * (1.0f - 1.0f / -angleScale));
      graph->connect(scaleThetaNode->output("Value"), addThetaNode->input("Value1"));
      scaledThetaOutput = addThetaNode->output("Value");
    }

    ClampNode *clampThetaNode = graph->create_node<ClampNode>();
    clampThetaNode->set_min(0.0f);
    clampThetaNode->set_max(M_PI_F - 0.005f);
    graph->connect(scaledThetaOutput, clampThetaNode->input("Value"));

    /* New Z = -cos(theta_ies) */
    MathNode *cosNode = graph->create_node<MathNode>();
    cosNode->set_math_type(NODE_MATH_COSINE);
    graph->connect(clampThetaNode->output("Result"), cosNode->input("Value1"));

    MathNode *newZNode = graph->create_node<MathNode>();
    newZNode->set_math_type(NODE_MATH_MULTIPLY);
    newZNode->set_value2(-1.0f);
    graph->connect(cosNode->output("Value"), newZNode->input("Value1"));
    outZ = newZNode->output("Value");

    /* New XY magnitude = sin(theta_ies) */
    MathNode *sinNode = graph->create_node<MathNode>();
    sinNode->set_math_type(NODE_MATH_SINE);
    graph->connect(clampThetaNode->output("Result"), sinNode->input("Value1"));

    CombineXYZNode *xyVecNode = graph->create_node<CombineXYZNode>();
    xyVecNode->set_z(0.0f);
    graph->connect(sepNode->output("X"), xyVecNode->input("X"));
    graph->connect(sepNode->output("Y"), xyVecNode->input("Y"));

    VectorMathNode *normXYNode = graph->create_node<VectorMathNode>();
    normXYNode->set_math_type(NODE_VECTOR_MATH_NORMALIZE);
    graph->connect(xyVecNode->output("Vector"), normXYNode->input("Vector1"));

    VectorMathNode *scaleXYNode = graph->create_node<VectorMathNode>();
    scaleXYNode->set_math_type(NODE_VECTOR_MATH_SCALE);
    graph->connect(normXYNode->output("Vector"), scaleXYNode->input("Vector1"));
    graph->connect(sinNode->output("Value"), scaleXYNode->input("Scale"));

    SeparateXYZNode *scaledSepNode = graph->create_node<SeparateXYZNode>();
    graph->connect(scaleXYNode->output("Vector"), scaledSepNode->input("Vector"));
    sepNode = scaledSepNode;

    if (angleScale < 0.0f) {
      /* For negative angleScale, mask out directions where scaled theta < 0 */
      MathNode *gteZeroNode = graph->create_node<MathNode>();
      gteZeroNode->set_math_type(NODE_MATH_GREATER_THAN);
      gteZeroNode->set_value2(-1e-4f);
      graph->connect(scaledThetaOutput, gteZeroNode->input("Value1"));

      MathNode *maskedFacNode = graph->create_node<MathNode>();
      maskedFacNode->set_math_type(NODE_MATH_MULTIPLY);
      graph->connect(iesNode->output("Fac"), maskedFacNode->input("Value1"));
      graph->connect(gteZeroNode->output("Value"), maskedFacNode->input("Value2"));
      iesFacOutput = maskedFacNode->output("Value");
    }
  }

  /* Align USD Lux photometric coordinate frame with Cycles IES node (Cycles: X = Y_usd, Y = X_usd,
   * Z = Z_usd) */
  CombineXYZNode *finalVecNode = graph->create_node<CombineXYZNode>();
  graph->connect(sepNode->output("Y"), finalVecNode->input("X"));
  graph->connect(sepNode->output("X"), finalVecNode->input("Y"));
  graph->connect(outZ, finalVecNode->input("Z"));

  graph->connect(finalVecNode->output("Vector"), iesNode->input("Vector"));

  ShaderInput *const strengthInput = outputNode->input("Strength");
  if (strengthInput) {
    if (strengthInput->link) {
      ShaderOutput *const previousOutput = strengthInput->link;
      graph->disconnect(strengthInput);

      MathNode *mulNode = graph->create_node<MathNode>();
      mulNode->set_math_type(NODE_MATH_MULTIPLY);
      graph->connect(previousOutput, mulNode->input("Value1"));
      graph->connect(iesFacOutput, mulNode->input("Value2"));
      graph->connect(mulNode->output("Value"), strengthInput);
    }
    else {
      graph->connect(iesFacOutput, strengthInput);
    }
  }
}

static void BuildConeShapingNodes(ShaderGraph *graph,
                                  float coneAngleDeg,
                                  float coneSoftness,
                                  ShaderNode *outputNode)
{
  const float clampedHalfAngleDeg = clamp(coneAngleDeg, 0.001f, 180.0f);
  const float thetaCutoff = GfDegreesToRadians(clampedHalfAngleDeg);
  const float clampedSoftness = clamp(coneSoftness, 0.0f, 1.0f);
  const float thetaSmoothStart = (1.0f - clampedSoftness) * thetaCutoff;

  GeometryNode *geomNode = graph->create_node<GeometryNode>();

  /* Transform Incoming from world space to object (light) space */
  VectorTransformNode *xformNode = graph->create_node<VectorTransformNode>();
  xformNode->set_transform_type(NODE_VECTOR_TRANSFORM_TYPE_VECTOR);
  xformNode->set_convert_from(NODE_VECTOR_TRANSFORM_CONVERT_SPACE_WORLD);
  xformNode->set_convert_to(NODE_VECTOR_TRANSFORM_CONVERT_SPACE_OBJECT);
  graph->connect(geomNode->output("Incoming"), xformNode->input("Vector"));

  VectorMathNode *normNode = graph->create_node<VectorMathNode>();
  normNode->set_math_type(NODE_VECTOR_MATH_NORMALIZE);
  graph->connect(xformNode->output("Vector"), normNode->input("Vector1"));

  SeparateXYZNode *sepNode = graph->create_node<SeparateXYZNode>();
  graph->connect(normNode->output("Vector"), sepNode->input("Vector"));

  /* In local light space, emission axis is -Z. cos(theta) = -Z */
  MathNode *negZNode = graph->create_node<MathNode>();
  negZNode->set_math_type(NODE_MATH_MULTIPLY);
  negZNode->set_value2(-1.0f);
  graph->connect(sepNode->output("Z"), negZNode->input("Value1"));

  ClampNode *clampCosNode = graph->create_node<ClampNode>();
  clampCosNode->set_min(-1.0f);
  clampCosNode->set_max(1.0f);
  graph->connect(negZNode->output("Value"), clampCosNode->input("Value"));

  MathNode *acosNode = graph->create_node<MathNode>();
  acosNode->set_math_type(NODE_MATH_ARCCOSINE);
  graph->connect(clampCosNode->output("Result"), acosNode->input("Value1"));

  ShaderOutput *coneFacOutput = nullptr;

  if (clampedSoftness > 0.0f && thetaSmoothStart < thetaCutoff) {
    MapRangeNode *mapRangeNode = graph->create_node<MapRangeNode>();
    mapRangeNode->set_range_type(NODE_MAP_RANGE_SMOOTHSTEP);
    mapRangeNode->set_from_min(thetaSmoothStart);
    mapRangeNode->set_from_max(thetaCutoff);
    mapRangeNode->set_to_min(1.0f);
    mapRangeNode->set_to_max(0.0f);
    mapRangeNode->set_clamp(true);
    graph->connect(acosNode->output("Value"), mapRangeNode->input("Value"));
    coneFacOutput = mapRangeNode->output("Result");
  }
  else {
    MathNode *compNode = graph->create_node<MathNode>();
    compNode->set_math_type(NODE_MATH_GREATER_THAN);
    compNode->set_value1(thetaCutoff + 1e-5f);
    graph->connect(acosNode->output("Value"), compNode->input("Value2"));
    coneFacOutput = compNode->output("Value");
  }

  ShaderInput *const strengthInput = outputNode->input("Strength");
  if (strengthInput) {
    if (strengthInput->link) {
      ShaderOutput *const previousOutput = strengthInput->link;
      graph->disconnect(strengthInput);

      MathNode *mulNode = graph->create_node<MathNode>();
      mulNode->set_math_type(NODE_MATH_MULTIPLY);
      graph->connect(previousOutput, mulNode->input("Value1"));
      graph->connect(coneFacOutput, mulNode->input("Value2"));
      graph->connect(mulNode->output("Value"), strengthInput);
    }
    else {
      graph->connect(coneFacOutput, strengthInput);
    }
  }
}

static void BuildFocusShapingNodes(ShaderGraph *graph,
                                   float focusShaping,
                                   float3 focusTint,
                                   ShaderNode *outputNode)
{
  GeometryNode *geometryNode = graph->create_node<GeometryNode>();

  /* Transform Incoming from world space to object (light) space */
  VectorTransformNode *xformNode = graph->create_node<VectorTransformNode>();
  xformNode->set_transform_type(NODE_VECTOR_TRANSFORM_TYPE_VECTOR);
  xformNode->set_convert_from(NODE_VECTOR_TRANSFORM_CONVERT_SPACE_WORLD);
  xformNode->set_convert_to(NODE_VECTOR_TRANSFORM_CONVERT_SPACE_OBJECT);
  graph->connect(geometryNode->output("Incoming"), xformNode->input("Vector"));

  /* Normalize to ensure unit length regardless of object scale. */
  VectorMathNode *normNode = graph->create_node<VectorMathNode>();
  normNode->set_math_type(NODE_VECTOR_MATH_NORMALIZE);
  graph->connect(xformNode->output("Vector"), normNode->input("Vector1"));

  SeparateXYZNode *sepNode = graph->create_node<SeparateXYZNode>();
  graph->connect(normNode->output("Vector"), sepNode->input("Vector"));

  /* UsdLux defines focus with the absolute value of the dot product, so sphere and cylinder
   * lights also emit behind the light. */
  MathNode *absoluteNode = graph->create_node<MathNode>();
  absoluteNode->set_math_type(NODE_MATH_ABSOLUTE);
  graph->connect(sepNode->output("Z"), absoluteNode->input("Value1"));

  ClampNode *clampCosNode = graph->create_node<ClampNode>();
  clampCosNode->set_min(0.0f);
  clampCosNode->set_max(1.0f);
  graph->connect(absoluteNode->output("Value"), clampCosNode->input("Value"));

  MathNode *powerNode = graph->create_node<MathNode>();
  powerNode->set_math_type(NODE_MATH_POWER);
  powerNode->set_value2(focusShaping);
  graph->connect(clampCosNode->output("Result"), powerNode->input("Value1"));

  ClampNode *clampFacNode = graph->create_node<ClampNode>();
  clampFacNode->set_min(0.0f);
  clampFacNode->set_max(1.0f);
  graph->connect(powerNode->output("Value"), clampFacNode->input("Value"));

  MixNode *mixNode = graph->create_node<MixNode>();
  mixNode->set_mix_type(NODE_MIX_BLEND);
  mixNode->set_use_clamp(true);

  graph->connect(clampFacNode->output("Result"), mixNode->input("Fac"));

  /* focusColor = lerp(focusFactor, focusTint, [1, 1, 1])
  In MixNode: result = (1 - Fac) * Color1 + Fac * Color2 */
  mixNode->set_color1(focusTint);
  mixNode->set_color2(make_float3(1.0f, 1.0f, 1.0f));

  /* Inject focusColor before outputNode's Color input (multiply by existing color) */
  ShaderInput *const targetColorInput = outputNode->input("Color");
  if (targetColorInput) {
    if (targetColorInput->link) {
      ShaderOutput *const previousOutput = targetColorInput->link;
      graph->disconnect(targetColorInput);

      VectorMathNode *mulNode = graph->create_node<VectorMathNode>();
      mulNode->set_math_type(NODE_VECTOR_MATH_MULTIPLY);
      graph->connect(previousOutput, mulNode->input("Vector1"));
      graph->connect(mixNode->output("Color"), mulNode->input("Vector2"));
      graph->connect(mulNode->output("Vector"), targetColorInput);
    }
    else {
      graph->connect(mixNode->output("Color"), targetColorInput);
    }
  }
}

void HdCyclesLight::PopulateShaderGraph(const HdContainerDataSourceHandle &lightContainer,
                                        HdRenderParam *renderParam)
{
  unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();
  ShaderNode *outputNode = nullptr;

  if (_lightType == HdPrimTypeTokens->domeLight) {
    BackgroundNode *bgNode = graph->create_node<BackgroundNode>();
    // Bake strength into shader graph, since only the shader is used for background lights
    bgNode->set_color(_domeStrength);

    graph->connect(bgNode->output("Background"), graph->output()->input("Surface"));

    outputNode = bgNode;
  }
  else {
    EmissionNode *emissionNode = graph->create_node<EmissionNode>();
    emissionNode->set_color(one_float3());
    emissionNode->set_strength(1.0f);

    graph->connect(emissionNode->output("Emission"), graph->output()->input("Surface"));

    if (lightContainer) {
      if (auto ds = HdStringDataSource::Cast(lightContainer->Get(_tokens->falloff))) {
        const std::string strVal = ds->GetTypedValue(0.0f);
        if (strVal == "Constant" || strVal == "Linear" || strVal == "Quadratic") {
          LightFalloffNode *lfoNode = graph->create_node<LightFalloffNode>();
          lfoNode->set_strength(1.0f);
          graph->connect(lfoNode->output(strVal.c_str()), emissionNode->input("Strength"));
        }
      }
    }

    outputNode = emissionNode;
  }

  bool hasSpatialVarying = false;
  bool hasColorTemperature = false;

  if (lightContainer) {
    const bool enableColorTemperature = GetTypedValue<bool>(
        lightContainer, HdLightTokens->enableColorTemperature, false);

    if (enableColorTemperature) {
      if (auto ds = HdFloatDataSource::Cast(lightContainer->Get(HdLightTokens->colorTemperature)))
      {
        BlackbodyNode *blackbodyNode = graph->create_node<BlackbodyNode>();
        blackbodyNode->set_temperature(ds->GetTypedValue(0.0f));

        if (_lightType == HdPrimTypeTokens->domeLight) {
          VectorMathNode *mathNode = graph->create_node<VectorMathNode>();
          mathNode->set_math_type(NODE_VECTOR_MATH_MULTIPLY);
          mathNode->set_vector2(_domeStrength);

          graph->connect(blackbodyNode->output("Color"), mathNode->input("Vector1"));
          graph->connect(mathNode->output("Vector"), outputNode->input("Color"));
        }
        else {
          graph->connect(blackbodyNode->output("Color"), outputNode->input("Color"));
        }

        hasColorTemperature = true;
      }
    }

    if (auto ds = HdAssetPathDataSource::Cast(lightContainer->Get(HdLightTokens->shapingIesFile)))
    {
      const SdfAssetPath assetPath = ds->GetTypedValue(0.0f);
      std::string filename = assetPath.GetResolvedPath();
      if (filename.empty()) {
        filename = assetPath.GetAssetPath();
      }

      if (!filename.empty()) {
        IESLightNode *iesNode = graph->create_node<IESLightNode>();
        iesNode->set_filename(ustring(filename));

        bool iesNormalize = false;
        if (auto ds = GetTypedDataSource<bool>(lightContainer, HdLightTokens->shapingIesNormalize))
        {
          iesNormalize = ds->GetTypedValue(false);
        }

        float iesStrength = 1.0f;
        bool didNormalize = false;

        /* USDLux requires the intensity of IES lights to be normalized
         * We calculate an iesStrength factor to use to normalize the IES light intensity
         * (total_power) 1 (normalized intensity) = 4pi (surface area of a normalized sphere) /
         * iesStrength */
        if (iesNormalize) {
          string content;
          if (path_read_text(filename, content)) {
            IESFile ies;
            if (ies.load(content)) {
              const float total_power = ies.integrate();
              if (total_power > 0.0f) {
                iesStrength = (4.0f * M_PI_F) / total_power;
                didNormalize = true;
              }
            }
          }
        }

        /* When normalization not requested or failed, invert Cycles' internal factor to use raw
         * photometric candelas */
        if (!didNormalize) {
          iesStrength = 1.0f / 0.0706650768394f;
        }

        iesNode->set_strength(iesStrength);

        float angleScale = 0.0f;
        if (auto ds = GetTypedDataSource<float>(lightContainer,
                                                HdLightTokens->shapingIesAngleScale))
        {
          angleScale = ds->GetTypedValue(0.0f);
        }

        BuildIESNodes(graph.get(), iesNode, angleScale, outputNode);

        hasSpatialVarying = true;
      }
    }

    if (auto ds = HdAssetPathDataSource::Cast(lightContainer->Get(HdLightTokens->textureFile))) {
      const SdfAssetPath assetPath = ds->GetTypedValue(0.0f);
      std::string filename = assetPath.GetResolvedPath();
      if (filename.empty()) {
        filename = assetPath.GetAssetPath();
      }
#ifdef WITH_HOUDINI_IMAGES
      filename = HdCycles::ResolveHoudiniImage(filename);
#endif

      if (!filename.empty()) {
        ImageSlotTextureNode *textureNode = nullptr;
        if (_lightType == HdPrimTypeTokens->domeLight) {
          Transform tfm = _object->get_tfm();
          transform_set_column(&tfm, 3, zero_float3());  // Remove translation
          /* For DomeLight_1, UsdImaging resolves poleAxis ("scene", "Y", "Z") and the stage's
           * upAxis into a GfMatrix4d under HdLightTokens->domeOffset (+90 deg x rotation when
           * Z-up) */
          if (auto offsetDs = HdMatrixDataSource::Cast(
                  lightContainer->Get(HdLightTokens->domeOffset)))
          {
            const GfMatrix4d domeOffset = offsetDs->GetTypedValue(0.0f);

            tfm = tfm * convert_transform(domeOffset);
          }

          /* Rotate from Blender's Z-up to USD's default Y-up (cancels with domeOffset when Z-up)
           */
          tfm = tfm * transform_rotate(-M_PI_2_F, make_float3(1.0f, 0.0f, 0.0f));

          /* We then spin around Blender's Z-up to align with USDLux */
          tfm = tfm * transform_rotate(-M_PI_2_F, make_float3(0.0f, 0.0f, 1.0f));
          /* The Cycles tab's Rotation, around the texture's pole (the light's +Y). */
          tfm = tfm * transform_rotate(_worldRotation, make_float3(0.0f, 0.0f, 1.0f));

          TextureCoordinateNode *coordNode = graph->create_node<TextureCoordinateNode>();
          coordNode->set_ob_tfm(tfm);
          coordNode->set_use_transform(true);

          textureNode = graph->create_node<EnvironmentTextureNode>();
          static_cast<EnvironmentTextureNode *>(textureNode)->set_filename(ustring(filename));

          graph->connect(coordNode->output("Object"), textureNode->input("Vector"));

          hasSpatialVarying = true;
        }
        else {
          GeometryNode *coordNode = graph->create_node<GeometryNode>();

          MappingNode *mappingNode = graph->create_node<MappingNode>();

          /* in Blender, UV coordinates are different between lights and surfaces, while USD
           * assumes they are the same. We flip on the x for emissive area lights with textures to
           * treat it as a surface.
           */
          mappingNode->set_scale(make_float3(-1.0f, 1.0f, 1.0f));

          graph->connect(coordNode->output("Parametric"), mappingNode->input("Vector"));

          textureNode = graph->create_node<ImageTextureNode>();
          static_cast<ImageTextureNode *>(textureNode)->set_filename(ustring(filename));
          graph->connect(mappingNode->output("Vector"), textureNode->input("Vector"));
        }

        if (hasColorTemperature) {
          VectorMathNode *mathNode = graph->create_node<VectorMathNode>();
          mathNode->set_math_type(NODE_VECTOR_MATH_MULTIPLY);

          graph->connect(textureNode->output("Color"), mathNode->input("Vector1"));
          ShaderInput *const outputNodeInput = outputNode->input("Color");
          graph->connect(outputNodeInput->link, mathNode->input("Vector2"));
          graph->disconnect(outputNodeInput);
          graph->connect(mathNode->output("Vector"), outputNodeInput);
        }
        else if (_lightType == HdPrimTypeTokens->domeLight) {
          VectorMathNode *mathNode = graph->create_node<VectorMathNode>();
          mathNode->set_math_type(NODE_VECTOR_MATH_MULTIPLY);
          mathNode->set_vector2(_domeStrength);

          graph->connect(textureNode->output("Color"), mathNode->input("Vector1"));
          graph->connect(mathNode->output("Vector"), outputNode->input("Color"));
        }
        else {
          graph->connect(textureNode->output("Color"), outputNode->input("Color"));
        }
      }
    }

    if (auto ds = GetTypedDataSource<float>(lightContainer, HdLightTokens->shapingFocus)) {
      const float focusShaping = ds->GetTypedValue(0.0f);

      if (focusShaping > 0.0f && _lightType != HdPrimTypeTokens->domeLight) {
        /* Fetch shaping:focusTint (default is black) */
        float3 focusTint = zero_float3();
        if (auto tintDs = GetTypedDataSource<GfVec3f>(lightContainer,
                                                      HdLightTokens->shapingFocusTint))
        {
          const GfVec3f tint = tintDs->GetTypedValue(0.0f);
          focusTint = make_float3(tint[0], tint[1], tint[2]);
        }

        BuildFocusShapingNodes(graph.get(), focusShaping, focusTint, outputNode);

        hasSpatialVarying = true;
      }
    }

    const float coneAngle = GetTypedValue<float>(
        lightContainer, HdLightTokens->shapingConeAngle, 90.0f);
    const float coneSoftness = clamp(
        GetTypedValue<float>(lightContainer, HdLightTokens->shapingConeSoftness, 0.0f),
        0.0f,
        1.0f);

    const bool hasConeShaping = (coneAngle < 90.0f || coneSoftness > 0.0f);
    if (hasConeShaping && _lightType != HdPrimTypeTokens->domeLight &&
        _lightType != HdPrimTypeTokens->distantLight)
    {
      BuildConeShapingNodes(graph.get(), coneAngle, coneSoftness, outputNode);
      hasSpatialVarying = true;
    }
  }

  Shader *const shader = _shader;
  if (_lightType == HdPrimTypeTokens->domeLight) {
    HdCyclesSession::AdjustWorld(graph.get(), exp2f(_worldExposure), _worldSaturation);
    static_cast<HdCyclesSession *>(renderParam)->SetWorldSource(shader, *graph);
  }
  shader->set_graph(std::move(graph));
  shader->tag_update((Scene *)_light->get_owner());

  shader->has_surface_spatial_varying = hasSpatialVarying;
}

void HdCyclesLight::Finalize(HdRenderParam *renderParam)
{
  if (!_light) {
    return;
  }

  const SceneLock lock(renderParam);
  const bool keep_nodes = static_cast<const HdCyclesSession *>(renderParam)->keep_nodes;
  static_cast<HdCyclesSession *>(renderParam)->RemoveLinking(_object->name.string());
  static_cast<HdCyclesSession *>(renderParam)->RemoveWorldMaterial(_light);
  static_cast<HdCyclesSession *>(renderParam)->RemoveWorldSource(_shader);
  static_cast<HdCyclesSession *>(renderParam)->lightsChanged = true;
  _instances.Finalize(static_cast<HdCyclesSession *>(renderParam), GetId());

  if (!keep_nodes) {
    HdCyclesSession *const session = static_cast<HdCyclesSession *>(renderParam);
    session->DeleteNode(_object);
    session->DeleteNode(_light);
    if (_shader) {
      session->DeleteNode(_shader); /* its own shader, never released before: one more per light made again */
    }
  }

  _light = nullptr;
  _object = nullptr;
  _shader = nullptr;
}

void HdCyclesLight::Initialize(HdRenderParam *renderParam)
{
  if (_light) {
    return;
  }

  const SceneLock lock(renderParam);
  static_cast<HdCyclesSession *>(renderParam)->lightsChanged = true;

  _object = lock.scene->create_node<Object>();
  _object->name = GetId().GetString();

  if (_lightType == HdPrimTypeTokens->domeLight) {
    _light = lock.scene->create_node<BackgroundLight>();
  }
  else if (_lightType == HdPrimTypeTokens->distantLight) {
    _light = lock.scene->create_node<SunLight>();
  }
  else if (_lightType == HdPrimTypeTokens->diskLight) {
    _light = lock.scene->create_node<AreaLight>();
    static_cast<AreaLight *>(_light)->set_ellipse(true);
  }
  else if (_lightType == HdPrimTypeTokens->rectLight) {
    _light = lock.scene->create_node<AreaLight>();
    static_cast<AreaLight *>(_light)->set_ellipse(false);
  }
  else if (_lightType == HdPrimTypeTokens->sphereLight) {
    /* We can't know in advance if this is spot light or point light, so we set to derived class
     * SpotLight and change the type later. */
    _light = lock.scene->create_node<SpotLight>();
  }

  _light->set_use_mis(true);
  _light->name = GetId().GetString();

  _object->set_geometry(_light);
  _object->set_random_id(hash_uint2(hash_string(_light->name.c_str()), 0));
  _object->set_visibility(PATH_RAY_VISIBILITY_ALL & ~PATH_RAY_VISIBILITY_CAMERA);

  Shader *const shader = static_cast<HdCyclesSession *>(renderParam)->TakeShader(lock.scene);
  _shader = shader;
  array<Node *> used_shaders;
  used_shaders.push_back_slow(shader);
  _light->set_used_shaders(used_shaders);

  /* Create default shader graph */
  PopulateShaderGraph(HdContainerDataSourceHandle(), renderParam);
}

void HdCyclesLightInstances::Sync(HdSceneDelegate *sceneDelegate,
                                  const SdfPath &id,
                                  const HdContainerDataSourceHandle &primDs,
                                  HdDirtyBits *dirtyBits)
{
  HdRenderIndex &renderIndex = sceneDelegate->GetRenderIndex();
  _tracker = &renderIndex.GetChangeTracker();
  /* Instancer changes mark the light DirtyInstancer through this dependency. */
  const SdfPath instancerId = sceneDelegate->GetInstancerId(id);
  if (instancerId != _instancerId) {
    if (!_instancerId.IsEmpty()) {
      _tracker->RemoveInstancerSprimDependency(_instancerId, id);
    }
    if (!instancerId.IsEmpty()) {
      _tracker->AddInstancerSprimDependency(instancerId, id);
    }
    _instancerId = instancerId;
    *dirtyBits |= HdLight::DirtyTransform;
  }
  if (*dirtyBits & HdLight::DirtyInstancer) {
    *dirtyBits |= HdLight::DirtyTransform;
  }
  if (!(*dirtyBits & HdLight::DirtyTransform)) {
    return;
  }

  _xform = GfMatrix4d(1.0);
  if (auto matrixDs = HdXformSchema::GetFromParent(primDs).GetMatrix()) {
    _xform = matrixDs->GetTypedValue(0.0f);
  }
  _transforms.clear();
  if (!_instancerId.IsEmpty()) {
    HdInstancer::_SyncInstancerAndParents(renderIndex, _instancerId);
    /* shortcut: instances at the current frame only, lights have no motion blur. */
    if (auto *instancer = static_cast<HdCyclesInstancer *>(renderIndex.GetInstancer(_instancerId)))
    {
      _transforms = instancer->ComputeInstanceTransforms(id, {}).front();
    }
  }
}

Transform HdCyclesLightInstances::GetTransform() const
{
  return convert_transform(_transforms.empty() ? _xform : _xform * _transforms.front());
}

bool HdCyclesLightInstances::IsEmpty() const
{
  return !_instancerId.IsEmpty() && _transforms.empty();
}

/* Copies the non-array sockets of `src` to `dst` (of the same type), except `skip`. */
static void CopySockets(Node *dst, const Node &src, std::initializer_list<const char *> skip)
{
  for (const SocketType &socket : src.type->inputs) {
    if (!socket.is_array() &&
        std::none_of(skip.begin(), skip.end(), [&](const char *n) { return socket.name == n; }) &&
        !dst->equals_value(src, socket))
    {
      dst->set_value(socket, src, socket);
    }
  }
}

static float AverageScale(const Transform &tfm)
{
  return (len(transform_get_column(&tfm, 0)) + len(transform_get_column(&tfm, 1)) +
          len(transform_get_column(&tfm, 2))) /
         3.0f;
}

void HdCyclesLightInstances::Update(Scene *scene,
                                    HdCyclesSession *session,
                                    const Object *object,
                                    const bool normalize)
{
  const size_t num = _transforms.empty() ? 0 : _transforms.size() - 1;
  if (num != _objects.size()) {
    session->lightsChanged = true;
  }
  for (size_t i = num; i < _objects.size(); ++i) {
    session->DeleteNode(_objects[i]);
    if (_lights[i]) {
      session->DeleteNode(_lights[i]);
    }
  }
  const size_t oldNum = std::min(num, _objects.size());
  _objects.resize(num);
  _lights.resize(num, nullptr);

  Geometry *geom = object->get_geometry();
  Light *light = geom->is_light() ? static_cast<Light *>(geom) : nullptr;
  for (size_t i = 0; i < num; ++i) {
    if (i >= oldNum) {
      _objects[i] = scene->create_node<Object>();
      /* The same name: light linking finds the instances by the light's name. */
      _objects[i]->name = object->name;
      _objects[i]->set_random_id(hash_uint2(hash_string(object->name.c_str()), uint(i + 1)));
    }
    Object *instance = _objects[i];
    CopySockets(instance, *object, {"tfm", "random_id", "geometry"});
    const Transform tfm = convert_transform(_xform * _transforms[i + 1]);
    instance->set_tfm(tfm);

    /* Cycles does not scale sphere light radii by the object, and the strength of normalized
     * lights depends on their size: instances scaled other than the first get their own light. */
    float radiusScale = 1.0f;
    float strengthScale = 1.0f;
    if (light && light->is_point_light() && static_cast<PointLight *>(light)->get_radius() > 0.0f)
    {
      radiusScale = AverageScale(tfm) / AverageScale(object->get_tfm());
      strengthScale = normalize ? 1.0f / sqr(radiusScale) : 1.0f;
    }
    else if (light && light->is_area_light() && normalize) {
      const AreaLight *areaLight = static_cast<AreaLight *>(light);
      strengthScale = areaLight->area(object->get_tfm()) / areaLight->area(tfm);
    }
    const bool ownLight = isfinite(radiusScale * strengthScale) &&
                          (fabsf(radiusScale - 1.0f) > 1e-5f ||
                           fabsf(strengthScale - 1.0f) > 1e-5f);
    if (ownLight && !_lights[i]) {
      if (light->is_point_light()) {
        _lights[i] = scene->create_node<SpotLight>();
      }
      else {
        _lights[i] = scene->create_node<AreaLight>();
      }
      _lights[i]->name = light->name;
      array<Node *> usedShaders = light->get_used_shaders();
      _lights[i]->set_used_shaders(usedShaders);
    }
    else if (!ownLight && _lights[i]) {
      session->DeleteNode(_lights[i]);
      _lights[i] = nullptr;
    }
    if (Light *own = _lights[i]) {
      CopySockets(own, *light, {"strength", "radius"});
      own->set_strength(light->get_strength() * strengthScale);
      if (light->is_point_light()) {
        static_cast<PointLight *>(own)->set_radius(static_cast<PointLight *>(light)->get_radius() *
                                                   radiusScale);
      }
      if (own->is_modified()) {
        own->tag_update(scene);
      }
    }
    instance->set_geometry(_lights[i] ? _lights[i] : object->get_geometry());
    if (instance->is_modified()) {
      instance->tag_update(scene);
    }
  }
}

void HdCyclesLightInstances::Finalize(HdCyclesSession *session, const SdfPath &id)
{
  if (_tracker && !_instancerId.IsEmpty()) {
    _tracker->RemoveInstancerSprimDependency(_instancerId, id);
  }
  _instancerId = SdfPath();
  if (!session->keep_nodes) {
    for (size_t i = 0; i < _objects.size(); ++i) {
      session->DeleteNode(_objects[i]);
      if (_lights[i]) {
        session->DeleteNode(_lights[i]);
      }
    }
  }
  _objects.clear();
  _lights.clear();
  _transforms.clear();
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
