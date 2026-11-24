/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Ported from hdCycles-h22 (https://projects.blender.org/devanshutak25/hdCycles-h22, branch hdCycles-H22, commit
 * 3e0bcd4, Apache-2.0) as hdcycles-blender patch 0026: outside Blender (Houdini, usdview) nothing else turns USD's
 * implicit shapes into meshes for Cycles. FindUVPrimvar is h22's (util.cpp), copied in here. */

#include "hydra/implicit_mesh.h"
#include "hydra/util.h"
#include "util/math_base.h"

#include <pxr/base/gf/rotation.h>
#include <pxr/imaging/geomUtil/capsuleMeshGenerator.h>
#include <pxr/imaging/geomUtil/coneMeshGenerator.h>
#include <pxr/imaging/geomUtil/cuboidMeshGenerator.h>
#include <pxr/imaging/geomUtil/cylinderMeshGenerator.h>
#include <pxr/imaging/geomUtil/planeMeshGenerator.h>
#include <pxr/imaging/geomUtil/sphereMeshGenerator.h>
#include <pxr/imaging/hd/capsuleSchema.h>
#include <pxr/imaging/hd/coneSchema.h>
#include <pxr/imaging/hd/cubeSchema.h>
#include <pxr/imaging/hd/cylinderSchema.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/planeSchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sphereSchema.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/pxOsd/meshTopology.h>
#include <pxr/imaging/pxOsd/tokens.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

/* Segments around the axis: the silhouette deviates from the circle by 0.12% of the radius. */
constexpr size_t kNumRadial = 64;
/* Segments from pole to pole of a sphere, and per cap of a capsule. */
constexpr size_t kNumAxial = 32;
constexpr size_t kNumCapAxial = 16;

const TfToken &StToken()
{
  static const TfToken st("st");
  return st;
}

/* The texture coordinate primvar: st, uv, else the first with the textureCoordinate role (h22's util.cpp). */
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

bool IsImplicit(const TfToken &primType)
{
  return primType == HdPrimTypeTokens->sphere || primType == HdPrimTypeTokens->cylinder ||
         primType == HdPrimTypeTokens->cone || primType == HdPrimTypeTokens->capsule ||
         primType == HdPrimTypeTokens->cube || primType == HdPrimTypeTokens->plane;
}

double GetDouble(const HdDoubleDataSourceHandle &ds, const double fallback)
{
  return ds ? ds->GetTypedValue(0.0f) : fallback;
}

/* The generators build along Z, USD shapes along their axis attribute (Z by default). */
GfMatrix4d AxisBasis(const HdTokenDataSourceHandle &axisDs)
{
  static const TfToken x("X"), y("Y");
  const TfToken axis = axisDs ? axisDs->GetTypedValue(0.0f) : TfToken();
  GfMatrix4d basis(1.0);
  if (axis == x) {
    basis.SetRotate(GfRotation(GfVec3d::ZAxis(), GfVec3d::XAxis()));
  }
  else if (axis == y) {
    basis.SetRotate(GfRotation(GfVec3d::ZAxis(), GfVec3d::YAxis()));
  }
  return basis;
}

template<class T>
HdContainerDataSourceHandle BuildPrimvar(const T &values,
                                         const TfToken &interpolation,
                                         const TfToken &role)
{
  return HdPrimvarSchema::Builder()
      .SetPrimvarValue(HdRetainedTypedSampledDataSource<T>::New(values))
      .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(interpolation))
      .SetRole(HdPrimvarSchema::BuildRoleDataSource(role))
      .Build();
}

/* Texture coordinates per face corner of a shape around the Z axis, laid out like Blender's UV
 * sphere: s goes around from the seam at -X (0.5 at +X), t goes up, by latitude on the sphere and
 * by height on the other shapes. Cap faces get a planar projection. Computed from the positions
 * and normals, GeomUtil does not promise an order of the points. */
