/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/mesh.h"
#include "hydra/geometry.inl"
#include "hydra/mesh_attributes.h"
#include "hydra/util.h"
#include "scene/mesh.h"

#include <algorithm>

#include <pxr/base/gf/vec2f.h>
#include <pxr/imaging/hd/geomSubsetSchema.h>
#include <pxr/imaging/hd/legacyDisplayStyleSchema.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/subdivisionTagsSchema.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

void RemoveUVTangents(AttributeSet &attributes, const ustring &name)
{
  for (const char *suffix :
       {".tangent", ".tangent_sign", ".undisplaced_tangent", ".undisplaced_tangent_sign"})
  {
    attributes.remove(ustring(name.string() + suffix));
  }
}

template<typename T>
VtValue ComputeTriangulatedUniformPrimvar(VtValue value, const VtIntArray &primitiveParams)
{
  T output;
  output.reserve(primitiveParams.size());
  const T &input = value.Get<T>();

  for (size_t i = 0; i < primitiveParams.size(); ++i) {
    const int faceIndex = HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(primitiveParams[i]);

    output.push_back(input[faceIndex]);
  }

  return VtValue(output);
}

VtValue ComputeTriangulatedUniformPrimvar(VtValue value,
                                          const HdType valueType,
                                          const VtIntArray &primitiveParams)
{
  switch (valueType) {
    case HdTypeFloat:
      return ComputeTriangulatedUniformPrimvar<VtFloatArray>(value, primitiveParams);
    case HdTypeFloatVec2:
      return ComputeTriangulatedUniformPrimvar<VtVec2fArray>(value, primitiveParams);
    case HdTypeFloatVec3:
      return ComputeTriangulatedUniformPrimvar<VtVec3fArray>(value, primitiveParams);
    case HdTypeFloatVec4:
      return ComputeTriangulatedUniformPrimvar<VtVec4fArray>(value, primitiveParams);
    default:
      TF_RUNTIME_ERROR("Unsupported attribute type %d", static_cast<int>(valueType));
      return VtValue();
  }
}

/* Cycles reads the UV map as float2 per face corner (mikktspace tangents, primitive_uv), so expand
 * UVs at other interpolations to face-varying. */
VtVec2fArray ComputeFaceVaryingUV(const VtVec2fArray &uv,
                                  const HdInterpolation interpolation,
                                  const HdMeshTopology &topology)
{
  if (interpolation == HdInterpolationFaceVarying) {
    return uv;
  }
  const VtIntArray &counts = topology.GetFaceVertexCounts();
  const VtIntArray &indices = topology.GetFaceVertexIndices();
  VtVec2fArray result;
  result.reserve(indices.size());
  for (size_t face = 0, corner = 0; face < counts.size(); ++face) {
    for (int i = 0; i < counts[face]; ++i, ++corner) {
      const size_t index = (interpolation == HdInterpolationUniform)  ? face :
                           (interpolation == HdInterpolationConstant) ? 0 :
                           (corner < indices.size())                  ? size_t(indices[corner]) :
                                                                        uv.size();
      if (index >= uv.size()) {
        return {};
      }
      result.push_back(uv[index]);
    }
  }
  return result;
}

VtValue ComputeTriangulatedFaceVaryingPrimvar(VtValue value,
                                              const HdType valueType,
                                              HdMeshUtil &meshUtil)
{
  if (meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
          HdGetValueData(value), value.GetArraySize(), valueType, &value)
#if PXR_VERSION >= 2511
      != HdMeshComputationResult::Error
#endif
  )
  {
    return value;
  }

  return VtValue();
}

/* Pointiness and Random Per Island, which Blender's sync computes, when a shader reads them. Like
 * Blender, on the control mesh of subdivision meshes, interpolated by the tessellation. */
