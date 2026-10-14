/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/implicit_mesh.h"

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

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

/* Segments around the axis: the silhouette deviates from the circle by 0.12% of the radius. */
constexpr size_t kNumRadial = 64;
/* Segments from pole to pole of a sphere, and per cap of a capsule. */
constexpr size_t kNumAxial = 32;
constexpr size_t kNumCapAxial = 16;

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

HdContainerDataSourceHandle BuildPrimvar(const VtVec3fArray &values, const TfToken &role)
{
  return HdPrimvarSchema::Builder()
      .SetPrimvarValue(HdRetainedTypedSampledDataSource<VtVec3fArray>::New(values))
      .SetInterpolation(
          HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->vertex))
      .SetRole(HdPrimvarSchema::BuildRoleDataSource(role))
      .Build();
}

/* Mesh topology, points and normals of an implicit shape. */
HdContainerDataSourceHandle BuildMesh(const TfToken &primType,
                                      const HdContainerDataSourceHandle &primDs)
{
  PxOsdMeshTopology topology;
  VtVec3fArray points;
  VtVec3fArray normals;

  if (primType == HdPrimTypeTokens->sphere) {
    const double radius = GetDouble(HdSphereSchema::GetFromParent(primDs).GetRadius(), 1.0);
    topology = GeomUtilSphereMeshGenerator::GenerateTopology(kNumRadial, kNumAxial);
    points.resize(GeomUtilSphereMeshGenerator::ComputeNumPoints(kNumRadial, kNumAxial));
    normals.resize(GeomUtilSphereMeshGenerator::ComputeNumNormals(kNumRadial, kNumAxial));
    GeomUtilSphereMeshGenerator::GeneratePoints(points.begin(), kNumRadial, kNumAxial, radius);
    GeomUtilSphereMeshGenerator::GenerateNormals(normals.begin(), kNumRadial, kNumAxial);
  }
  else if (primType == HdPrimTypeTokens->cylinder) {
    const HdCylinderSchema schema = HdCylinderSchema::GetFromParent(primDs);
    const double radius = GetDouble(schema.GetRadius(), 1.0);
    const double bottom = GetDouble(schema.GetRadiusBottom(), radius);
    const double top = GetDouble(schema.GetRadiusTop(), radius);
    const double height = GetDouble(schema.GetHeight(), 2.0);
    const GfMatrix4d basis = AxisBasis(schema.GetAxis());
    topology = GeomUtilCylinderMeshGenerator::GenerateTopology(kNumRadial);
    points.resize(GeomUtilCylinderMeshGenerator::ComputeNumPoints(kNumRadial));
    normals.resize(GeomUtilCylinderMeshGenerator::ComputeNumNormals(kNumRadial));
    GeomUtilCylinderMeshGenerator::GeneratePoints(
        points.begin(), kNumRadial, bottom, top, height, &basis);
    GeomUtilCylinderMeshGenerator::GenerateNormals(
        normals.begin(), kNumRadial, bottom, top, height, &basis);
  }
  else if (primType == HdPrimTypeTokens->cone) {
    const HdConeSchema schema = HdConeSchema::GetFromParent(primDs);
    const double radius = GetDouble(schema.GetRadius(), 1.0);
    const double height = GetDouble(schema.GetHeight(), 2.0);
    const GfMatrix4d basis = AxisBasis(schema.GetAxis());
    topology = GeomUtilConeMeshGenerator::GenerateTopology(kNumRadial);
    points.resize(GeomUtilConeMeshGenerator::ComputeNumPoints(kNumRadial));
    normals.resize(GeomUtilConeMeshGenerator::ComputeNumNormals(kNumRadial));
    GeomUtilConeMeshGenerator::GeneratePoints(points.begin(), kNumRadial, radius, height, &basis);
    GeomUtilConeMeshGenerator::GenerateNormals(
        normals.begin(), kNumRadial, radius, height, &basis);
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
    for (size_t i = 0; i < indices.size(); ++i) {
      indices[i] = int(i);
      points[i] = corners[cornerIndices[i]];
      /* One normal per face, every face is a quad. */
      normals[i] = faceNormals[i / 4];
    }
    topology = PxOsdMeshTopology(
        cube.GetScheme(), cube.GetOrientation(), cube.GetFaceVertexCounts(), indices);
  }
  else if (primType == HdPrimTypeTokens->plane) {
    const HdPlaneSchema schema = HdPlaneSchema::GetFromParent(primDs);
    const double width = GetDouble(schema.GetWidth(), 2.0);
    const double length = GetDouble(schema.GetLength(), 2.0);
    const GfMatrix4d basis = AxisBasis(schema.GetAxis());
    topology = GeomUtilPlaneMeshGenerator::GenerateTopology();
    points.resize(GeomUtilPlaneMeshGenerator::ComputeNumPoints());
    GeomUtilPlaneMeshGenerator::GeneratePoints(points.begin(), width, length, &basis);
    /* Per point like the other shapes, instead of the generator's single normal. */
    normals.assign(points.size(), GfVec3f(basis.TransformDir(GfVec3d::ZAxis())));
  }
  else {
    const HdCapsuleSchema schema = HdCapsuleSchema::GetFromParent(primDs);
    const double radius = GetDouble(schema.GetRadius(), 0.5);
    const double bottom = GetDouble(schema.GetRadiusBottom(), radius);
    const double top = GetDouble(schema.GetRadiusTop(), radius);
    const double height = GetDouble(schema.GetHeight(), 1.0);
    const GfMatrix4d basis = AxisBasis(schema.GetAxis());
    topology = GeomUtilCapsuleMeshGenerator::GenerateTopology(kNumRadial, kNumCapAxial);
    points.resize(GeomUtilCapsuleMeshGenerator::ComputeNumPoints(kNumRadial, kNumCapAxial));
    normals.resize(GeomUtilCapsuleMeshGenerator::ComputeNumNormals(kNumRadial, kNumCapAxial));
    GeomUtilCapsuleMeshGenerator::GeneratePoints(
        points.begin(), kNumRadial, kNumCapAxial, bottom, top, height, &basis);
    GeomUtilCapsuleMeshGenerator::GenerateNormals(
        normals.begin(), kNumRadial, kNumCapAxial, bottom, top, height, &basis);
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

  const HdContainerDataSourceHandle primvarsDs = HdRetainedContainerDataSource::New(
      HdPrimvarsSchemaTokens->points,
      BuildPrimvar(points, HdPrimvarSchemaTokens->point),
      HdPrimvarsSchemaTokens->normals,
      BuildPrimvar(normals, HdPrimvarSchemaTokens->normal));

  return HdRetainedContainerDataSource::New(
      HdMeshSchema::GetSchemaToken(),
      meshDs,
      HdPrimvarsSchema::GetSchemaToken(),
      HdOverlayContainerDataSource::New(primvarsDs,
                                        HdPrimvarsSchema::GetFromParent(primDs).GetContainer()));
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
    }
  }
  _SendPrimsDirtied(dirtied);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