VtVec2fArray RevolvedSt(const PxOsdMeshTopology &topology,
                        const VtVec3fArray &points,
                        const VtVec3fArray &normals,
                        const bool latitude,
                        const float bottomRadius,
                        const float topRadius)
{
  float zMin = FLT_MAX;
  float zMax = -FLT_MAX;
  for (const GfVec3f &p : points) {
    zMin = std::min(zMin, p[2]);
    zMax = std::max(zMax, p[2]);
  }

  const auto onAxis = [](const GfVec3f &p) {
    return p[0] * p[0] + p[1] * p[1] < 1e-8f * p.GetLengthSq();
  };

  const VtIntArray &indices = topology.GetFaceVertexIndices();
  VtVec2fArray st(indices.size());
  size_t first = 0;
  for (const int count : topology.GetFaceVertexCounts()) {
    const size_t end = first + count;
    bool cap = true;
    for (size_t c = first; c < end; ++c) {
      cap = cap && std::abs(normals[indices[c]][2]) > 0.999f;
    }
    if (cap) {
      for (size_t c = first; c < end; ++c) {
        const GfVec3f &p = points[indices[c]];
        const bool top = normals[indices[c]][2] > 0.0f;
        /* Not zero on the point-sized top cap of a cylinder with radiusTop 0. */
        const float diameter = std::max(2.0f * (top ? topRadius : bottomRadius), FLT_MIN);
        /* Mirrored on the bottom cap, so the texture reads the right way round from outside. */
        st[c] = GfVec2f(0.5f + (top ? p[0] : -p[0]) / diameter, 0.5f + p[1] / diameter);
      }
      first = end;
      continue;
    }

    float sMin = FLT_MAX;
    float sMax = -FLT_MAX;
    for (size_t c = first; c < end; ++c) {
      const GfVec3f &p = points[indices[c]];
      st[c][1] = latitude ?
                     0.5f + std::asin(std::clamp(normals[indices[c]][2], -1.0f, 1.0f)) * M_1_PI_F :
                     (p[2] - zMin) / (zMax - zMin);
      if (!onAxis(p)) {
        st[c][0] = 0.5f + std::atan2(p[1], p[0]) * M_1_2PI_F;
        sMin = std::min(sMin, st[c][0]);
        sMax = std::max(sMax, st[c][0]);
      }
    }
    /* A face across the seam continues past s = 1 instead of spanning the whole texture. Corners
     * on the axis (poles, the cone apex) take the mean s of the other corners, like the poles of
     * Blender's UV sphere. */
    float sSum = 0.0f;
    int numAround = 0;
    for (size_t c = first; c < end; ++c) {
      if (!onAxis(points[indices[c]])) {
        if (sMax - sMin > 0.5f && st[c][0] < 0.5f) {
          st[c][0] += 1.0f;
        }
        sSum += st[c][0];
        numAround++;
      }
    }
    for (size_t c = first; c < end; ++c) {
      if (onAxis(points[indices[c]])) {
        st[c][0] = sSum / float(std::max(numAround, 1));
      }
    }
    first = end;
  }
  return st;
}

/* Blender's cube layout: -X, +Y, +X and -Y from bottom to top in the middle column, -Z left and
 * +Z right of +X. The point q is on the cube from -1 to 1, n is the face normal. */
GfVec2f CubeSt(const GfVec3f &q, const GfVec3f &n)
{
  if (n[0] < -0.5f) {
    return GfVec2f(0.375f + 0.125f * (q[2] + 1.0f), 0.125f * (q[1] + 1.0f));
  }
  if (n[1] > 0.5f) {
    return GfVec2f(0.375f + 0.125f * (q[2] + 1.0f), 0.25f + 0.125f * (q[0] + 1.0f));
  }
  if (n[0] > 0.5f) {
    return GfVec2f(0.375f + 0.125f * (q[2] + 1.0f), 0.5f + 0.125f * (1.0f - q[1]));
  }
  if (n[1] < -0.5f) {
    return GfVec2f(0.375f + 0.125f * (q[2] + 1.0f), 0.75f + 0.125f * (1.0f - q[0]));
  }
  if (n[2] < -0.5f) {
    return GfVec2f(0.125f + 0.125f * (q[0] + 1.0f), 0.5f + 0.125f * (1.0f - q[1]));
  }
  return GfVec2f(0.625f + 0.125f * (1.0f - q[0]), 0.5f + 0.125f * (1.0f - q[1]));
}

