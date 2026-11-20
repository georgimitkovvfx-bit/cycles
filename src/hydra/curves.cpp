/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/curves.h"
#include "hydra/geometry.inl"
#include "hydra/util.h"
#include "scene/hair.h"
#include "util/hash.h"
#include "util/types_float3.h"

#include <pxr/imaging/hd/basisCurvesSchema.h>
#include <pxr/imaging/hd/basisCurvesTopologySchema.h>

#include <array>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

template<typename T> VtValue ApplyKeyMap(const HdCyclesCurveKeyMap &map, const VtArray<T> &values)
{
  VtArray<T> keys(map.size());
  for (size_t key = 0; key < map.size(); ++key) {
    T sum(0.0f);
    for (int i = map.start[key]; i < map.start[key + 1]; ++i) {
      if (size_t(map.terms[i].first) < values.size()) {
        sum += values[map.terms[i].first] * map.terms[i].second;
      }
    }
    keys[key] = sum;
  }
  return VtValue(std::move(keys));
}

/* Weights of the 4 control points of a cubic segment at t. */
std::array<float, 4> CubicWeights(const TfToken &basis, const float t)
{
  const float s = 1.0f - t;
  if (basis == HdTokens->bspline) {
    return {s * s * s / 6.0f,
            (3.0f * t * t * t - 6.0f * t * t + 4.0f) / 6.0f,
            (-3.0f * t * t * t + 3.0f * t * t + 3.0f * t + 1.0f) / 6.0f,
            t * t * t / 6.0f};
  }
  if (basis == HdTokens->catmullRom) {
    return {0.5f * (-t * t * t + 2.0f * t * t - t),
            0.5f * (3.0f * t * t * t - 5.0f * t * t + 2.0f),
            0.5f * (-3.0f * t * t * t + 4.0f * t * t + t),
            0.5f * (t * t * t - t * t)};
  }
  /* Bezier (also the fallback for Hydra's unsupported bases). */
  return {s * s * s, 3.0f * t * s * s, 3.0f * t * t * s, t * t * t};
}

class KeyMapBuilder {
 public:
  explicit KeyMapBuilder(HdCyclesCurveKeyMap &map) : _map(map)
  {
    _map = HdCyclesCurveKeyMap();
  }
  void AddTerm(const int index, const float weight)
  {
    if (weight != 0.0f) {
      _map.terms.emplace_back(index, weight);
    }
  }
  void EndKey()
  {
    if (_map.start.empty()) {
      _map.start.push_back(0);
    }
    _map.start.push_back(int(_map.terms.size()));
  }

 private:
  HdCyclesCurveKeyMap &_map;
};

}  // namespace

VtValue HdCyclesCurveKeyMap::Apply(const VtValue &value) const
{
  if (empty() || value.GetArraySize() != sourceSize) {
    return value;
  }
  if (value.IsHolding<VtFloatArray>()) {
    return ApplyKeyMap(*this, value.UncheckedGet<VtFloatArray>());
  }
  if (value.IsHolding<VtVec2fArray>()) {
    return ApplyKeyMap(*this, value.UncheckedGet<VtVec2fArray>());
  }
  if (value.IsHolding<VtVec3fArray>()) {
    return ApplyKeyMap(*this, value.UncheckedGet<VtVec3fArray>());
  }
  if (value.IsHolding<VtVec4fArray>()) {
    return ApplyKeyMap(*this, value.UncheckedGet<VtVec4fArray>());
  }
  return value;
}

HdCyclesCurves::HdCyclesCurves(const SdfPath &rprimId) : HdCyclesGeometry(rprimId) {}

HdCyclesCurves::~HdCyclesCurves() = default;

HdDirtyBits HdCyclesCurves::GetInitialDirtyBitsMask() const
{
  HdDirtyBits bits = HdCyclesGeometry::GetInitialDirtyBitsMask();
  bits |= HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyWidths |
          HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyTopology;
  return bits;
}

HdDirtyBits HdCyclesCurves::_PropagateDirtyBits(HdDirtyBits bits) const
{
  if (bits & (HdChangeTracker::DirtyTopology)) {
    // Changing topology clears the geometry, so need to populate everything again
    bits |= HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyWidths |
            HdChangeTracker::DirtyPrimvar;
  }

  return bits;
}

