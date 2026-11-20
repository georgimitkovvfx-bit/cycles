/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/util.h"
#include "kernel/types.h"
#include "scene/attribute.h"
#include "scene/geometry.h"
#include "scene/object.h"

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdSceneIndexPrim GetPrim(HdSceneDelegate *delegate, const SdfPath &id)
{
  if (!delegate) {
    return {};
  }
  HdSceneIndexBaseRefPtr si = delegate->GetRenderIndex().GetTerminalSceneIndex();
  if (!si) {
    return {};
  }
  return si->GetPrim(id);
}

VtValue ReadPrimvar(const HdPrimvarsSchema &primvars, const TfToken &name)
{
  if (!primvars) {
    return {};
  }
  HdSampledDataSourceHandle ds = primvars.GetPrimvar(name).GetFlattenedPrimvarValue();
  return ds ? ds->GetValue(0.0f) : VtValue();
}

HdInterpolation ReadPrimvarInterpolation(const HdPrimvarsSchema &primvars, const TfToken &name)
{
  if (!primvars) {
    return HdInterpolationCount;
  }
  HdTokenDataSourceHandle ds = primvars.GetPrimvar(name).GetInterpolation();
  if (!ds) {
    return HdInterpolationCount;
  }
  const TfToken token = ds->GetTypedValue(0.0f);
  if (token == HdPrimvarSchemaTokens->constant) {
    return HdInterpolationConstant;
  }
  if (token == HdPrimvarSchemaTokens->uniform) {
    return HdInterpolationUniform;
  }
  if (token == HdPrimvarSchemaTokens->varying) {
    return HdInterpolationVarying;
  }
  if (token == HdPrimvarSchemaTokens->vertex) {
    return HdInterpolationVertex;
  }
  if (token == HdPrimvarSchemaTokens->faceVarying) {
    return HdInterpolationFaceVarying;
  }
  if (token == HdPrimvarSchemaTokens->instance) {
    return HdInterpolationInstance;
  }
  return HdInterpolationCount;
}

TfToken ReadPrimvarRole(const HdPrimvarsSchema &primvars, const TfToken &name)
{
  if (!primvars) {
    return {};
  }
  HdTokenDataSourceHandle ds = primvars.GetPrimvar(name).GetRole();
  return ds ? ds->GetTypedValue(0.0f) : TfToken();
}

TfTokenVector PrimvarNamesAtInterpolation(const HdPrimvarsSchema &primvars,
                                          HdInterpolation interpolation)
{
  TfTokenVector result;
  if (!primvars) {
    return result;
  }
  for (const TfToken &name : primvars.GetPrimvarNames()) {
    if (ReadPrimvarInterpolation(primvars, name) == interpolation) {
      result.push_back(name);
    }
  }
  return result;
}

static float ComponentAsFloat(const char *data, const HdType type)
{
  switch (type) {
    case HdTypeBool:
      return *reinterpret_cast<const bool *>(data) ? 1.0f : 0.0f;
    case HdTypeUInt8:
      return float(*reinterpret_cast<const uint8_t *>(data));
    case HdTypeUInt16:
      return float(*reinterpret_cast<const uint16_t *>(data));
    case HdTypeInt8:
      return float(*reinterpret_cast<const int8_t *>(data));
    case HdTypeInt16:
      return float(*reinterpret_cast<const int16_t *>(data));
    case HdTypeInt32:
      return float(*reinterpret_cast<const int32_t *>(data));
    case HdTypeUInt32:
      return float(*reinterpret_cast<const uint32_t *>(data));
    case HdTypeFloat:
      return *reinterpret_cast<const float *>(data);
    case HdTypeDouble:
      return float(*reinterpret_cast<const double *>(data));
    case HdTypeHalfFloat:
      return float(*reinterpret_cast<const GfHalf *>(data));
    default:
      return 0.0f;
  }
}