/* Mesh topology, points, normals and texture coordinates of an implicit shape. */
HdContainerDataSourceHandle BuildMesh(const TfToken &primType,
                                      const HdContainerDataSourceHandle &primDs)
{
  PxOsdMeshTopology topology;
  VtVec3fArray points;
  VtVec3fArray normals;
  VtVec2fArray st;
  /* The faces of the curved shapes share points across the seam. */
  TfToken stInterpolation = HdPrimvarSchemaTokens->faceVarying;
  GfMatrix4d basis(1.0);

  if (primType == HdPrimTypeTokens->sphere) {
    const double radius = GetDouble(HdSphereSchema::GetFromParent(primDs).GetRadius(), 1.0);
    topology = GeomUtilSphereMeshGenerator::GenerateTopology(kNumRadial, kNumAxial);
    points.resize(GeomUtilSphereMeshGenerator::ComputeNumPoints(kNumRadial, kNumAxial));
    normals.resize(GeomUtilSphereMeshGenerator::ComputeNumNormals(kNumRadial, kNumAxial));
    GeomUtilSphereMeshGenerator::GeneratePoints(points.begin(), kNumRadial, kNumAxial, radius);
    GeomUtilSphereMeshGenerator::GenerateNormals(normals.begin(), kNumRadial, kNumAxial);
    st = RevolvedSt(topology, points, normals, true, 0.0f, 0.0f);
  }
  else if (primType == HdPrimTypeTokens->cylinder) {
    const HdCylinderSchema schema = HdCylinderSchema::GetFromParent(primDs);
    const double radius = GetDouble(schema.GetRadius(), 1.0);
    const double bottom = GetDouble(schema.GetRadiusBottom(), radius);
    const double top = GetDouble(schema.GetRadiusTop(), radius);
    const double height = GetDouble(schema.GetHeight(), 2.0);
    basis = AxisBasis(schema.GetAxis());
    topology = GeomUtilCylinderMeshGenerator::GenerateTopology(kNumRadial);
    points.resize(GeomUtilCylinderMeshGenerator::ComputeNumPoints(kNumRadial));
    normals.resize(GeomUtilCylinderMeshGenerator::ComputeNumNormals(kNumRadial));
    GeomUtilCylinderMeshGenerator::GeneratePoints(points.begin(), kNumRadial, bottom, top, height);
    GeomUtilCylinderMeshGenerator::GenerateNormals(
        normals.begin(), kNumRadial, bottom, top, height);
    st = RevolvedSt(topology, points, normals, false, bottom, top);
  }
  else if (primType == HdPrimTypeTokens->cone) {
    const HdConeSchema schema = HdConeSchema::GetFromParent(primDs);
    const double radius = GetDouble(schema.GetRadius(), 1.0);
    const double height = GetDouble(schema.GetHeight(), 2.0);
    basis = AxisBasis(schema.GetAxis());
    topology = GeomUtilConeMeshGenerator::GenerateTopology(kNumRadial);
    points.resize(GeomUtilConeMeshGenerator::ComputeNumPoints(kNumRadial));
    normals.resize(GeomUtilConeMeshGenerator::ComputeNumNormals(kNumRadial));
    GeomUtilConeMeshGenerator::GeneratePoints(points.begin(), kNumRadial, radius, height);
    GeomUtilConeMeshGenerator::GenerateNormals(normals.begin(), kNumRadial, radius, height);
    st = RevolvedSt(topology, points, normals, false, radius, radius);
  }
  else if (primType == HdPrimTypeTokens->cube) {
    const double size = GetDouble(HdCubeSchema::GetFromParent(primDs).GetSize(), 2.0);
    const PxOsdMeshTopology cube = GeomUtilCuboidMeshGenerator::GenerateTopology();
    VtVec3fArray corners(GeomUtilCuboidMeshGenerator::ComputeNumPoints());
    VtVec3fArray faceNormals(GeomUtilCuboidMeshGenerator::ComputeNumNormals());
    GeomUtilCuboidMeshGenerator::GeneratePoints(corners.begin(), size, size, size);
    GeomUtilCuboidMeshGenerator::GenerateNormals(faceNormals.begin());
    /* Own points per face: faces sharing the corners would get averaged, smooth normals. */
    const VtIntArray &cornerIndices = cube.GetFaceVertexIndices();
    VtIntArray indices(cornerIndices.size());
    points.resize(indices.size());
    normals.resize(indices.size());
    st.resize(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
      indices[i] = int(i);
      points[i] = corners[cornerIndices[i]];
      /* One normal per face, every face is a quad. */
      normals[i] = faceNormals[i / 4];
      st[i] = CubeSt(points[i] / (0.5f * float(size)), normals[i]);
    }
    stInterpolation = HdPrimvarSchemaTokens->vertex;
    topology = PxOsdMeshTopology(
        cube.GetScheme(), cube.GetOrientation(), cube.GetFaceVertexCounts(), indices);
  }
  else if (primType == HdPrimTypeTokens->plane) {
    const HdPlaneSchema schema = HdPlaneSchema::GetFromParent(primDs);
    const double width = GetDouble(schema.GetWidth(), 2.0);
    const double length = GetDouble(schema.GetLength(), 2.0);
    basis = AxisBasis(schema.GetAxis());
    topology = GeomUtilPlaneMeshGenerator::GenerateTopology();
    points.resize(GeomUtilPlaneMeshGenerator::ComputeNumPoints());
    GeomUtilPlaneMeshGenerator::GeneratePoints(points.begin(), width, length);
    /* Per point like the other shapes, instead of the generator's single normal. */
    normals.assign(points.size(), GfVec3f::ZAxis());
    for (const GfVec3f &p : points) {
      st.push_back(GfVec2f(0.5f + p[0] / float(width), 0.5f + p[1] / float(length)));
    }
    stInterpolation = HdPrimvarSchemaTokens->vertex;
  }
  else {
    const HdCapsuleSchema schema = HdCapsuleSchema::GetFromParent(primDs);
    const double radius = GetDouble(schema.GetRadius(), 0.5);
    const double bottom = GetDouble(schema.GetRadiusBottom(), radius);
    const double top = GetDouble(schema.GetRadiusTop(), radius);
    const double height = GetDouble(schema.GetHeight(), 1.0);
    basis = AxisBasis(schema.GetAxis());
    topology = GeomUtilCapsuleMeshGenerator::GenerateTopology(kNumRadial, kNumCapAxial);
    points.resize(GeomUtilCapsuleMeshGenerator::ComputeNumPoints(kNumRadial, kNumCapAxial));
    normals.resize(GeomUtilCapsuleMeshGenerator::ComputeNumNormals(kNumRadial, kNumCapAxial));
    GeomUtilCapsuleMeshGenerator::GeneratePoints(
        points.begin(), kNumRadial, kNumCapAxial, bottom, top, height);
    GeomUtilCapsuleMeshGenerator::GenerateNormals(
        normals.begin(), kNumRadial, kNumCapAxial, bottom, top, height);
    st = RevolvedSt(topology, points, normals, false, bottom, top);
  }

  /* The texture coordinates are computed along Z, rotate to the axis after that. */
  if (basis != GfMatrix4d(1.0)) {
    for (GfVec3f &p : points) {
      p = GfVec3f(basis.Transform(p));
    }
    for (GfVec3f &n : normals) {
      n = GfVec3f(basis.TransformDir(n));
    }
  }

  const HdContainerDataSourceHandle meshDs =
      HdMeshSchema::Builder()
          .SetTopology(HdMeshTopologySchema::Builder()
                           .SetFaceVertexCounts(HdRetainedTypedSampledDataSource<VtIntArray>::New(
                               topology.GetFaceVertexCounts()))
                           .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(
                               topology.GetFaceVertexIndices()))
                           .SetOrientation(HdMeshTopologySchema::BuildOrientationDataSource(
                               topology.GetOrientation()))
                           .Build())
          /* Already smooth, don't let viewport refinement levels shrink it. */
          .SetSubdivisionScheme(
              HdRetainedTypedSampledDataSource<TfToken>::New(PxOsdOpenSubdivTokens->none))
          .Build();

  const HdPrimvarsSchema inputPrimvars = HdPrimvarsSchema::GetFromParent(primDs);
  const TfToken names[] = {
      HdPrimvarsSchemaTokens->points, HdPrimvarsSchemaTokens->normals, StToken()};
  const HdDataSourceBaseHandle values[] = {
      BuildPrimvar(points, HdPrimvarSchemaTokens->vertex, HdPrimvarSchemaTokens->point),
      BuildPrimvar(normals, HdPrimvarSchemaTokens->vertex, HdPrimvarSchemaTokens->normal),
      BuildPrimvar(st, stInterpolation, HdPrimvarSchemaTokens->textureCoordinate)};
  /* Authored texture coordinates win. Not even an authored st is overlaid: its indices would
   * apply to ours. */
  const size_t numPrimvars = FindUVPrimvar(inputPrimvars).IsEmpty() ? 3 : 2;

  return HdRetainedContainerDataSource::New(
      HdMeshSchema::GetSchemaToken(),
      meshDs,
      HdPrimvarsSchema::GetSchemaToken(),
      HdOverlayContainerDataSource::New(
          HdRetainedContainerDataSource::New(numPrimvars, names, values),
          inputPrimvars.GetContainer()));
}

}  // namespace