void PopulateIslandAttributes(Mesh *mesh,
                              Scene *scene,
                              const HdMeshTopology &topology,
                              const VtIntArray &primitiveParams)
{
  const bool subdivision = mesh->get_subdivision_type() != Mesh::SUBDIVISION_NONE;
  AttributeSet &attributes = subdivision ? mesh->subd_attributes : mesh->attributes;
  attributes.remove(ATTR_STD_POINTINESS);
  attributes.remove(ATTR_STD_RANDOM_PER_ISLAND);

  const bool needPointiness = mesh->need_attribute(scene, ATTR_STD_POINTINESS);
  const bool needIsland = mesh->need_attribute(scene, ATTR_STD_RANDOM_PER_ISLAND);
  const Attribute *attr_P = attributes.find(ATTR_STD_POSITION);
  if ((!needPointiness && !needIsland) || !attr_P) {
    return;
  }

  const VtIntArray &counts = topology.GetFaceVertexCounts();
  const VtIntArray &indices = topology.GetFaceVertexIndices();
  const int numVerts = topology.GetNumPoints();
  /* Topology from the scene: indices past the points would be read out of bounds. */
  size_t numCorners = 0;
  for (const int count : counts) {
    numCorners += size_t(std::max(count, 0));
  }
  if (attr_P->size < numVerts || numCorners != indices.size() ||
      std::ranges::any_of(counts, [](const int count) { return count < 0; }) ||
      std::ranges::any_of(indices, [numVerts](const int v) { return v < 0 || v >= numVerts; }))
  {
    return;
  }

  const PolygonMesh polygons = {attr_P->data<packed_float3>(),
                                numVerts,
                                counts.data(),
                                counts.size(),
                                indices.data(),
                                topology.GetOrientation() == HdTokens->leftHanded};

  if (needPointiness) {
    const std::vector<float> pointiness = compute_pointiness(polygons);
    std::ranges::copy(pointiness, attributes.add(ATTR_STD_POINTINESS)->data_for_write<float>());
  }
  if (needIsland) {
    const std::vector<float> island = compute_random_per_island(polygons);
    float *data = attributes.add(ATTR_STD_RANDOM_PER_ISLAND)->data_for_write<float>();
    if (subdivision) {
      std::ranges::copy(island, data);
    }
    else {
      /* Per triangle, from the polygon it was cut from. */
      for (size_t i = 0; i < primitiveParams.size(); ++i) {
        data[i] = island[HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(primitiveParams[i])];
      }
    }
  }
}

}  // namespace

Transform convert_transform(const GfMatrix4d &matrix)
{
  return make_transform(matrix[0][0],
                        matrix[1][0],
                        matrix[2][0],
                        matrix[3][0],
                        matrix[0][1],
                        matrix[1][1],
                        matrix[2][1],
                        matrix[3][1],
                        matrix[0][2],
                        matrix[1][2],
                        matrix[2][2],
                        matrix[3][2]);
}

HdCyclesMesh::HdCyclesMesh(const SdfPath &rprimId)
    : HdCyclesGeometry(rprimId), _util(&_topology, rprimId)
{
}

HdCyclesMesh::~HdCyclesMesh() = default;

HdDirtyBits HdCyclesMesh::GetInitialDirtyBitsMask() const
{
  HdDirtyBits bits = HdCyclesGeometry::GetInitialDirtyBitsMask();
  bits |= HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyNormals |
          HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyTopology |
          HdChangeTracker::DirtyDisplayStyle | HdChangeTracker::DirtySubdivTags;
  return bits;
}

HdDirtyBits HdCyclesMesh::_PropagateDirtyBits(HdDirtyBits bits) const
{
  if (bits & (HdChangeTracker::DirtyMaterialId)) {
    // Update used shaders from geometry subsets if any exist in the topology
    bits |= HdChangeTracker::DirtyTopology;
  }

  if (bits & (HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyDisplayStyle |
              HdChangeTracker::DirtySubdivTags))
  {
    // Do full topology update when display style or subdivision changes
    bits |= HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyDisplayStyle |
            HdChangeTracker::DirtySubdivTags;
  }

  if (bits & (HdChangeTracker::DirtyTopology)) {
    // Changing topology clears the geometry, so need to populate everything again
    bits |= HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyNormals |
            HdChangeTracker::DirtyPrimvar;
  }

  return bits;
}

void HdCyclesMesh::Populate(HdSceneDelegate *sceneDelegate, HdDirtyBits dirtyBits, bool &rebuild)
{
  /* Rebuild subdivision and displacement from the authored geometry when primvars change.
   * Decided here, after the shaders synced, because displacement depends on shader state. */
  if ((dirtyBits & HdChangeTracker::DirtyPrimvar) &&
      (_geom->get_subdivision_type() != Mesh::SUBDIVISION_NONE || _geom->has_true_displacement() ||
       IsAdaptiveSubdivision(sceneDelegate) != _adaptiveSubdivision))
  {
    dirtyBits |= HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyPoints |
                 HdChangeTracker::DirtyNormals;
  }

  if (HdChangeTracker::IsTopologyDirty(dirtyBits, GetId())) {
    PopulateTopology(sceneDelegate);
  }

  if (dirtyBits & HdChangeTracker::DirtyPoints) {
    PopulatePoints(sceneDelegate);
  }

  // Must happen after topology update, so that normals attribute size can be calculated. Moved
  // points need it too: Cycles computes the normals of meshes without authored ones only when
  // the attribute is missing, and Hydra sends no DirtyNormals for them.
  if (dirtyBits & (HdChangeTracker::DirtyNormals | HdChangeTracker::DirtyPoints)) {
    PopulateNormals(sceneDelegate);
  }

  /* Moved points change the pointiness, and shader changes resync the primvars. Before the
   * primvars, which tag an attribute-only update. */
  if (dirtyBits & (HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyPrimvar)) {
    PopulateIslandAttributes(_geom, (Scene *)_geom->get_owner(), _topology, _primitiveParams);
  }

  // Must happen after topology update, so that appropriate attribute set can be selected
  if (dirtyBits & HdChangeTracker::DirtyPrimvar) {
    PopulatePrimvars(sceneDelegate);
  }

  rebuild = (_geom->triangles_is_modified()) || (_geom->subd_start_corner_is_modified()) ||
            (_geom->subd_num_corners_is_modified()) || (_geom->subd_shader_is_modified()) ||
            (_geom->subd_smooth_is_modified()) || (_geom->subd_ptex_offset_is_modified()) ||
            (_geom->subd_face_corners_is_modified());
}

