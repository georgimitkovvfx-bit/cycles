/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/pointcloud.h"
#include "hydra/geometry.inl"
#include "hydra/util.h"
#include "scene/pointcloud.h"
#include "util/color.h"
#include "util/hash.h"
#include "util/types_spherical_harmonics.h"

#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>

#include <cmath>

HDCYCLES_NAMESPACE_OPEN_SCOPE

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (ids)
    (positions)
    (orientations)
    (scales)
    (opacities)
    ((radianceSphericalHarmonicsCoefficients, "radiance:sphericalHarmonicsCoefficients"))
    ((radianceSphericalHarmonicsDegree, "radiance:sphericalHarmonicsDegree"))
);
// clang-format on

namespace {

/* Primvars of a particle field: float or half arrays (Hydra keeps the names of the float
 * attributes for both), at a shutter time for the motion steps. */
VtValue ReadSplatPrimvar(const HdPrimvarsSchema &primvars, const TfToken &name, const float time)
{
  const HdSampledDataSourceHandle ds = primvars ?
                                           primvars.GetPrimvar(name).GetFlattenedPrimvarValue() :
                                           nullptr;
  return ds ? ds->GetValue(time) : VtValue();
}

bool SplatPrimvarIsAnimated(const HdPrimvarsSchema &primvars,
                            const TfToken &name,
                            const std::vector<float> &motionTimes)
{
  const HdSampledDataSourceHandle ds = primvars ?
                                           primvars.GetPrimvar(name).GetFlattenedPrimvarValue() :
                                           nullptr;
  std::vector<float> sampleTimes;
  return !motionTimes.empty() && ds &&
         ds->GetContributingSampleTimesForInterval(
             motionTimes.front(), motionTimes.back(), &sampleTimes);
}

VtVec3fArray ReadSplatFloat3(const HdPrimvarsSchema &primvars,
                             const TfToken &name,
                             const float time = 0.0f)
{
  const VtValue value = PrimvarAsFloats(ReadSplatPrimvar(primvars, name, time));
  return value.IsHolding<VtVec3fArray>() ? value.UncheckedGet<VtVec3fArray>() : VtVec3fArray();
}

VtFloatArray ReadSplatFloat(const HdPrimvarsSchema &primvars,
                            const TfToken &name,
                            const float time = 0.0f)
{
  const VtValue value = PrimvarAsFloats(ReadSplatPrimvar(primvars, name, time));
  return value.IsHolding<VtFloatArray>() ? value.UncheckedGet<VtFloatArray>() : VtFloatArray();
}

std::vector<Quaternion> ReadSplatOrientations(const HdPrimvarsSchema &primvars,
                                              const float time = 0.0f)
{
  const VtValue value = ReadSplatPrimvar(primvars, _tokens->orientations, time);
  std::vector<Quaternion> result;
  const auto add = [&](const auto &quats) {
    result.reserve(quats.size());
    for (const auto &q : quats) {
      const auto &i = q.GetImaginary();
      result.push_back(make_quaternion(float(q.GetReal()), float(i[0]), float(i[1]), float(i[2])));
    }
  };
  if (value.IsHolding<VtQuatfArray>()) {
    add(value.UncheckedGet<VtQuatfArray>());
  }
  else if (value.IsHolding<VtQuathArray>()) {
    add(value.UncheckedGet<VtQuathArray>());
  }
  return result;
}

}  // namespace

HdCyclesPoints::HdCyclesPoints(const SdfPath &rprimId, const bool gaussianSplats)
    : HdCyclesGeometry(rprimId), _gaussianSplats(gaussianSplats)
{
}

HdCyclesPoints::~HdCyclesPoints() = default;

HdDirtyBits HdCyclesPoints::GetInitialDirtyBitsMask() const
{
  HdDirtyBits bits = HdCyclesGeometry::GetInitialDirtyBitsMask();
  bits |= HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyWidths |
          HdChangeTracker::DirtyPrimvar;
  return bits;
}