HdCyclesImplicitMeshSceneIndexRefPtr HdCyclesImplicitMeshSceneIndex::New(
    const HdSceneIndexBaseRefPtr &inputSceneIndex)
{
  return TfCreateRefPtr(new HdCyclesImplicitMeshSceneIndex(inputSceneIndex));
}

HdCyclesImplicitMeshSceneIndex::HdCyclesImplicitMeshSceneIndex(
    const HdSceneIndexBaseRefPtr &inputSceneIndex)
    : HdSingleInputFilteringSceneIndexBase(inputSceneIndex)
{
}

HdSceneIndexPrim HdCyclesImplicitMeshSceneIndex::GetPrim(const SdfPath &primPath) const
{
  HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
  if (IsImplicit(prim.primType)) {
    prim.dataSource = HdOverlayContainerDataSource::New(BuildMesh(prim.primType, prim.dataSource),
                                                        prim.dataSource);
    prim.primType = HdPrimTypeTokens->mesh;
  }
  return prim;
}

SdfPathVector HdCyclesImplicitMeshSceneIndex::GetChildPrimPaths(const SdfPath &primPath) const
{
  return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
}

void HdCyclesImplicitMeshSceneIndex::_PrimsAdded(
    const HdSceneIndexBase & /*sender*/, const HdSceneIndexObserver::AddedPrimEntries &entries)
{
  HdSceneIndexObserver::AddedPrimEntries added = entries;
  for (HdSceneIndexObserver::AddedPrimEntry &entry : added) {
    if (IsImplicit(entry.primType)) {
      entry.primType = HdPrimTypeTokens->mesh;
    }
  }
  _SendPrimsAdded(added);
}