void HdCyclesMesh::PopulatePoints(HdSceneDelegate *sceneDelegate)
{
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const VtValue value = ReadPrimvar(primvars, HdTokens->points);

  if (!value.IsHolding<VtVec3fArray>()) {
    TF_WARN("Invalid points data for %s", GetId().GetText());
    return;
  }

  const VtVec3fArray &points = value.UncheckedGet<VtVec3fArray>();
  const size_t numPoints = _topology.GetNumPoints();

  TF_VERIFY(points.size() >= numPoints);

  const bool subdivision = _geom->get_subdivision_type() != Mesh::SUBDIVISION_NONE;
  if (subdivision) {
    /* Drop the previous tessellation, like the topology update: Mesh::tessellate takes every
     * vertex of the mesh as a base vertex, and reads the positions past their end otherwise. */
    _geom->clear_tessellation();
  }
  AttributeSet &attributes = (subdivision) ? _geom->subd_attributes : _geom->attributes;
  SetPositions(attributes.add(ATTR_STD_POSITION), primvars, points, numPoints);
}

void HdCyclesMesh::PopulateNormals(HdSceneDelegate *sceneDelegate)
{
  _geom->attributes.remove(ATTR_STD_VERTEX_NORMAL);
  _geom->attributes.remove(ATTR_STD_CORNER_NORMAL);
  _geom->subd_attributes.remove(ATTR_STD_VERTEX_NORMAL);

  // Authored normals should only exist on triangle meshes
  if (_geom->get_subdivision_type() != Mesh::SUBDIVISION_NONE) {
    return;
  }

  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const HdInterpolation interpolation = ReadPrimvarInterpolation(primvars, HdTokens->normals);
  if (interpolation == HdInterpolationCount) {
    return;  // Ignore missing normals
  }
  const VtValue value = ReadPrimvar(primvars, HdTokens->normals);

  if (!value.IsHolding<VtVec3fArray>()) {
    TF_WARN("Invalid normals data for %s", GetId().GetText());
    return;
  }

  const auto &normals = value.UncheckedGet<VtVec3fArray>();

  if (interpolation == HdInterpolationConstant) {
    TF_VERIFY(normals.size() == 1);

    const GfVec3f constantNormal = normals[0];

    packed_normal *const N =
        _geom->attributes.add(ATTR_STD_VERTEX_NORMAL)->data_for_write<packed_normal>();
    for (size_t i = 0; i < _geom->num_verts(); ++i) {
      N[i] = packed_normal(make_float3(constantNormal[0], constantNormal[1], constantNormal[2]));
    }
  }
  else if (interpolation == HdInterpolationUniform) {
    TF_VERIFY(normals.size() == static_cast<size_t>(_topology.GetNumFaces()));
    /* Nothing to do, face normals are computed on demand in the kernel. */
  }
  else if (interpolation == HdInterpolationVertex || interpolation == HdInterpolationVarying) {
    TF_VERIFY(normals.size() == static_cast<size_t>(_topology.GetNumPoints()) &&
              static_cast<size_t>(_topology.GetNumPoints()) == _geom->num_verts());

    packed_normal *const N =
        _geom->attributes.add(ATTR_STD_VERTEX_NORMAL)->data_for_write<packed_normal>();
    for (size_t i = 0; i < _geom->num_verts(); ++i) {
      N[i] = packed_normal(make_float3(normals[i][0], normals[i][1], normals[i][2]));
    }
  }
  else if (interpolation == HdInterpolationFaceVarying) {
    TF_VERIFY(normals.size() == static_cast<size_t>(_topology.GetNumFaceVaryings()));

    const VtValue triangulated = ComputeTriangulatedFaceVaryingPrimvar(
        value, HdTypeFloatVec3, _util);
    if (!triangulated.IsHolding<VtVec3fArray>() ||
        triangulated.GetArraySize() != _geom->num_triangles() * 3)
    {
      return;
    }
    const auto &cornerNormals = triangulated.UncheckedGet<VtVec3fArray>();

    packed_normal *const N =
        _geom->attributes.add(ATTR_STD_CORNER_NORMAL)->data_for_write<packed_normal>();
    for (size_t i = 0; i < cornerNormals.size(); ++i) {
      N[i] = packed_normal(
          make_float3(cornerNormals[i][0], cornerNormals[i][1], cornerNormals[i][2]));
    }
  }
}