HdDirtyBits HdCyclesPoints::_PropagateDirtyBits(HdDirtyBits bits) const
{
  // Points and widths always have to be updated together
  if (bits & (HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyWidths)) {
    bits |= HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyWidths;
  }

  return bits;
}

void HdCyclesPoints::Populate(HdSceneDelegate *sceneDelegate, HdDirtyBits dirtyBits, bool &rebuild)
{
  if (_gaussianSplats) {
    /* The splat data are primvars ("positions" is not Hydra's points). */
    if (dirtyBits & (HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyPoints |
                     HdChangeTracker::DirtyWidths))
    {
      const size_t numPoints = _geom->num_points();
      PopulateGaussianSplats(sceneDelegate);
      PopulatePrimvars(sceneDelegate);
      rebuild = _geom->num_points() != numPoints;
    }
    return;
  }

  if (dirtyBits & (HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyWidths)) {
    const size_t numPoints = _geom->num_points();

    PopulatePoints(sceneDelegate);
    PopulateWidths(sceneDelegate);

    rebuild = _geom->num_points() != numPoints;

    array<int> shaders;
    shaders.reserve(_geom->num_points());
    for (size_t i = 0; i < _geom->num_points(); ++i) {
      shaders.push_back_reserved(0);
    }

    _geom->set_shader(shaders);
  }

  if (dirtyBits & HdChangeTracker::DirtyPrimvar) {
    PopulatePrimvars(sceneDelegate);
  }

  if (dirtyBits & (HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyPrimvar)) {
    PopulatePointRandom(sceneDelegate);
  }
}

/* Point Info Random, like Blender: a hash of the point id (the ids of USD points, which stay
 * with a point when the points are reordered), else of the point index. */
void HdCyclesPoints::PopulatePointRandom(HdSceneDelegate *sceneDelegate)
{
  if (!_geom->need_attribute((Scene *)_geom->get_owner(), ATTR_STD_POINT_RANDOM)) {
    _geom->attributes.remove(ATTR_STD_POINT_RANDOM);
    return;
  }
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const VtValue value = ReadPrimvar(HdPrimvarsSchema::GetFromParent(prim.dataSource),
                                    _tokens->ids);
  const VtInt64Array *ids = value.IsHolding<VtInt64Array>() &&
                                    value.UncheckedGet<VtInt64Array>().size() ==
                                        _geom->num_points() ?
                                &value.UncheckedGet<VtInt64Array>() :
                                nullptr;
  float *random = _geom->attributes.add(ATTR_STD_POINT_RANDOM)->data_for_write<float>();
  for (size_t i = 0; i < _geom->num_points(); ++i) {
    random[i] = hash_uint2_to_float(ids ? uint((*ids)[i]) : uint(i), 0);
  }
}

void HdCyclesPoints::PopulatePoints(HdSceneDelegate *sceneDelegate)
{
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const VtValue value = ReadPrimvar(primvars, HdTokens->points);

  if (!value.IsHolding<VtVec3fArray>()) {
    TF_WARN("Invalid points data for %s", GetId().GetText());
    return;
  }

  const auto &points = value.UncheckedGet<VtVec3fArray>();

  _geom->resize(int(points.size()));

  /* Radius has no motion steps, Cycles uses the center one for all. */
  SetPositions(_geom->attributes.add(ATTR_STD_POSITION), primvars, points, points.size());
}