void HdCyclesCurves::Populate(HdSceneDelegate *sceneDelegate, HdDirtyBits dirtyBits, bool &rebuild)
{
  if (HdChangeTracker::IsTopologyDirty(dirtyBits, GetId())) {
    PopulateTopology(sceneDelegate);
  }

  if (dirtyBits & HdChangeTracker::DirtyPoints) {
    PopulatePoints(sceneDelegate);
  }

  if (dirtyBits & HdChangeTracker::DirtyWidths) {
    PopulateWidths(sceneDelegate);
  }

  if (dirtyBits & HdChangeTracker::DirtyPrimvar) {
    PopulatePrimvars(sceneDelegate);
  }

  if (dirtyBits & (HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyPoints |
                   HdChangeTracker::DirtyPrimvar))
  {
    PopulateCurveAttributes();
  }

  rebuild = (_geom->position_is_modified()) || (_geom->radius_is_modified());
}

/* Curves Info Intercept, Length and Random, like Blender: the key's length along the curve
 * divided by the curve length, the length of the polyline through the keys, a hash of the curve
 * index. */
void HdCyclesCurves::PopulateCurveAttributes()
{
  Scene *const scene = (Scene *)_geom->get_owner();
  const AttributeStandard standards[] = {
      ATTR_STD_CURVE_INTERCEPT, ATTR_STD_CURVE_LENGTH, ATTR_STD_CURVE_RANDOM};
  float *data[3] = {};
  for (int i = 0; i < 3; ++i) {
    if (_geom->need_attribute(scene, standards[i])) {
      data[i] = _geom->attributes.add(standards[i])->data_for_write<float>();
    }
    else {
      _geom->attributes.remove(standards[i]);
    }
  }
  float *intercept = data[0];
  float *length = data[1];
  float *random = data[2];

  const Attribute *attr_P = _geom->attributes.find(ATTR_STD_POSITION);
  const packed_float3 *keys = attr_P ? attr_P->data<packed_float3>() : nullptr;
  for (size_t curve = 0; curve < _geom->num_curves(); ++curve) {
    if (random) {
      random[curve] = hash_uint2_to_float(uint(curve), 0);
    }
    if (!(intercept || length) || !keys) {
      continue;
    }
    const Hair::Curve c = _geom->get_curve(curve);
    float total = 0.0f;
    for (int k = 0; k < c.num_keys; ++k) {
      const int key = c.first_key + k;
      if (k > 0) {
        total += len(make_float3(keys[key]) - make_float3(keys[key - 1]));
      }
      if (intercept) {
        intercept[key] = total;
      }
    }
    if (intercept && total > 0.0f) {
      for (int k = 1; k < c.num_keys; ++k) {
        intercept[c.first_key + k] /= total;
      }
    }
    if (length) {
      length[curve] = total;
    }
  }
}

void HdCyclesCurves::PopulatePoints(HdSceneDelegate *sceneDelegate)
{
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const VtValue value = ReadPrimvar(primvars, HdTokens->points);

  if (!value.IsHolding<VtVec3fArray>()) {
    TF_WARN("Invalid points data for %s", GetId().GetText());
    return;
  }

  const auto &points = value.UncheckedGet<VtVec3fArray>();

  /* Radius has no motion steps, Cycles uses the center one for all. */
  if (!_vertexMap.empty()) {
    if (points.size() != _numControlPoints) {
      TF_WARN("Invalid points data for %s", GetId().GetText());
      return;
    }
    SetPositions(_geom->attributes.add(ATTR_STD_POSITION),
                 primvars,
                 points,
                 _geom->num_keys(),
                 [this](const VtVec3fArray &controlPoints) {
                   return _vertexMap.Apply(VtValue(controlPoints)).UncheckedGet<VtVec3fArray>();
                 });
    return;
  }

  TF_VERIFY(points.size() >= _geom->num_keys());

  SetPositions(_geom->attributes.add(ATTR_STD_POSITION),
               primvars,
               points,
               std::min(points.size(), _geom->num_keys()));
}