void HdCyclesMesh::PopulatePrimvars(HdSceneDelegate *sceneDelegate)
{
  Scene *const scene = (Scene *)_geom->get_owner();

  const bool subdivision = _geom->get_subdivision_type() != Mesh::SUBDIVISION_NONE;
  AttributeSet &attributes = subdivision ? _geom->subd_attributes : _geom->attributes;

  /* Remove UVs before reading them again. */
  while (const Attribute *previous = attributes.find(ATTR_STD_UV)) {
    RemoveUVTangents(_geom->attributes, previous->name);
    attributes.remove(previous->name);
  }

  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const TfToken uvName = FindUVPrimvar(primvars);
  const size_t numPoints = ReadPrimvar(primvars, HdTokens->points).GetArraySize();
  /* The bake UV map of the bake target, exported even when no shader reads it. */
  const BakeSettings bake = _session->GetBake();
  const TfToken bakeUVName = bake.object == GetId().GetString() ? TfToken(bake.uvMap) :
                                                                  TfToken();

  const std::pair<HdInterpolation, AttributeElement> interpolations[] = {
      std::make_pair(HdInterpolationFaceVarying, ATTR_ELEMENT_CORNER),
      std::make_pair(HdInterpolationUniform, ATTR_ELEMENT_FACE),
      std::make_pair(HdInterpolationVertex, ATTR_ELEMENT_VERTEX),
      std::make_pair(HdInterpolationVarying, ATTR_ELEMENT_VERTEX),
      std::make_pair(HdInterpolationConstant, ATTR_ELEMENT_OBJECT),
  };

  for (const auto &interpolation : interpolations) {
    for (const TfToken &primvarName : PrimvarNamesAtInterpolation(primvars, interpolation.first)) {
      // Skip special primvars that are handled separately
      if (primvarName == HdTokens->points || primvarName == HdTokens->normals) {
        continue;
      }

      VtValue value = PrimvarAsFloats(ReadPrimvar(primvars, primvarName));
      if (value.IsEmpty()) {
        continue;
      }

      const AttributeStandard std = PrimvarStandard(primvars, primvarName, uvName);
      if (std == ATTR_STD_VERTEX_COLOR && interpolation.first == HdInterpolationConstant) {
        SetInstanceColor(value);
      }

      // Skip attributes that are not needed
      std::vector<ustring> names = RequestedPrimvarNames(
          _geom, scene, primvars, primvarName, std);
      const bool bakeUV = !bakeUVName.IsEmpty() && primvarName == bakeUVName;
      if (bakeUV &&
          std::find(names.begin(), names.end(), ustring(primvarName.GetString())) == names.end())
      {
        names.emplace_back(primvarName.GetString());
      }
      if (names.empty()) {
        continue;
      }

      HdInterpolation primvarInterpolation = interpolation.first;
      AttributeElement elem = interpolation.second;
      /* The bake UV map is read like the UV map: float2 per face corner (bake.cpp). */
      if (std == ATTR_STD_UV || bakeUV) {
        const VtVec2fArray uv = ComputeFaceVaryingUV(
            PrimvarAsFloat2(value), primvarInterpolation, _topology);
        if (uv.empty()) {
          continue;
        }
        value = uv;
        primvarInterpolation = HdInterpolationFaceVarying;
        elem = ATTR_ELEMENT_CORNER;
      }

      const HdType valueType = HdGetValueTupleType(value).type;

      if (!subdivision) {
        // Adjust attributes for polygons that were triangulated
        if (primvarInterpolation == HdInterpolationUniform) {
          /* Read per face, a short array would be read out of bounds. */
          if (value.GetArraySize() != size_t(_topology.GetNumFaces())) {
            TF_WARN("Primvar %s has %zu values, expected %d",
                    primvarName.GetText(),
                    value.GetArraySize(),
                    _topology.GetNumFaces());
            continue;
          }
          value = ComputeTriangulatedUniformPrimvar(value, valueType, _primitiveParams);
          if (value.IsEmpty()) {
            continue;
          }
        }
        else if (primvarInterpolation == HdInterpolationFaceVarying) {
          value = ComputeTriangulatedFaceVaryingPrimvar(value, valueType, _util);
          if (value.IsEmpty()) {
            continue;
          }
        }
      }

      /* A value for every point: the mesh only has the points up to the last one its faces use
       * (Houdini keeps unused points after a Blast), the values of the others are left out. */
      const bool unusedPoints = elem == ATTR_ELEMENT_VERTEX && value.GetArraySize() == numPoints;

      const ustring ownName(primvarName.GetString());
      for (const ustring &name : names) {
        /* Recreate, in case the type, role or interpolation changed. */
        if (Attribute *previous = attributes.find(name)) {
          if (previous->std == ATTR_STD_NONE || previous->std == ATTR_STD_UV ||
              previous->std == ATTR_STD_VERTEX_COLOR)
          {
            RemoveUVTangents(_geom->attributes, name);
            attributes.remove(name);
          }
        }
        ApplyPrimvars(
            attributes, name, value, elem, name == ownName ? std : ATTR_STD_NONE, unusedPoints);
        /* Copies of the UV map under other names subdivide smoothly like the UV map. */
        Attribute *attr = (std == ATTR_STD_UV) ? attributes.find(name) : nullptr;
        if (attr) {
          attr->flags |= ATTR_SUBDIVIDE_SMOOTH_FVAR;
        }
      }
    }
  }

  /* Tangents are derived from UVs and may include named or fallback maps. */
  for (const AttributeStandard std : {ATTR_STD_UV_TANGENT,
                                      ATTR_STD_UV_TANGENT_SIGN,
                                      ATTR_STD_UV_TANGENT_UNDISPLACED,
                                      ATTR_STD_UV_TANGENT_SIGN_UNDISPLACED})
  {
    while (_geom->attributes.find(std)) {
      _geom->attributes.remove(std);
    }
  }

  if (!subdivision && !_geom->is_modified()) {
    /* An attribute-only edit does not need a BVH update. */
    _geom->add_vertex_normals();
    _geom->update_tangents(scene, true);
    _geom->update_tangents(scene, false);
    _attributesChanged = true;
  }
}