void HdCyclesPoints::PopulateWidths(HdSceneDelegate *sceneDelegate)
{
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const VtValue value = ReadPrimvar(primvars, HdTokens->widths);
  const HdInterpolation interpolation = ReadPrimvarInterpolation(primvars, HdTokens->widths);

  if (!value.IsHolding<VtFloatArray>()) {
    /* Without widths, width 1 like Karma and Storm. */
    if (!value.IsEmpty()) {
      TF_WARN("Invalid widths data for %s", GetId().GetText());
    }
    std::fill_n(_geom->get_radius_for_write(), _geom->num_points(), 0.5f);
    return;
  }

  const auto &widths = value.UncheckedGet<VtFloatArray>();
  float *radius = _geom->get_radius_for_write();

  if (interpolation == HdInterpolationConstant && !widths.empty()) {
    const float constantRadius = widths[0] * 0.5f;

    for (size_t i = 0; i < _geom->num_points(); ++i) {
      radius[i] = constantRadius;
    }
  }
  else if (widths.size() == _geom->num_points()) {
    /* Vertex, or varying which is the same for points. */
    for (size_t i = 0; i < _geom->num_points(); ++i) {
      radius[i] = widths[i] * 0.5f;
    }
  }
  else {
    TF_WARN("Invalid widths data for %s", GetId().GetText());
    std::fill_n(radius, _geom->num_points(), 0.5f);
  }
}

void HdCyclesPoints::PopulatePrimvars(HdSceneDelegate *sceneDelegate)
{
  Scene *const scene = (Scene *)_geom->get_owner();

  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const TfToken uvName = FindUVPrimvar(primvars);

  const std::pair<HdInterpolation, AttributeElement> interpolations[] = {
      std::make_pair(HdInterpolationVertex, ATTR_ELEMENT_VERTEX),
      std::make_pair(HdInterpolationVarying, ATTR_ELEMENT_VERTEX),
      std::make_pair(HdInterpolationFaceVarying, ATTR_ELEMENT_VERTEX),
      std::make_pair(HdInterpolationUniform, ATTR_ELEMENT_OBJECT),
      std::make_pair(HdInterpolationConstant, ATTR_ELEMENT_OBJECT),
  };

  for (const auto &interpolation : interpolations) {
    for (const TfToken &primvarName : PrimvarNamesAtInterpolation(primvars, interpolation.first)) {
      // Skip special primvars that are handled separately
      if (primvarName == HdTokens->points || primvarName == HdTokens->widths ||
          (_gaussianSplats &&
           (primvarName == _tokens->positions || primvarName == _tokens->orientations ||
            primvarName == _tokens->scales || primvarName == _tokens->opacities ||
            primvarName == _tokens->radianceSphericalHarmonicsCoefficients ||
            primvarName == _tokens->radianceSphericalHarmonicsDegree)))
      {
        continue;
      }

      VtValue value = PrimvarAsFloats(ReadPrimvar(primvars, primvarName));
      if (value.IsEmpty()) {
        continue;
      }

      AttributeStandard std = PrimvarStandard(primvars, primvarName, uvName);
      if (primvarName == HdTokens->normals && interpolation.first == HdInterpolationVertex) {
        std = ATTR_STD_VERTEX_NORMAL;
      }
      else if (std == ATTR_STD_VERTEX_COLOR && interpolation.first == HdInterpolationConstant) {
        SetInstanceColor(value);
      }

      // Skip attributes that are not needed
      const std::vector<ustring> names = RequestedPrimvarNames(
          _geom, scene, primvars, primvarName, std);
      if (names.empty()) {
        continue;
      }
      if (std == ATTR_STD_UV) {
        value = PrimvarAsFloat2(value); /* Cycles stores UVs as float2. */
      }

      const ustring ownName(primvarName.GetString());
      for (const ustring &name : names) {
        ApplyPrimvars(_geom->attributes,
                      name,
                      value,
                      interpolation.second,
                      name == ownName ? std : ATTR_STD_NONE);
      }
    }
  }
}

Shader *HdCyclesPoints::GetDefaultShader(Scene *scene) const
{
  return _gaussianSplats ? scene->default_gsplat : HdCyclesGeometry::GetDefaultShader(scene);
}

/* USD ParticleField3DGaussianSplat: linear opacities and scales, (real, imaginary) orientations,
 * spherical harmonics coefficients per particle in the 3DGS (PLY) order, the first one the DC
 * term. Cycles' radiance base is the DC coefficient with the opacity, the other coefficients go
 * to the packed (8 bit, [-1, 1]) degree 1 to 3 attribute. */