void HdCyclesCurves::PopulateWidths(HdSceneDelegate *sceneDelegate)
{
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const VtValue value = ReadPrimvar(primvars, HdTokens->widths);
  const HdInterpolation interpolation = ReadPrimvarInterpolation(primvars, HdTokens->widths);
  Attribute *attr_R = _geom->attributes.add(ATTR_STD_RADIUS);

  if (!value.IsHolding<VtFloatArray>()) {
    /* Without widths, width 1 like Karma and Storm. */
    if (!value.IsEmpty()) {
      TF_WARN("Invalid widths data for %s", GetId().GetText());
    }
    attr_R->remove_motion();
    std::fill_n(_geom->get_radius_for_write(), _geom->num_keys(), 0.5f);
    return;
  }

  /* Radii at the keys: vertex widths follow the curve basis, varying ones are linear. */
  const auto setRadius = [&](const VtValue &widthsValue, float *radius) {
    if (!widthsValue.IsHolding<VtFloatArray>()) {
      return false;
    }
    const VtValue keyValue = (interpolation == HdInterpolationVertex) ?
                                 _vertexMap.Apply(widthsValue) :
                             (interpolation == HdInterpolationVarying ||
                              interpolation == HdInterpolationFaceVarying) ?
                                 _varyingMap.Apply(widthsValue) :
                                 widthsValue;
    const auto &widths = keyValue.UncheckedGet<VtFloatArray>();

    if (interpolation == HdInterpolationConstant && !widths.empty()) {
      std::fill_n(radius, _geom->num_keys(), widths[0] * 0.5f);
    }
    else if (interpolation == HdInterpolationUniform) {
      if (widths.size() != _geom->num_curves()) {
        return false;
      }
      for (size_t curve = 0; curve < _geom->num_curves(); ++curve) {
        const Hair::Curve c = _geom->get_curve(curve);
        std::fill_n(radius + c.first_key, c.num_keys, widths[curve] * 0.5f);
      }
    }
    else if (widths.size() == _geom->num_keys()) {
      for (size_t i = 0; i < _geom->num_keys(); ++i) {
        radius[i] = widths[i] * 0.5f;
      }
    }
    else {
      return false;
    }
    return true;
  };

  if (!setRadius(value, _geom->get_radius_for_write())) {
    TF_WARN("Invalid widths data for %s", GetId().GetText());
  }

  /* Motion blur: radii at the motion steps of the points, when the widths are animated. Cycles
   * reads the radius steps with the position steps, so they need the same number. */
  const Attribute *attr_P = _geom->attributes.find(ATTR_STD_POSITION);
  const std::vector<float> motionTimes = GetObjectMotionTimes(
      primvars.GetPrimvar(HdTokens->points).GetPrimvarValue(), true);
  const HdSampledDataSourceHandle widthsDs =
      primvars.GetPrimvar(HdTokens->widths).GetFlattenedPrimvarValue();
  std::vector<float> sampleTimes;
  const int numSteps = int(_geom->get_motion_steps());
  if (!attr_P || !attr_P->has_motion() || int(motionTimes.size()) != numSteps || !widthsDs ||
      !widthsDs->GetContributingSampleTimesForInterval(
          motionTimes.front(), motionTimes.back(), &sampleTimes))
  {
    attr_R->remove_motion();
    return;
  }
  attr_R->add_motion(_geom);
  for (int step = 0; step < numSteps; ++step) {
    const int attrStep = attr_R->time_step_to_attr_step(step, numSteps);
    if (attrStep != 0 &&
        !setRadius(widthsDs->GetValue(motionTimes[step]), attr_R->data_for_write<float>(attrStep)))
    {
      attr_R->remove_motion(); /* Changing topology. */
      break;
    }
  }
  _geom->tag_radius_modified();
}