VtValue PrimvarAsFloats(const VtValue &value)
{
  if (value.IsHolding<VtFloatArray>() || value.IsHolding<VtVec2fArray>() ||
      value.IsHolding<VtVec3fArray>() || value.IsHolding<VtVec4fArray>())
  {
    return value;
  }
  if (value.IsHolding<VtBoolArray>()) {
    /* Hydra has no tuple type for bool arrays. */
    const VtBoolArray &bools = value.UncheckedGet<VtBoolArray>();
    VtFloatArray result(bools.size());
    std::transform(bools.begin(), bools.end(), result.begin(), [](bool b) { return float(b); });
    return VtValue(std::move(result));
  }
  if (value.IsHolding<VtInt64Array>()) {
    /* Nor for int64 arrays, like the ids of USD points. */
    const VtInt64Array &ints = value.UncheckedGet<VtInt64Array>();
    VtFloatArray result(ints.size());
    std::transform(ints.begin(), ints.end(), result.begin(), [](int64_t i) { return float(i); });
    return VtValue(std::move(result));
  }

  /* Scalars have a count of 1, so they become 1-element arrays like constant array primvars. */
  const HdTupleType tupleType = HdGetValueTupleType(value);
  const char *data = static_cast<const char *>(HdGetValueData(value));
  if (!data || tupleType.type == HdTypeInvalid) {
    return {}; /* Strings, tokens and types Hydra does not know, like int64. */
  }
  const HdType componentType = HdGetComponentType(tupleType.type);
  const size_t numComponents = HdGetComponentCount(tupleType.type);
  if (numComponents < 1 || numComponents > 4 || componentType == HdTypeInt32_2_10_10_10_REV) {
    return {}; /* Matrices and packed normals. */
  }

  const size_t stride = HdDataSizeOfType(componentType);
  const auto convert = [&](auto array) {
    float *result = reinterpret_cast<float *>(array.data());
    for (size_t i = 0; i < tupleType.count * numComponents; ++i) {
      result[i] = ComponentAsFloat(data + i * stride, componentType);
    }
    return VtValue(std::move(array));
  };
  switch (numComponents) {
    case 1:
      return convert(VtFloatArray(tupleType.count));
    case 2:
      return convert(VtVec2fArray(tupleType.count));
    case 3:
      return convert(VtVec3fArray(tupleType.count));
    default:
      return convert(VtVec4fArray(tupleType.count));
  }
}

VtVec2fArray PrimvarAsFloat2(const VtValue &value)
{
  const HdTupleType tupleType = HdGetValueTupleType(value);
  const size_t numComponents = HdGetComponentCount(tupleType.type);
  const float *data = static_cast<const float *>(HdGetValueData(value));
  VtVec2fArray result(tupleType.count, GfVec2f(0.0f));
  for (size_t i = 0; i < result.size(); ++i) {
    for (size_t c = 0; c < std::min<size_t>(numComponents, 2); ++c) {
      result[i][c] = data[i * numComponents + c];
    }
  }
  return result;
}

TfToken FindUVPrimvar(const HdPrimvarsSchema &primvars)
{
  static const TfToken st("st"), uv("uv");
  if (primvars.GetPrimvar(st)) {
    return st;
  }
  if (primvars.GetPrimvar(uv)) {
    return uv;
  }
  for (const TfToken &name : primvars.GetPrimvarNames()) {
    if (ReadPrimvarRole(primvars, name) == HdPrimvarRoleTokens->textureCoordinate) {
      return name;
    }
  }
  return {};
}

AttributeStandard PrimvarStandard(const HdPrimvarsSchema &primvars,
                                  const TfToken &name,
                                  const TfToken &uvName)
{
  static const TfToken Cd("Cd");
  if (name == uvName) {
    return ATTR_STD_UV;
  }
  if (name == HdTokens->displayColor ||
      (name == Cd && !primvars.GetPrimvar(HdTokens->displayColor)))
  {
    return ATTR_STD_VERTEX_COLOR;
  }
  /* The Attribute node requests these names as standard attributes. */
  const AttributeStandard std = Attribute::name_standard(ustring(name.GetString()));
  return (std >= ATTR_STD_VOLUME_DENSITY && std <= ATTR_STD_VOLUME_VELOCITY) ? std : ATTR_STD_NONE;
}

std::vector<ustring> RequestedPrimvarNames(Geometry *geom,
                                           Scene *scene,
                                           const HdPrimvarsSchema &primvars,
                                           const TfToken &name,
                                           const AttributeStandard std)
{
  return RequestedPrimvarNames(
      geom,
      scene,
      [&](const TfToken &primvarName) { return bool(primvars.GetPrimvar(primvarName)); },
      name,
      std);
}

