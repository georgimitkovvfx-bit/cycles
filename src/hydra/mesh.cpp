/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/mesh.h"
#include "hydra/geometry.inl"
#include "hydra/util.h"
#include "scene/mesh.h"

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
  if (HdChangeTracker::IsTopologyDirty(dirtyBits, GetId())) {
    PopulateTopology(sceneDelegate);
  }

  if (dirtyBits & HdChangeTracker::DirtyPoints) {
    PopulatePoints(sceneDelegate);
  }

  // Must happen after topology update, so that normals attribute size can be calculated
  if (dirtyBits & HdChangeTracker::DirtyNormals) {
    PopulateNormals(sceneDelegate);
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

  VtVec3fArray points = value.UncheckedGet<VtVec3fArray>();
  const size_t numPoints = _topology.GetNumPoints();

  TF_VERIFY(points.size() >= numPoints);

  // Motion blur: positions at the motion steps, from time samples of the points, or else their
  // velocities (per second). Rendered at the center step.
  const std::vector<float> motionTimes = _session->GetMotionTimes();
  std::vector<VtVec3fArray> motionPoints;
  if (!motionTimes.empty() && points.size() >= numPoints) {
    const HdSampledDataSourceHandle pointsDs =
        primvars.GetPrimvar(HdTokens->points).GetPrimvarValue();
    std::vector<float> sampleTimes;
    if (pointsDs && pointsDs->GetContributingSampleTimesForInterval(
                        motionTimes.front(), motionTimes.back(), &sampleTimes))
    {
      for (const float time : motionTimes) {
        const VtValue stepValue = pointsDs->GetValue(time);
        if (!stepValue.IsHolding<VtVec3fArray>() ||
            stepValue.UncheckedGet<VtVec3fArray>().size() != points.size())
        {
          motionPoints.clear();  // Changing topology
          break;
        }
        motionPoints.push_back(stepValue.UncheckedGet<VtVec3fArray>());
      }
    }
    else {
      const VtValue velocitiesValue = ReadPrimvar(primvars, HdTokens->velocities);
      if (velocitiesValue.IsHolding<VtVec3fArray>() &&
          velocitiesValue.UncheckedGet<VtVec3fArray>().size() == points.size())
      {
        const VtVec3fArray &velocities = velocitiesValue.UncheckedGet<VtVec3fArray>();
        for (const float time : motionTimes) {
          const float seconds = float(time / _session->GetFramesPerSecond());
          VtVec3fArray stepPoints(points.size());
          for (size_t i = 0; i < points.size(); ++i) {
            stepPoints[i] = points[i] + velocities[i] * seconds;
          }
          motionPoints.push_back(std::move(stepPoints));
        }
      }
    }
  }
  if (!motionPoints.empty()) {
    points = motionPoints[motionTimes.size() / 2];
  }

  const bool subdivision = _geom->get_subdivision_type() != Mesh::SUBDIVISION_NONE;
  if (subdivision) {
    /* Drop the previous tessellation, like the topology update: Mesh::tessellate takes every
     * vertex of the mesh as a base vertex, and reads the positions past their end otherwise. */
    _geom->resize_mesh(int(numPoints), 0);
  }
  AttributeSet &attributes = (subdivision) ? _geom->subd_attributes : _geom->attributes;
  Attribute *attr_P = attributes.add(ATTR_STD_POSITION);

  _geom->set_use_motion_blur(!motionPoints.empty());
  _geom->set_motion_steps(motionPoints.empty() ? 0 : uint(motionPoints.size()));
  if (motionPoints.empty()) {
    attr_P->remove_motion();
  }
  else {
    attr_P->add_motion(_geom);
  }

  packed_float3 *verts = attr_P->data_for_write<packed_float3>();
  std::copy_n(reinterpret_cast<const packed_float3 *>(points.data()), numPoints, verts);

  for (int step = 0; step < int(motionPoints.size()); ++step) {
    const int attrStep = attr_P->time_step_to_attr_step(step, int(motionPoints.size()));
    if (attrStep != 0) {
      std::copy_n(reinterpret_cast<const packed_float3 *>(motionPoints[step].data()),
                  numPoints,
                  attr_P->data_for_write<packed_float3>(attrStep));
    }
  }
  _geom->tag_position_modified();
}

void HdCyclesMesh::PopulateNormals(HdSceneDelegate *sceneDelegate)
{
  _geom->attributes.remove(ATTR_STD_VERTEX_NORMAL);
  _geom->attributes.remove(ATTR_STD_CORNER_NORMAL);

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

  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
  const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  const TfToken uvName = FindUVPrimvar(primvars);

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
      const std::vector<ustring> names = RequestedPrimvarNames(
          _geom, scene, primvars, primvarName, std);
      if (names.empty()) {
        continue;
      }

      HdInterpolation primvarInterpolation = interpolation.first;
      AttributeElement elem = interpolation.second;
      if (std == ATTR_STD_UV) {
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

      const ustring ownName(primvarName.GetString());
      for (const ustring &name : names) {
        ApplyPrimvars(attributes, name, value, elem, name == ownName ? std : ATTR_STD_NONE);
      }
    }
  }
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

  _topology = HdMeshTopology(
      scheme, orientation, faceVertexCounts, faceVertexIndices, holeIndices, refineLevel);

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
    const HdSubdivisionTagsSchema subdivSchema = HdSubdivisionTagsSchema::GetFromParent(
        prim.dataSource);
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

          _geom->add_edge_crease(v0, v1, weight);
        }

        indexOffset += creaseLength;
        creaseLengthOffset++;
      }

      const VtIntArray cornerIndices = subdivTags.GetCornerIndices();
      const VtFloatArray cornerWeights = subdivTags.GetCornerWeights();

      for (size_t i = 0; i < cornerIndices.size(); ++i) {
        _geom->add_vertex_crease(cornerIndices[i], cornerWeights[i]);
      }
    }

    _geom->set_subd_dicing_rate(1.0f);
    _geom->set_subd_max_level(_topology.GetRefineLevel());
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