void HdCyclesCurves::PopulatePrimvars(HdSceneDelegate *sceneDelegate)
{
  Scene *const scene = (Scene *)_geom->get_owner();

  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const TfToken uvName = FindUVPrimvar(primvars);

  const std::pair<HdInterpolation, AttributeElement> interpolations[] = {
      std::make_pair(HdInterpolationVertex, ATTR_ELEMENT_CURVE_KEY),
      std::make_pair(HdInterpolationVarying, ATTR_ELEMENT_CURVE_KEY),
      std::make_pair(HdInterpolationFaceVarying, ATTR_ELEMENT_CURVE_KEY),
      std::make_pair(HdInterpolationUniform, ATTR_ELEMENT_CURVE),
      std::make_pair(HdInterpolationConstant, ATTR_ELEMENT_OBJECT),
  };

  for (const auto &interpolation : interpolations) {
    for (const TfToken &primvarName : PrimvarNamesAtInterpolation(primvars, interpolation.first)) {
      // Skip special primvars that are handled separately
      if (primvarName == HdTokens->points || primvarName == HdTokens->widths) {
        continue;
      }

      VtValue value = PrimvarAsFloats(ReadPrimvar(primvars, primvarName));
      if (value.IsEmpty()) {
        continue;
      }
      if (interpolation.first == HdInterpolationVertex) {
        value = _vertexMap.Apply(value);
      }
      else if (interpolation.first != HdInterpolationUniform &&
               interpolation.first != HdInterpolationConstant)
      {
        value = _varyingMap.Apply(value);
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

      AttributeElement elem = interpolation.second;
      if (std == ATTR_STD_VERTEX_NORMAL) {
        elem = ATTR_ELEMENT_CURVE_KEY_NORMAL;
      }
      else if (std == ATTR_STD_UV) {
        value = PrimvarAsFloat2(value); /* Cycles stores UVs as float2. */
      }

      const ustring ownName(primvarName.GetString());
      for (const ustring &name : names) {
        ApplyPrimvars(_geom->attributes, name, value, elem, name == ownName ? std : ATTR_STD_NONE);
      }
    }
  }
}

void HdCyclesCurves::PopulateTopology(HdSceneDelegate *sceneDelegate)
{
  // Clear geometry before populating it again with updated topology
  _geom->clear(true);

  // Ribbons or 3D curves for the whole scene, like Blender's Curves Shape setting
  _geom->curve_shape = static_cast<const Scene *>(_geom->get_owner())->params.hair_shape;

  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdBasisCurvesTopologySchema topoSchema =
      HdBasisCurvesSchema::GetFromParent(prim.dataSource).GetTopology();

  TfToken curveType = HdTokens->linear;
  if (auto ds = topoSchema.GetType()) {
    curveType = ds->GetTypedValue(0.0f);
  }
  TfToken curveBasis = HdTokens->bezier;
  if (auto ds = topoSchema.GetBasis()) {
    curveBasis = ds->GetTypedValue(0.0f);
  }
  TfToken curveWrap = HdTokens->nonperiodic;
  if (auto ds = topoSchema.GetWrap()) {
    curveWrap = ds->GetTypedValue(0.0f);
  }
  VtIntArray curveVertexCounts;
  if (auto ds = topoSchema.GetCurveVertexCounts()) {
    curveVertexCounts = ds->GetTypedValue(0.0f);
  }
  VtIntArray curveIndices;
  if (auto ds = topoSchema.GetCurveIndices()) {
    curveIndices = ds->GetTypedValue(0.0f);
  }
  const HdBasisCurvesTopology topology(
      curveType, curveBasis, curveWrap, curveVertexCounts, curveIndices);

  const VtIntArray vertCounts = topology.GetCurveVertexCounts();
  const bool cubic = curveType == HdTokens->cubic;
  const bool periodic = curveWrap == HdTokens->periodic;
  const bool pinned = curveWrap == HdTokens->pinned;

  /* Linear curves are Cycles' linear 3D curves (ribbons are always Catmull-Rom). */
  if (!cubic && _geom->curve_shape == CURVE_THICK) {
    _geom->curve_shape = CURVE_THICK_LINEAR;
  }

  /* Cycles' Catmull-Rom keys: the curve sampled 2 (B-spline, Catmull-Rom) or 3 (Bezier) times
   * per segment, which also keeps the end tangents of Catmull-Rom curves (Cycles' own end keys
   * are doubled). Periodic curves end with their first key again. Pinned B-spline and
   * Catmull-Rom curves get phantom end points (2 P0 - P1). */
  const TfToken basis = (curveBasis == HdTokens->bspline || curveBasis == HdTokens->catmullRom) ?
                            curveBasis :
                            HdTokens->bezier;
  const bool identity = !cubic && !periodic;
  _numControlPoints = 0;
  for (const int count : vertCounts) {
    _numControlPoints += size_t(std::max(count, 0));
  }
  std::vector<int> keyCounts;
  for (const int count : vertCounts) {
    keyCounts.push_back(std::max(count, 0));
  }

  if (identity) {
    _vertexMap = HdCyclesCurveKeyMap();
    _varyingMap = HdCyclesCurveKeyMap();
  }
  else {
    KeyMapBuilder vertex(_vertexMap);
    KeyMapBuilder varying(_varyingMap);
    const int samples = !cubic ? 1 : basis == HdTokens->bezier ? 3 : 2;
    const int vstep = (cubic && basis == HdTokens->bezier) ? 3 : 1;
    size_t cvOffset = 0;
    size_t varyingOffset = 0;
    for (size_t curve = 0; curve < vertCounts.size(); ++curve) {
      const int n = std::max(vertCounts[curve], 0);
      /* Segments and the control point index of each segment's first point. */
      int numSegments = 0;
      int firstPoint = 0;
      if (!cubic) {
        numSegments = periodic ? n : n - 1;
      }
      else if (basis == HdTokens->bezier) {
        numSegments = periodic ? n / 3 : (n - 1) / 3;
      }
      else {
        numSegments = periodic ? n : pinned ? n - 1 : n - 3;
        firstPoint = pinned ? -1 : 0;
      }
      const int numVarying = !cubic ? n : periodic ? numSegments : numSegments + 1;

      const auto addPoint = [&](const int i, const float weight) {
        if (periodic) {
          vertex.AddTerm(int(cvOffset) + (i % n + n) % n, weight);
        }
        else if (i < 0) { /* Pinned phantom points. */
          vertex.AddTerm(int(cvOffset), 2.0f * weight);
          vertex.AddTerm(int(cvOffset) + 1, -weight);
        }
        else if (i >= n) {
          vertex.AddTerm(int(cvOffset) + n - 1, 2.0f * weight);
          vertex.AddTerm(int(cvOffset) + n - 2, -weight);
        }
        else {
          vertex.AddTerm(int(cvOffset) + i, weight);
        }
      };
      const auto addKey = [&](const int segment, const float t) {
        if (!cubic) {
          addPoint(segment + (t > 0.0f ? 1 : 0), 1.0f);
        }
        else {
          const std::array<float, 4> weights = CubicWeights(basis, t);
          for (int j = 0; j < 4; ++j) {
            addPoint(firstPoint + segment * vstep + j, weights[j]);
          }
        }
        vertex.EndKey();
        const int v0 = segment % std::max(numVarying, 1);
        const int v1 = (segment + 1) % std::max(numVarying, 1);
        varying.AddTerm(int(varyingOffset) + v0, 1.0f - t);
        varying.AddTerm(int(varyingOffset) + v1, t);
        varying.EndKey();
      };

      if (numSegments < 1 || n < (cubic ? (basis == HdTokens->bezier ? 4 : 2) : 2)) {
        /* Not a valid curve: its control points as keys (one zero key without any). */
        for (int i = 0; i < std::max(n, 1); ++i) {
          if (n > 0) {
            vertex.AddTerm(int(cvOffset) + std::min(i, n - 1), 1.0f);
          }
          vertex.EndKey();
          if (numVarying > 0) {
            varying.AddTerm(int(varyingOffset) + std::min(i, numVarying - 1), 1.0f);
          }
          varying.EndKey();
        }
        keyCounts[curve] = std::max(n, 1);
      }
      else {
        for (int segment = 0; segment < numSegments; ++segment) {
          for (int sample = 0; sample < samples; ++sample) {
            addKey(segment, float(sample) / float(samples));
          }
        }
        /* The end of the last segment; periodic curves end at their start. */
        if (periodic) {
          addKey(0, 0.0f);
        }
        else {
          addKey(numSegments - 1, 1.0f);
        }
        keyCounts[curve] = numSegments * samples + 1;
      }
      cvOffset += n;
      varyingOffset += std::max(numVarying, 0);
    }
    _vertexMap.sourceSize = _numControlPoints;
    _varyingMap.sourceSize = varyingOffset;
  }

  size_t numKeys = 0;
  for (const int count : keyCounts) {
    numKeys += count;
  }
  _geom->resize_curves(topology.GetNumCurves(), numKeys);

  int *curve_first_key = _geom->get_curve_first_key().data();

  for (int curve = 0, key = 0; curve < topology.GetNumCurves(); ++curve) {
    // Always reference shader at index zero, which is the primitive material
    curve_first_key[curve] = key;

    key += keyCounts[curve];
  }

  std::ranges::fill(_geom->get_curve_shader(), 0);

  _geom->tag_curve_first_key_modified();
  _geom->tag_curve_shader_modified();
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