void HdCyclesPoints::PopulateGaussianSplats(HdSceneDelegate *sceneDelegate)
{
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);

  const VtVec3fArray positions = ReadSplatFloat3(primvars, _tokens->positions);
  const size_t n = positions.size();

  _geom->clear(true);
  _geom->set_render_as(PointCloud::RENDER_AS_GSPLATS);
  _geom->resize(int(n));
  Attribute *attr_P = _geom->attributes.add(ATTR_STD_POSITION);
  SetPositions(attr_P, primvars, positions, n, {}, _tokens->positions);
  std::fill_n(_geom->get_radius_for_write(), n, 0.0f); /* Cycles bounds the splats. */

  /* Motion blur of the scales, orientations and opacities like Karma, at the motion steps of the
   * positions (static positions get steps when only the other splat data are animated). */
  const std::vector<float> motionTimes = GetObjectMotionTimes(
      primvars.GetPrimvar(_tokens->positions).GetPrimvarValue(), true); /* As SetPositions. */
  const bool scalesMotion = SplatPrimvarIsAnimated(primvars, _tokens->scales, motionTimes);
  const bool orientationsMotion = SplatPrimvarIsAnimated(
      primvars, _tokens->orientations, motionTimes);
  const bool opacitiesMotion = SplatPrimvarIsAnimated(primvars, _tokens->opacities, motionTimes);
  if (!attr_P->has_motion() && (scalesMotion || orientationsMotion || opacitiesMotion)) {
    _geom->set_use_motion_blur(_session->session->scene->integrator->get_motion_blur());
    _geom->set_motion_steps(uint(motionTimes.size()));
    attr_P->add_motion(_geom);
    for (int step = 1; step < attr_P->num_motion_steps(); ++step) {
      std::copy_n(attr_P->data<packed_float3>(), n, attr_P->data_for_write<packed_float3>(step));
    }
  }
  const int numSteps = attr_P->has_motion() &&
                               motionTimes.size() == size_t(_geom->get_motion_steps()) ?
                           int(_geom->get_motion_steps()) :
                           0;

  const VtVec3fArray sh = ReadSplatFloat3(primvars,
                                          _tokens->radianceSphericalHarmonicsCoefficients);
  int degree = VtValue::Cast<int>(ReadPrimvar(primvars, _tokens->radianceSphericalHarmonicsDegree))
                   .GetWithDefault(-1);
  if (degree < 0 && n > 0) {
    degree = int(std::lround(std::sqrt(double(sh.size() / n)))) - 1;
  }
  const size_t coefficients = size_t(std::max(degree + 1, 0)) * size_t(std::max(degree + 1, 0));
  /* Ignored when too short, like USD says. */
  const bool hasSH = coefficients > 0 && sh.size() >= n * coefficients;

  Attribute *attr_base = _geom->attributes.add(ATTR_STD_GSPLAT_RADIANCE_BASE);
  Attribute *attr_scale = _geom->attributes.add(ATTR_STD_GSPLAT_SCALE);
  Attribute *attr_rotation = _geom->attributes.add(ATTR_STD_GSPLAT_ROTATION);
  PackedSphericalHarmonicsRest *rest = nullptr;
  if (hasSH && coefficients > 1) {
    rest = _geom->attributes.add(ATTR_STD_GSPLAT_RADIANCE_SPHERICAL_HARMONICS_REST)
               ->data_for_write<PackedSphericalHarmonicsRest>();
  }
  if (numSteps > 0) {
    if (scalesMotion || opacitiesMotion) {
      _geom->attributes.find(ATTR_STD_RADIUS)->add_motion(_geom); /* Bounds of every step. */
    }
    for (const auto &[attr, animated] : {std::pair{attr_base, opacitiesMotion},
                                         std::pair{attr_scale, scalesMotion},
                                         std::pair{attr_rotation, orientationsMotion}})
    {
      if (animated) {
        attr->add_motion(_geom);
      }
    }
  }

  /* Without coefficients, the display color like Karma: Houdini's SOP Import of a Bake GSplats SOP
   * (Compute SPH Coefficients off by default) writes only the color, as primvars:displayColor
   * (scene linear). Cycles reads 0.5 + C0 * DC as sRGB. Else a 0 DC coefficient (0.5, USD).
   * The spherical harmonics are not blurred: every step has the frame's DC. */
  const VtVec3fArray displayColor = hasSH ? VtVec3fArray() :
                                            ReadSplatFloat3(primvars, HdTokens->displayColor);
  const auto fillBase = [&](const VtFloatArray &opacities, float4 *base) {
    for (size_t i = 0; i < n; ++i) {
      float3 dc = zero_float3();
      if (hasSH) {
        dc = make_float3(sh[i * coefficients][0], sh[i * coefficients][1], sh[i * coefficients][2]);
      }
      else if (!displayColor.empty()) {
        const GfVec3f &c = displayColor[displayColor.size() == n ? i : 0];
        dc = (color_linear_to_srgb_v3(make_float3(c[0], c[1], c[2])) - make_float3(0.5f)) /
             0.28209479177387814f;
      }
      base[i] = make_float4(dc.x, dc.y, dc.z, i < opacities.size() ? opacities[i] : 1.0f);
    }
  };
  const auto fillScale = [&](const VtVec3fArray &scales, packed_float3 *scale) {
    for (size_t i = 0; i < n; ++i) {
      scale[i] = i < scales.size() ? make_float3(scales[i][0], scales[i][1], scales[i][2]) :
                                     PointCloud::DEFAULT_GSPLAT_SCALE;
    }
  };
  const auto fillRotation = [&](const std::vector<Quaternion> &orientations,
                                Quaternion *rotation) {
    for (size_t i = 0; i < n; ++i) {
      rotation[i] = i < orientations.size() ? orientations[i] : identity_quaternion();
    }
  };
  fillBase(ReadSplatFloat(primvars, _tokens->opacities), attr_base->data_for_write<float4>());
  fillScale(ReadSplatFloat3(primvars, _tokens->scales),
            attr_scale->data_for_write<packed_float3>());
  fillRotation(ReadSplatOrientations(primvars), attr_rotation->data_for_write<Quaternion>());
  for (int step = 0; step < numSteps; ++step) {
    const float time = motionTimes[step];
    if (opacitiesMotion) {
      fillBase(
          ReadSplatFloat(primvars, _tokens->opacities, time),
          attr_base->data_for_write<float4>(attr_base->time_step_to_attr_step(step, numSteps)));
    }
    if (scalesMotion) {
      fillScale(ReadSplatFloat3(primvars, _tokens->scales, time),
                attr_scale->data_for_write<packed_float3>(
                    attr_scale->time_step_to_attr_step(step, numSteps)));
    }
    if (orientationsMotion) {
      fillRotation(ReadSplatOrientations(primvars, time),
                   attr_rotation->data_for_write<Quaternion>(
                       attr_rotation->time_step_to_attr_step(step, numSteps)));
    }
  }

  if (rest) {
    const size_t numRest = std::min<size_t>(coefficients - 1,
                                            PackedSphericalHarmonicsRest::MAX_COEFFICIENTS);
    for (size_t i = 0; i < n; ++i) {
      spherical_harmonics_rest_fill_zero(rest[i]);
      for (size_t c = 0; c < numRest; ++c) {
        const GfVec3f &v = sh[i * coefficients + 1 + c];
        spherical_harmonics_rest_set_coefficient(rest[i], int(c), make_float3(v[0], v[1], v[2]));
      }
    }
  }

  array<int> shaders;
  shaders.resize(n);
  std::fill(shaders.begin(), shaders.end(), 0);
  _geom->set_shader(shaders);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