std::vector<ustring> RequestedPrimvarNames(Geometry *geom,
                                           Scene *scene,
                                           const std::function<bool(const TfToken &)> &hasPrimvar,
                                           const TfToken &name,
                                           const AttributeStandard std)
{
  /* Houdini SOP Import renames these attributes to the USD names. */
  static const std::pair<TfToken, TfToken> aliases[] = {
      {HdTokens->displayColor, TfToken("Cd")},
      {HdTokens->displayOpacity, TfToken("Alpha")},
      {TfToken("st"), TfToken("uv")},
      {HdTokens->velocities, TfToken("v")},
      {HdTokens->accelerations, TfToken("accel")},
  };

  std::vector<ustring> names;
  const ustring ownName(name.GetString());
  if (geom->need_attribute(scene, ownName) || geom->need_attribute(scene, std)) {
    names.push_back(ownName);
  }
  for (const auto &[usdName, houdiniName] : aliases) {
    const TfToken alias = (name == usdName)     ? houdiniName :
                          (name == houdiniName) ? usdName :
                                                  TfToken();
    if (!alias.IsEmpty() && !hasPrimvar(alias) &&
        geom->need_attribute(scene, ustring(alias.GetString())))
    {
      names.emplace_back(alias.GetString());
    }
  }
  const ustring prefixedName("primvars:" + name.GetString());
  if (geom->need_attribute(scene, prefixedName)) {
    names.push_back(prefixedName);
  }
  return names;
}

void SetObjectAttribute(Object *object, const ustring &name, const VtValue &value)
{
  if (value.GetArraySize() != 1) {
    return;
  }
  SetObjectAttribute(object,
                     name,
                     HdGetComponentCount(HdGetValueTupleType(value).type),
                     static_cast<const float *>(HdGetValueData(value)));
}

void SetObjectAttribute(Object *object, const ustring &name, size_t width, const float *data)
{
  static const TypeDesc types[] = {TypeFloat, TypeFloat2, TypeVector, TypeFloat4};
  const ParamValue param(name, types[width - 1], 1, data);

  for (ParamValue &attribute : object->attributes) {
    if (attribute.name() == name) {
      attribute = param;
      return;
    }
  }
  object->attributes.push_back(param);
}

void ApplyPrimvars(AttributeSet &attributes,
                   const ustring &name,
                   VtValue value,
                   AttributeElement elem,
                   AttributeStandard std,
                   const bool dropExtraValues)
{
  /* Wrong counts would overflow or underfill the attribute, e.g. Houdini array attributes or
   * cubic curve varying data. */
  const size_t expected = Attribute::element_size(attributes.geometry, elem, attributes.prim);
  if (value.GetArraySize() != expected && !(dropExtraValues && value.GetArraySize() > expected)) {
    TF_WARN(
        "Primvar %s has %zu values, expected %zu", name.c_str(), value.GetArraySize(), expected);
    return;
  }

  const void *data = HdGetValueData(value);
  size_t size = expected;

  const HdType valueType = HdGetValueTupleType(value).type;

  TypeDesc attrType = CCL_NS::TypeUnknown;
  switch (valueType) {
    case HdTypeFloat:
      attrType = CCL_NS::TypeFloat;
      size *= sizeof(float);
      break;
    case HdTypeFloatVec2:
      attrType = CCL_NS::TypeFloat2;
      size *= sizeof(float2);
      static_assert(sizeof(GfVec2f) == sizeof(float2));
      break;
    case HdTypeFloatVec3: {
      const auto &valueData = value.Get<VtVec3fArray>();

      if (elem & ATTR_ELEMENT_IS_NORMAL) {
        attrType = CCL_NS::TypeNormal;

        VtArray<packed_normal> valueConverted;
        valueConverted.reserve(size);
        for (size_t i = 0; i < size; ++i) {
          const GfVec3f &vec = valueData[i];
          valueConverted.push_back(packed_normal(make_float3(vec[0], vec[1], vec[2])));
        }
        size *= sizeof(packed_normal);
        data = valueConverted.data();
        value = std::move(valueConverted);
      }
      else {
        attrType = CCL_NS::TypeVector;
        size *= sizeof(packed_float3);
        static_assert(sizeof(GfVec3f) == sizeof(packed_float3));
      }
      break;
    }
    case HdTypeFloatVec4:
      attrType = CCL_NS::TypeFloat4;
      size *= sizeof(float4);
      static_assert(sizeof(GfVec4f) == sizeof(float4));
      break;
    default:
      TF_WARN("Unsupported attribute type %d", static_cast<int>(valueType));
      return;
  }

  Attribute *const attr = attributes.add(name, attrType, elem);
  attr->std = std;

  assert(size == attr->data_sizeof() * attr->size);
  std::memcpy(attr->data_for_write(), data, size);
}