void HdCyclesImplicitMeshSceneIndex::_PrimsRemoved(
    const HdSceneIndexBase & /*sender*/, const HdSceneIndexObserver::RemovedPrimEntries &entries)
{
  _SendPrimsRemoved(entries);
}

void HdCyclesImplicitMeshSceneIndex::_PrimsDirtied(
    const HdSceneIndexBase & /*sender*/, const HdSceneIndexObserver::DirtiedPrimEntries &entries)
{
  /* A changed shape (radius, height, axis, size) changes the mesh. */
  static const HdDataSourceLocatorSet shapeLocators = {HdSphereSchema::GetDefaultLocator(),
                                                       HdCylinderSchema::GetDefaultLocator(),
                                                       HdConeSchema::GetDefaultLocator(),
                                                       HdCapsuleSchema::GetDefaultLocator(),
                                                       HdCubeSchema::GetDefaultLocator(),
                                                       HdPlaneSchema::GetDefaultLocator()};
  HdSceneIndexObserver::DirtiedPrimEntries dirtied = entries;
  for (HdSceneIndexObserver::DirtiedPrimEntry &entry : dirtied) {
    if (entry.dirtyLocators.Intersects(shapeLocators)) {
      entry.dirtyLocators.insert(HdMeshSchema::GetDefaultLocator());
      entry.dirtyLocators.insert(HdPrimvarsSchema::GetPointsLocator());
      entry.dirtyLocators.insert(HdPrimvarsSchema::GetNormalsLocator());
      entry.dirtyLocators.insert(HdPrimvarsSchema::GetDefaultLocator().Append(StToken()));
    }
  }
  _SendPrimsDirtied(dirtied);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
