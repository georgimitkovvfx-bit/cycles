/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/util.h"
#include "scene/attribute.h"
#include "scene/object.h"

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/array.h>
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

void SetObjectAttribute(Object *object, const ustring &name, const VtValue &value)
{
  if (value.GetArraySize() != 1) {
    return;
  }
  static const TypeDesc types[] = {TypeFloat, TypeFloat2, TypeVector, TypeFloat4};
  const ParamValue param(name,
                         types[HdGetComponentCount(HdGetValueTupleType(value).type) - 1],
                         1,
                         HdGetValueData(value));

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
                   AttributeStandard std)
{
  /* Wrong counts would overflow or underfill the attribute, e.g. Houdini array attributes or
   * cubic curve varying data. */
  const size_t expected = Attribute::element_size(attributes.geometry, elem, attributes.prim);
  if (value.GetArraySize() != expected) {
    TF_WARN(
        "Primvar %s has %zu values, expected %zu", name.c_str(), value.GetArraySize(), expected);
    return;
  }

  const void *data = HdGetValueData(value);
  size_t size = value.GetArraySize();

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
        size = valueData.size() * sizeof(packed_normal);

        VtArray<packed_normal> valueConverted;
        valueConverted.reserve(valueData.size());
        for (const GfVec3f &vec : valueData) {
          valueConverted.push_back(packed_normal(make_float3(vec[0], vec[1], vec[2])));
        }
        data = valueConverted.data();
        value = std::move(valueConverted);
      }
      else {
        attrType = CCL_NS::TypeVector;
        size = valueData.size() * sizeof(packed_float3);
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

HDCYCLES_NAMESPACE_CLOSE_SCOPE