unsigned int LightMultiplierVisibility(const HdContainerDataSourceHandle &light)
{
  unsigned int visibility = PATH_RAY_VISIBILITY_ALL;
  if (GetTypedValue<float>(light, HdLightTokens->diffuse, 1.0f) == 0.0f) {
    visibility &= ~PATH_RAY_VISIBILITY_DIFFUSE;
  }
  if (GetTypedValue<float>(light, HdLightTokens->specular, 1.0f) == 0.0f) {
    visibility &= ~PATH_RAY_VISIBILITY_GLOSSY;
  }
  return visibility;
}

static bool KarmaRayVisible(const std::string &pattern, const std::string &ray)
{
  const auto trim = [](std::string text) {
    const size_t begin = text.find_first_not_of(" \t");
    const size_t end = text.find_last_not_of(" \t");
    return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
  };
  /* An empty expression makes the object unrenderable, like in Karma. */
  if (trim(pattern).empty()) {
    return false;
  }
  /* Alternatives ("|") of terms that must all hold ("&"). */
  size_t start = 0;
  while (start <= pattern.size()) {
    const size_t bar = std::min(pattern.find('|', start), pattern.size());
    const std::string alternative = pattern.substr(start, bar - start);
    bool all = true;
    size_t termStart = 0;
    while (termStart <= alternative.size()) {
      const size_t amp = std::min(alternative.find('&', termStart), alternative.size());
      std::string term = trim(alternative.substr(termStart, amp - termStart));
      bool negate = false;
      while (!term.empty() && (term[0] == '-' || term[0] == '!' || term[0] == '+')) {
        negate ^= term[0] != '+';
        term = trim(term.substr(1));
      }
      const bool match = term == "*" || term == ray;
      all = all && (negate ? !match : match);
      termStart = amp + 1;
    }
    if (all) {
      return true;
    }
    start = bar + 1;
  }
  return false;
}

unsigned int KarmaRenderVisibility(const std::string &pattern)
{
  static const std::pair<const char *, unsigned int> rays[] = {
      {"primary", PATH_RAY_VISIBILITY_CAMERA},
      {"shadow", PATH_RAY_VISIBILITY_SHADOW},
      {"diffuse", PATH_RAY_VISIBILITY_DIFFUSE},
      {"reflect", PATH_RAY_VISIBILITY_GLOSSY},
      {"refract", PATH_RAY_VISIBILITY_TRANSMIT},
      {"volume", PATH_RAY_VISIBILITY_VOLUME_SCATTER},
  };
  unsigned int visibility = 0;
  for (const auto &[ray, bit] : rays) {
    if (KarmaRayVisible(pattern, ray)) {
      visibility |= bit;
    }
  }
  return visibility;
}

int VelocityBlurMode(const VtValue &value)
{
  if (value.IsHolding<TfToken>() || value.IsHolding<std::string>()) {
    const std::string label = value.IsHolding<TfToken>() ?
                                  value.UncheckedGet<TfToken>().GetString() :
                                  value.UncheckedGet<std::string>();
    return label == "No Velocity Blur"  ? 0 :
           label == "Velocity Blur"     ? 1 :
           label == "Acceleration Blur" ? 2 :
                                          -1;
  }
  const int mode = VtValue::Cast<int>(value).GetWithDefault(-1);
  return mode >= 0 && mode <= 2 ? mode : -1;
}

unsigned int KarmaLightContributions(const HdContainerDataSourceHandle &light)
{
  static const TfToken contribs("inputs:karma:light:contribs");
  const std::string pattern = GetTypedValue<std::string>(light, contribs, "*");
  /* Coat is a Cycles glossy closure; SSS has no light ray type in Cycles. */
  static const std::pair<const char *, unsigned int> components[] = {
      {"diffuse", PATH_RAY_VISIBILITY_DIFFUSE},
      {"reflect", PATH_RAY_VISIBILITY_GLOSSY},
      {"coat", PATH_RAY_VISIBILITY_GLOSSY},
      {"refract", PATH_RAY_VISIBILITY_TRANSMIT},
      {"volume", PATH_RAY_VISIBILITY_VOLUME_SCATTER},
  };
  unsigned int visibility = 0;
  for (const auto &[component, bit] : components) {
    if (KarmaRayVisible(pattern, component)) {
      visibility |= bit;
    }
  }
  return visibility;
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