bool HdCyclesMesh::IsAdaptiveSubdivision(HdSceneDelegate *sceneDelegate) const
{
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  return VtValue::Cast<bool>(GetObjectSetting(HdPrimvarsSchema::GetFromParent(prim.dataSource),
                                              "use_adaptive_subdivision"))
      .GetWithDefault(false);
}

void HdCyclesMesh::PopulateTopology(HdSceneDelegate *sceneDelegate)
{
  // Clear geometry before populating it again with updated topology
  _geom->clear(true);

  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdLegacyDisplayStyleSchema displayStyleSchema = HdLegacyDisplayStyleSchema::GetFromParent(
      prim.dataSource);

  int refineLevel = 0;
  if (auto ds = displayStyleSchema.GetRefineLevel()) {
    refineLevel = ds->GetTypedValue(0.0f);
  }
  bool flatShadingEnabled = false;
  if (auto ds = displayStyleSchema.GetFlatShadingEnabled()) {
    flatShadingEnabled = ds->GetTypedValue(0.0f);
  }
  /* Uniform normals are one per face: the faces are flat, as USD means and Karma and Storm render them. Cycles
   * computes face normals only for flat faces; smooth ones averaged the corners (a roof of two faces at 90°: 0.571
   * with a 0.20 gradient across each face, Karma 0.446 flat). From hdcycles-blender patch 0017. */
  if (ReadPrimvarInterpolation(HdPrimvarsSchema::GetFromParent(prim.dataSource), HdTokens->normals) ==
      HdInterpolationUniform)
  {
    flatShadingEnabled = true;
  }

  const HdMeshSchema meshSchema = HdMeshSchema::GetFromParent(prim.dataSource);
  const HdMeshTopologySchema topoSchema = meshSchema.GetTopology();

  TfToken scheme = PxOsdOpenSubdivTokens->none;
  if (auto ds = meshSchema.GetSubdivisionScheme()) {
    scheme = ds->GetTypedValue(0.0f);
  }
  TfToken orientation = HdTokens->rightHanded;
  if (auto ds = topoSchema.GetOrientation()) {
    orientation = ds->GetTypedValue(0.0f);
  }
  VtIntArray faceVertexCounts;
  if (auto ds = topoSchema.GetFaceVertexCounts()) {
    faceVertexCounts = ds->GetTypedValue(0.0f);
  }
  VtIntArray faceVertexIndices;
  if (auto ds = topoSchema.GetFaceVertexIndices()) {
    faceVertexIndices = ds->GetTypedValue(0.0f);
  }
  VtIntArray holeIndices;
  if (auto ds = topoSchema.GetHoleIndices()) {
    holeIndices = ds->GetTypedValue(0.0f);
  }

  /* Blender's Adaptive Subdivision: Cycles dices the mesh to the dicing rate in pixels, up to
   * the Max Subdivisions render setting, also without a refine level. Without it, the mesh is
   * subdivided up to its refine level. */
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const bool adaptive = IsAdaptiveSubdivision(sceneDelegate);
  _adaptiveSubdivision = adaptive;
  /* Without the Cycles setting, Karma's Dicing Quality: quality 2 dices twice as finely, so half
   * the dicing rate (approximate, Karma's micropolygon size is not specified in pixels). */
  VtValue dicingRate = GetObjectSetting(primvars, "dicing_rate");
  if (dicingRate.IsEmpty()) {
    const float quality = VtValue::Cast<float>(
                              ReadPrimvar(primvars, TfToken("karma:object:dicingquality")))
                              .GetWithDefault(0.0f);
    if (quality > 0.0f) {
      dicingRate = VtValue(1.0f / quality);
    }
  }
  const float dicingScale = std::max(
      0.0f, VtValue::Cast<float>(dicingRate).GetWithDefault(1.0f));
  if (adaptive) {
    refineLevel = std::max(refineLevel, 1);
    /* Houdini's SOP Import authors polygon meshes as "none": dice them flat (Blender's Simple
     * subdivision), so displacement has vertices to move, like Karma dicing polygons. */
    if (scheme == PxOsdOpenSubdivTokens->none) {
      scheme = PxOsdOpenSubdivTokens->bilinear;
    }
  }

  /* Use refinement only for supported subdivision meshes, so triangle indices
   * and face-varying primvars agree about which holes to omit. */
  const bool useSubdivision = refineLevel > 0 && (scheme == PxOsdOpenSubdivTokens->bilinear ||
                                                  scheme == PxOsdOpenSubdivTokens->catmullClark);
  _topology = HdMeshTopology(scheme,
                             orientation,
                             faceVertexCounts,
                             faceVertexIndices,
                             holeIndices,
                             useSubdivision ? refineLevel : 0);

  /* Geom subsets are published as child prims of the mesh in the scene index. */
  HdGeomSubsets geomSubsetsList;
  if (HdSceneIndexBaseRefPtr si = sceneDelegate->GetRenderIndex().GetTerminalSceneIndex()) {
    for (const SdfPath &childPath : si->GetChildPrimPaths(GetId())) {
      const HdSceneIndexPrim childPrim = si->GetPrim(childPath);
      if (childPrim.primType != HdPrimTypeTokens->geomSubset) {
        continue;
      }
      const HdGeomSubsetSchema subsetSchema = HdGeomSubsetSchema::GetFromParent(
          childPrim.dataSource);
      /* Only face subsets supported here, not point or curve subsets. */
      if (auto typeDs = subsetSchema.GetType()) {
        if (typeDs->GetTypedValue(0.0f) != HdGeomSubsetSchemaTokens->typeFaceSet) {
          continue;
        }
      }
      HdGeomSubset subset;
      subset.type = HdGeomSubset::TypeFaceSet;
      if (auto ds = subsetSchema.GetIndices()) {
        subset.indices = ds->GetTypedValue(0.0f);
      }
      if (auto pathDs = HdMaterialBindingsSchema::GetFromParent(childPrim.dataSource)
                            .GetMaterialBinding()
                            .GetPath())
      {
        subset.materialId = pathDs->GetTypedValue(0.0f);
      }
      geomSubsetsList.push_back(subset);
    }
  }
  _topology.SetGeomSubsets(geomSubsetsList);

  const TfToken subdivScheme = _topology.GetScheme();
  if (subdivScheme == PxOsdOpenSubdivTokens->bilinear && _topology.GetRefineLevel() > 0) {
    _geom->set_subdivision_type(Mesh::SUBDIVISION_LINEAR);
  }
  else if (subdivScheme == PxOsdOpenSubdivTokens->catmullClark && _topology.GetRefineLevel() > 0) {
    _geom->set_subdivision_type(Mesh::SUBDIVISION_CATMULL_CLARK);
  }
  else {
    _geom->set_subdivision_type(Mesh::SUBDIVISION_NONE);
  }

  const bool smooth = !flatShadingEnabled;
  const bool subdivision = _geom->get_subdivision_type() != Mesh::SUBDIVISION_NONE;

  // Initialize lookup table from polygon face to material shader index
  VtIntArray faceShaders(_topology.GetNumFaces(), 0);

  const HdGeomSubsets &geomSubsets = _topology.GetGeomSubsets();
  if (!geomSubsets.empty()) {
    array<Node *> usedShaders = std::move(_geom->get_used_shaders());
    // Remove any previous materials except for the material assigned to the prim
    usedShaders.resize(1);

    std::unordered_map<SdfPath, int, SdfPath::Hash> materials;

    for (const HdGeomSubset &geomSubset : geomSubsets) {
      TF_VERIFY(geomSubset.type == HdGeomSubset::TypeFaceSet);

      int shader = 0;
      const auto it = materials.find(geomSubset.materialId);
      if (it != materials.end()) {
        shader = it->second;
      }
      else {
        const auto *const material = static_cast<const HdCyclesMaterial *>(
            sceneDelegate->GetRenderIndex().GetSprim(HdPrimTypeTokens->material,
                                                     geomSubset.materialId));

        if (material && material->GetCyclesShader()) {
          shader = static_cast<int>(usedShaders.size());
          usedShaders.push_back_slow(material->GetCyclesShader());

          materials.emplace(geomSubset.materialId, shader);
        }
      }

      for (const int face : geomSubset.indices) {
        faceShaders[face] = shader;
      }
    }

    _geom->set_used_shaders(usedShaders);
  }

  const VtIntArray vertIndx = _topology.GetFaceVertexIndices();
  const VtIntArray vertCounts = _topology.GetFaceVertexCounts();

  if (!subdivision) {
    VtVec3iArray triangles;
    _util.ComputeTriangleIndices(&triangles, &_primitiveParams);

    _geom->resize_mesh(_topology.GetNumPoints(), triangles.size());

    int *geom_indices = _geom->get_triangles().data();
    for (size_t i = 0; i < _primitiveParams.size(); ++i) {
      const GfVec3i triangle = triangles[i];
      geom_indices[i * 3 + 0] = triangle[0];
      geom_indices[i * 3 + 1] = triangle[1];
      geom_indices[i * 3 + 2] = triangle[2];
    }

    int *shader = _geom->get_shader().data();
    for (size_t i = 0; i < _primitiveParams.size(); ++i) {
      const int faceIndex = HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(_primitiveParams[i]);
      shader[i] = faceShaders[faceIndex];
    }

    std::ranges::fill(_geom->get_smooth(), smooth);

    _geom->tag_triangles_modified();
    _geom->tag_shader_modified();
    _geom->tag_smooth_modified();
  }
  else {
    /* The subdivision tags are in the mesh schema (mesh.subdivisionTags). */
    const HdSubdivisionTagsSchema subdivSchema = meshSchema.GetSubdivisionTags();
    PxOsdSubdivTags subdivTags;
    if (auto ds = subdivSchema.GetInterpolateBoundary()) {
      subdivTags.SetVertexInterpolationRule(ds->GetTypedValue(0.0f));
    }
    if (auto ds = subdivSchema.GetFaceVaryingLinearInterpolation()) {
      subdivTags.SetFaceVaryingInterpolationRule(ds->GetTypedValue(0.0f));
    }
    if (auto ds = subdivSchema.GetTriangleSubdivisionRule()) {
      subdivTags.SetTriangleSubdivision(ds->GetTypedValue(0.0f));
    }
    if (auto ds = subdivSchema.GetCornerIndices()) {
      subdivTags.SetCornerIndices(ds->GetTypedValue(0.0f));
    }
    if (auto ds = subdivSchema.GetCornerSharpnesses()) {
      subdivTags.SetCornerWeights(ds->GetTypedValue(0.0f));
    }
    if (auto ds = subdivSchema.GetCreaseIndices()) {
      subdivTags.SetCreaseIndices(ds->GetTypedValue(0.0f));
    }
    if (auto ds = subdivSchema.GetCreaseLengths()) {
      subdivTags.SetCreaseLengths(ds->GetTypedValue(0.0f));
    }
    if (auto ds = subdivSchema.GetCreaseSharpnesses()) {
      subdivTags.SetCreaseWeights(ds->GetTypedValue(0.0f));
    }
    _topology.SetSubdivTags(subdivTags);

    size_t numCorners = 0;
    for (const int vertCount : vertCounts) {
      numCorners += vertCount;
    }

    _geom->resize_subd_faces(_topology.GetNumFaces(), numCorners);
    _geom->resize_mesh(_topology.GetNumPoints(), 0);
    Attribute *subd_attr_P = _geom->subd_attributes.add(ATTR_STD_POSITION);
    subd_attr_P->resize(_topology.GetNumPoints());

    std::copy_n(vertIndx.data(), vertIndx.size(), _geom->get_subd_face_corners().data());

    int *subd_start_corner = _geom->get_subd_start_corner().data();
    int *subd_num_corners = _geom->get_subd_num_corners().data();
    int *subd_ptex_offset = _geom->get_subd_ptex_offset().data();

    // TODO: Handle hole indices
    int ptex_offset = 0;
    size_t faceIndex = 0;
    size_t indexOffset = 0;
    for (const int vertCount : vertCounts) {
      subd_start_corner[faceIndex] = indexOffset;
      subd_num_corners[faceIndex] = vertCount;
      subd_ptex_offset[faceIndex] = ptex_offset;
      const int num_ptex = (vertCount == 4) ? 1 : vertCount;
      ptex_offset += num_ptex;

      faceIndex++;
      indexOffset += vertCount;
    }

    std::copy_n(faceShaders.data(), faceShaders.size(), _geom->get_subd_shader().data());
    std::ranges::fill(_geom->get_subd_smooth(), smooth);

    _geom->tag_subd_face_corners_modified();
    _geom->tag_subd_start_corner_modified();
    _geom->tag_subd_num_corners_modified();
    _geom->tag_subd_shader_modified();
    _geom->tag_subd_smooth_modified();
    _geom->tag_subd_ptex_offset_modified();

    const VtIntArray creaseLengths = subdivTags.GetCreaseLengths();
    if (!creaseLengths.empty()) {
      size_t numCreases = 0;
      for (const int creaseLength : creaseLengths) {
        numCreases += creaseLength - 1;
      }

      _geom->reserve_subd_creases(numCreases);

      const VtIntArray creaseIndices = subdivTags.GetCreaseIndices();
      const VtFloatArray creaseWeights = subdivTags.GetCreaseWeights();

      indexOffset = 0;
      size_t creaseLengthOffset = 0;
      size_t createWeightOffset = 0;
      for (const int creaseLength : creaseLengths) {
        for (int j = 0; j < creaseLength - 1; ++j, ++createWeightOffset) {
          const int v0 = creaseIndices[indexOffset + j];
          const int v1 = creaseIndices[indexOffset + j + 1];

          const float weight = creaseWeights.size() == creaseLengths.size() ?
                                   creaseWeights[creaseLengthOffset] :
                                   creaseWeights[createWeightOffset];

          /* USD sharpness is OpenSubdiv's (10 = infinitely sharp), Cycles' crease is that / 10. */
          _geom->add_edge_crease(v0, v1, weight / 10.0f);
        }

        indexOffset += creaseLength;
        creaseLengthOffset++;
      }
    }

    /* Corners also without edge creases. */
    const VtIntArray cornerIndices = subdivTags.GetCornerIndices();
    const VtFloatArray cornerWeights = subdivTags.GetCornerWeights();
    for (size_t i = 0; i < std::min(cornerIndices.size(), cornerWeights.size()); ++i) {
      _geom->add_vertex_crease(cornerIndices[i], cornerWeights[i] / 10.0f);
    }

    /* Boundary and face-varying rules, with the USD fallbacks when not authored. */
    const TfToken boundary = subdivTags.GetVertexInterpolationRule();
    _geom->set_subdivision_boundary_interpolation(
        boundary == PxOsdOpenSubdivTokens->none     ? Mesh::SUBDIVISION_BOUNDARY_NONE :
        boundary == PxOsdOpenSubdivTokens->edgeOnly ? Mesh::SUBDIVISION_BOUNDARY_EDGE_ONLY :
                                                      Mesh::SUBDIVISION_BOUNDARY_EDGE_AND_CORNER);
    const TfToken fvar = subdivTags.GetFaceVaryingInterpolationRule();
    _geom->set_subdivision_fvar_interpolation(
        fvar == PxOsdOpenSubdivTokens->none         ? Mesh::SUBDIVISION_FVAR_LINEAR_NONE :
        fvar == PxOsdOpenSubdivTokens->cornersOnly  ? Mesh::SUBDIVISION_FVAR_LINEAR_CORNERS_ONLY :
        fvar == PxOsdOpenSubdivTokens->cornersPlus2 ? Mesh::SUBDIVISION_FVAR_LINEAR_CORNERS_PLUS2 :
        fvar == PxOsdOpenSubdivTokens->boundaries   ? Mesh::SUBDIVISION_FVAR_LINEAR_BOUNDARIES :
        fvar == PxOsdOpenSubdivTokens->all          ? Mesh::SUBDIVISION_FVAR_LINEAR_ALL :
                                                      Mesh::SUBDIVISION_FVAR_LINEAR_CORNERS_PLUS1);

    /* Diced in pixels of the dicing camera (the render camera, HdCyclesRenderPass). */
    const HdCyclesSession::DicingSettings &dicing = _session->GetDicing();
    _geom->set_subd_adaptive_space(Mesh::SUBDIVISION_ADAPTIVE_SPACE_PIXEL);
    if (adaptive) {
      // Like Blender, at least 0.1 pixels
      _geom->set_subd_dicing_rate(std::max(
          0.1f, (_session->IsViewport() ? dicing.previewRate : dicing.rate) * dicingScale));
      _geom->set_subd_max_level(dicing.maxLevel);
    }
    else {
      _geom->set_subd_dicing_rate(1.0f);
      _geom->set_subd_max_level(_topology.GetRefineLevel());
    }
    if (!_instances.empty()) {
      _geom->set_subd_objecttoworld(_instances[0]->get_tfm());
    }
  }
}

void HdCyclesMesh::Finalize(PXR_NS::HdRenderParam *renderParam)
{
  _topology = HdMeshTopology();
  _primitiveParams.clear();

  HdCyclesGeometry<PXR_NS::HdMesh, Mesh>::Finalize(renderParam);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
