/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/bake.h"
#include "scene/attribute.h"
#include "scene/bake.h"
#include "scene/geometry.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "util/tbb.h"

#ifdef WITH_EMBREE
#  include <embree4/rtcore.h>
#endif

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <set>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

const char *const kBakePrimitivePass = "BakePrimitive";
const char *const kBakeDifferentialPass = "BakeDifferential";
const char *const kBakeObjectPass = "BakeObject";

namespace {

/* The instanceId of an object, its first attribute (HdCyclesGeometry::InitializeInstance): -1
 * for a prim without instancer, which counts as instance 0. */
int InstanceIndex(const Object *object)
{
  if (object->attributes.empty() || object->attributes.front().type() != TypeFloat) {
    return 0;
  }
  return std::max(0, int(object->attributes.front().get<float>()));
}

Object *FindBakeObject(Scene *scene, const std::string &name, const int instance)
{
  if (name.empty()) {
    return nullptr;
  }
  /* By instanceId: the instances of a prim share its name, and scene->objects is not in
   * instance order (objects are erased by swap). */
  for (Object *object : scene->objects) {
    if (object->name == name && object->get_geometry() && object->get_geometry()->is_mesh() &&
        InstanceIndex(object) == instance)
    {
      return object;
    }
  }
  return nullptr;
}

/* The primitive index as the bits of a float, like the kernel reads it (__float_as_uint). */
float AsFloat(const uint bits)
{
  return __uint_as_float(bits);
}

#ifdef WITH_EMBREE
/* For the rays of Selected to Active, one for the process (Cycles' own is the CPU device's). */
RTCDevice BakeRayDevice()
{
  static const RTCDevice device = rtcNewDevice(nullptr);
  return device;
}
#endif

/* Corner positions of a mesh in world space. */
std::vector<float3> WorldCorners(const Object *object)
{
  const Mesh *const mesh = static_cast<const Mesh *>(object->get_geometry());
  const Attribute *const attrP = mesh->attributes.find(ATTR_STD_POSITION);
  if (!attrP) {
    return {};
  }
  const packed_float3 *const P = attrP->data<packed_float3>();
  const array<int> &triangles = mesh->get_triangles();
  const Transform tfm = object->get_tfm();
  std::vector<float3> corners(triangles.size());
  for (size_t c = 0; c < triangles.size(); ++c) {
    const float3 p = make_float3(P[triangles[c]]);
    corners[c] = mesh->transform_applied ? p : transform_point(&tfm, p);
  }
  return corners;
}

}  // namespace

struct BakeSources {
#ifdef WITH_EMBREE
  RTCScene scene = nullptr;
#endif
  /* Device index of the object of each geometry of the scene. */
  std::vector<int> objects;
  /* Cage corners in world space, empty without a cage. */
  std::vector<float3> cage;
  /* Smooth shading of the target's triangles, which then cast along the vertex normals. */
  std::vector<unsigned char> smooth;
  Transform inverseTfm = transform_identity();
  float extrusion = 0.0f;
  float maxDistance = 0.0f;
  /* What it was built from: the same again reuses it. */
  BakeSettings settings;
  std::vector<std::string> prims;
  unsigned int version = 0;
  const Object *target = nullptr;

  BakeSources() = default;
  BakeSources(const BakeSources &) = delete;
  BakeSources &operator=(const BakeSources &) = delete;
  ~BakeSources()
  {
#ifdef WITH_EMBREE
    if (scene) {
      rtcReleaseScene(scene);
    }
#endif
  }
};

namespace {

/* Selected to Active: the source meshes (but the target) in world space in a BVH. Built again
 * only when the settings, the source prims or the scene state version changed, not for each
 * snapshot (each render start and tile). */
std::shared_ptr<const BakeSources> SnapshotBakeSources(
    Scene *scene,
    const BakeSettings &settings,
    const std::vector<std::string> &prims,
    const unsigned int version,
    const Object *target,
    const std::shared_ptr<const BakeSources> &previous)
{
  if (previous && previous->version == version && previous->target == target &&
      previous->settings == settings && previous->prims == prims)
  {
    return previous;
  }
  auto sources = std::make_shared<BakeSources>();
  sources->settings = settings;
  sources->prims = prims;
  sources->version = version;
  sources->target = target;
  const Mesh *const targetMesh = static_cast<const Mesh *>(target->get_geometry());
  sources->inverseTfm = transform_inverse(target->get_tfm());
  sources->extrusion = settings.cageExtrusion;
  sources->maxDistance = settings.maxRayDistance;
  const array<bool> &smooth = targetMesh->get_smooth();
  sources->smooth.resize(targetMesh->num_triangles(), 1);
  for (size_t t = 0; t < sources->smooth.size() && t < smooth.size(); ++t) {
    sources->smooth[t] = smooth[t];
  }
  /* Blender's cage: the same triangles as the target, other positions. */
  if (const Object *cage = FindBakeObject(scene, settings.cage, 0)) {
    if (static_cast<const Mesh *>(cage->get_geometry())->num_triangles() ==
        targetMesh->num_triangles())
    {
      sources->cage = WorldCorners(cage);
    }
  }

#ifdef WITH_EMBREE
  RTCDevice device = BakeRayDevice();
  if (!device) {
    return sources;
  }
  const std::set<std::string> names(prims.begin(), prims.end());
  sources->scene = rtcNewScene(device);
  for (const Object *object : scene->objects) {
    if (object == target || !object->get_geometry() || !object->get_geometry()->is_mesh() ||
        !names.count(object->name.string()))
    {
      continue;
    }
    const std::vector<float3> corners = WorldCorners(object);
    if (corners.empty()) {
      continue;
    }
    /* Corners, not shared vertices: as many as the target snapshot keeps. */
    RTCGeometry geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
    float *const vertices = static_cast<float *>(rtcSetNewGeometryBuffer(
        geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(float) * 3, corners.size()));
    unsigned *const indices = static_cast<unsigned *>(
        rtcSetNewGeometryBuffer(geometry,
                                RTC_BUFFER_TYPE_INDEX,
                                0,
                                RTC_FORMAT_UINT3,
                                sizeof(unsigned) * 3,
                                corners.size() / 3));
    for (size_t c = 0; c < corners.size(); ++c) {
      vertices[c * 3 + 0] = corners[c].x;
      vertices[c * 3 + 1] = corners[c].y;
      vertices[c * 3 + 2] = corners[c].z;
      indices[c] = unsigned(c);
    }
    rtcCommitGeometry(geometry);
    rtcAttachGeometryByID(sources->scene, geometry, unsigned(sources->objects.size()));
    rtcReleaseGeometry(geometry);
    sources->objects.push_back(object->get_device_index());
  }
  rtcCommitScene(sources->scene);
#else
  (void)prims;
#endif
  return sources;
}

/* Selected to Active for one pixel of the target: the ray from the cage (or the extruded target)
 * through the target's point, like Blender's. Replaces the target triangle and barycentrics of
 * the pixel by those of the source surface it hits, false when it hits none. */
bool CastBakeRay(const BakeMesh &mesh, float *primitive, float *object)
{
#ifdef WITH_EMBREE
  const BakeSources &sources = *mesh.sources;
  if (!sources.scene) {
    /* No Embree device: no ray hits a source. */
    return false;
  }
  const size_t c = size_t(__float_as_uint(primitive[2])) * 3;
  const float w[3] = {primitive[0], primitive[1], 1.0f - primitive[0] - primitive[1]};
  float3 P = w[0] * mesh.P[c] + w[1] * mesh.P[c + 1] + w[2] * mesh.P[c + 2];
  float3 N = (!mesh.N.empty() && sources.smooth[c / 3]) ?
                 w[0] * mesh.N[c] + w[1] * mesh.N[c + 1] + w[2] * mesh.N[c + 2] :
                 cross(mesh.P[c + 1] - mesh.P[c], mesh.P[c + 2] - mesh.P[c]);
  if (!mesh.transformApplied) {
    P = transform_point(&mesh.tfm, P);
    N = transform_direction_transposed(&sources.inverseTfm, N);
  }
  N = safe_normalize(N);
  /* shortcut: the extrusion is in world units (Blender's in object units), the same without
   * object scale. */
  float3 origin = P + N * sources.extrusion;
  float3 direction = -N;
  if (!sources.cage.empty()) {
    origin = w[0] * sources.cage[c] + w[1] * sources.cage[c + 1] + w[2] * sources.cage[c + 2];
    const float3 toTarget = P - origin;
    if (len_squared(toTarget) > 0.0f) {
      direction = normalize(toTarget);
    }
  }

  RTCRayHit rayHit;
  rayHit.ray.org_x = origin.x;
  rayHit.ray.org_y = origin.y;
  rayHit.ray.org_z = origin.z;
  rayHit.ray.dir_x = direction.x;
  rayHit.ray.dir_y = direction.y;
  rayHit.ray.dir_z = direction.z;
  rayHit.ray.tnear = 0.0f;
  rayHit.ray.tfar = sources.maxDistance > 0.0f ? sources.maxDistance : FLT_MAX;
  rayHit.ray.time = 0.0f;
  rayHit.ray.mask = ~0u;
  rayHit.ray.id = 0;
  rayHit.ray.flags = 0;
  rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
  rayHit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
  rtcIntersect1(sources.scene, &rayHit);
  if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
    return false;
  }
  /* Embree's u, v weigh corners 1 and 2, Blender's bake barycentrics corners 0 and 1. */
  primitive[0] = 1.0f - rayHit.hit.u - rayHit.hit.v;
  primitive[1] = rayHit.hit.u;
  primitive[2] = AsFloat(rayHit.hit.primID);
  *object = AsFloat(uint(sources.objects[rayHit.hit.geomID]));
  return true;
#else
  (void)mesh;
  (void)primitive;
  (void)object;
  return false;
#endif
}

/* The target triangle and barycentrics of each pixel of the tile, and their derivatives. */
void RasterizeBake(const BakeMesh &mesh,
                   const OutputDriver::Tile &tile,
                   std::vector<float> &primitive,
                   std::vector<float> &differential);

}  // namespace

BakeMesh SnapshotBakeMesh(Scene *scene,
                          const BakeSettings &settings,
                          const std::vector<std::string> &sourcePrims,
                          const unsigned int sourcesVersion,
                          const std::shared_ptr<const BakeSources> &previousSources)
{
  BakeMesh bake;
  Object *const object = FindBakeObject(scene, settings.object, settings.instance);
  if (!object) {
    return bake;
  }
  const Mesh *const mesh = static_cast<const Mesh *>(object->get_geometry());
  const Attribute *const attrP = mesh->attributes.find(ATTR_STD_POSITION);
  /* The bake UV map, which the mesh exports for the target (HdCyclesMesh::PopulatePrimvars). */
  const Attribute *const attrUV = settings.uvMap.empty() ?
                                      mesh->attributes.find(ATTR_STD_UV) :
                                      mesh->attributes.find(ustring(settings.uvMap));
  const size_t num = mesh->num_triangles();
  if (!attrP || !attrUV || num == 0 || attrUV->type != TypeFloat2 ||
      (attrUV->element != ATTR_ELEMENT_CORNER && attrUV->element != ATTR_ELEMENT_VERTEX))
  {
    return bake;
  }
  const Attribute *const attrN = mesh->attributes.find(ATTR_STD_VERTEX_NORMAL);
  /* MikkTSpace tangents, which Cycles computes while baking (Scene::need_global_attribute), of
   * the default UV map.
   * shortcut: other UV maps get per triangle tangents; request "<map>.tangent" if their normal
   * maps must match Blender's across seams. */
  const Attribute *const attrT = mesh->attributes.find(ATTR_STD_UV_TANGENT);
  const Attribute *const attrS = mesh->attributes.find(ATTR_STD_UV_TANGENT_SIGN);
  const bool tangents = attrUV->std == ATTR_STD_UV && attrT && attrS &&
                        attrT->element == ATTR_ELEMENT_CORNER &&
                        attrS->element == ATTR_ELEMENT_CORNER;

  const array<int> &triangles = mesh->get_triangles();
  const packed_float3 *const P = attrP->data<packed_float3>();
  const packed_normal *const N = attrN ? attrN->data<packed_normal>() : nullptr;
  const float2 *const uvs = attrUV->data<float2>();
  const bool cornerUV = attrUV->element == ATTR_ELEMENT_CORNER;

  bake.object = object;
  bake.objectIndex = object->get_device_index();
  bake.deviceObjects = scene->dscene.objects.size();
  bake.deviceTriangles = scene->dscene.tri_vindex.size();
  bake.numTriangles = num;
  bake.tfm = object->get_tfm();
  bake.transformApplied = mesh->transform_applied;
  bake.uv.resize(num * 3);
  bake.P.resize(num * 3);
  if (N) {
    bake.N.resize(num * 3);
  }
  if (tangents) {
    bake.tangent.resize(num * 3);
    bake.sign.resize(num * 3);
  }
  for (size_t c = 0; c < num * 3; ++c) {
    const int v = triangles[c];
    bake.uv[c] = cornerUV ? uvs[c] : uvs[v];
    bake.P[c] = make_float3(P[v]);
    if (N) {
      bake.N[c] = N[v].decode();
    }
    if (tangents) {
      bake.tangent[c] = make_float3(attrT->data<packed_float3>()[c]);
      bake.sign[c] = attrS->data<float>()[c];
    }
  }
  if (settings.useSources()) {
    bake.sources = SnapshotBakeSources(
        scene, settings, sourcePrims, sourcesVersion, object, previousSources);
  }
  return bake;
}

bool BakeMeshCurrent(const BakeMesh &mesh, const Scene *scene)
{
  return mesh.deviceObjects == scene->dscene.objects.size() &&
         mesh.deviceTriangles == scene->dscene.tri_vindex.size();
}

std::vector<unsigned char> BakeCoverage(const OutputDriver::Tile &tile)
{
  const size_t num = size_t(tile.size.x) * tile.size.y;
  std::vector<float> primitive(num * 3);
  if (!tile.get_pass_pixels(kBakePrimitivePass, 3, primitive.data())) {
    return {};
  }
  std::vector<unsigned char> coverage(num);
  for (size_t i = 0; i < num; ++i) {
    coverage[i] = __float_as_uint(primitive[i * 3 + 2]) != ~0u;
  }
  return coverage;
}

void ExtendBakeMargin(float *pixels,
                      const int width,
                      const int height,
                      const int channels,
                      std::vector<unsigned char> coverage,
                      const int margin)
{
  if (coverage.size() != size_t(width) * height) {
    return;
  }
  std::vector<size_t> ring;
  for (int step = 0; step < margin; ++step) {
    ring.clear();
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const size_t pixel = size_t(y) * width + x;
        if (coverage[pixel]) {
          continue;
        }
        int count = 0;
        float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            const int nx = x + dx, ny = y + dy;
            if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= width || ny >= height) {
              continue;
            }
            const size_t neighbor = size_t(ny) * width + nx;
            if (coverage[neighbor] != 1) {
              continue;
            }
            for (int c = 0; c < channels && c < 4; ++c) {
              sum[c] += pixels[neighbor * channels + c];
            }
            count++;
          }
        }
        if (count > 0) {
          for (int c = 0; c < channels && c < 4; ++c) {
            pixels[pixel * channels + c] = sum[c] / float(count);
          }
          ring.push_back(pixel);
        }
      }
    }
    if (ring.empty()) {
      break;
    }
    /* Marked after the step, so each ring only reads the pixels of the steps before it. */
    for (const size_t pixel : ring) {
      coverage[pixel] = 1;
    }
  }
}

void ConvertBakeNormals(const BakeMesh &mesh,
                        const OutputDriver::Tile &tile,
                        float *pixels,
                        const int channels,
                        const BakeNormalSpace space)
{
  if (space == BakeNormalSpace::World || !mesh.valid() || channels < 3) {
    return;
  }
  const size_t num = size_t(tile.size.x) * tile.size.y;
  std::vector<float> primitive(num * 3);
  if (!tile.get_pass_pixels(kBakePrimitivePass, 3, primitive.data())) {
    return;
  }
  /* In the target's frame also with Selected to Active, whose pass holds the source triangles:
   * the pixels the pass covers, at the target triangles of its UV map. */
  std::vector<float> target;
  if (mesh.sources) {
    std::vector<float> differential;
    RasterizeBake(mesh, tile, target, differential);
    for (size_t i = 0; i < num; ++i) {
      if (__float_as_uint(primitive[i * 3 + 2]) == ~0u) {
        target[i * 3 + 2] = AsFloat(~0u);
      }
    }
    primitive.swap(target);
  }

  for (size_t i = 0; i < num; ++i) {
    const uint prim = __float_as_uint(primitive[i * 3 + 2]);
    if (prim == ~0u || prim >= mesh.numTriangles) {
      continue;
    }
    const float w[3] = {primitive[i * 3 + 0],
                        primitive[i * 3 + 1],
                        1.0f - primitive[i * 3 + 0] - primitive[i * 3 + 1]};
    const size_t c = size_t(prim) * 3;
    float3 n = make_float3(
        pixels[i * channels + 0], pixels[i * channels + 1], pixels[i * channels + 2]);
    /* World to mesh (and object) space: transposed object transform, for normals. The mesh is
     * in world space when Cycles applied the object transform to it. */
    if (!mesh.transformApplied || space == BakeNormalSpace::Object) {
      n = transform_direction_transposed(&mesh.tfm, n);
    }
    n = safe_normalize(n);
    float3 result = n;
    if (space == BakeNormalSpace::Tangent) {
      const float3 &p0 = mesh.P[c], &p1 = mesh.P[c + 1], &p2 = mesh.P[c + 2];
      const float3 ns = mesh.N.empty() ? safe_normalize(cross(p1 - p0, p2 - p0)) :
                                         safe_normalize(w[0] * mesh.N[c] + w[1] * mesh.N[c + 1] +
                                                        w[2] * mesh.N[c + 2]);
      float3 tangent;
      float sign = 1.0f;
      if (!mesh.tangent.empty()) {
        tangent = w[0] * mesh.tangent[c] + w[1] * mesh.tangent[c + 1] + w[2] * mesh.tangent[c + 2];
        sign = (w[0] * mesh.sign[c] + w[1] * mesh.sign[c + 1] + w[2] * mesh.sign[c + 2]) < 0.0f ?
                   -1.0f :
                   1.0f;
      }
      else {
        /* Per triangle tangent from the UVs. */
        const float2 d1 = mesh.uv[c + 1] - mesh.uv[c], d2 = mesh.uv[c + 2] - mesh.uv[c];
        const float det = d1.x * d2.y - d1.y * d2.x;
        const float3 dp1 = p1 - p0, dp2 = p2 - p0;
        if (det != 0.0f) {
          tangent = (dp1 * d2.y - dp2 * d1.y) / det;
          sign = dot(cross(ns, tangent), (dp2 * d1.x - dp1 * d2.x) / det) < 0.0f ? -1.0f : 1.0f;
        }
        else {
          /* Degenerate UVs: any tangent, so the pixel is still in tangent space. */
          float3 bitangent;
          make_orthonormals(ns, &tangent, &bitangent);
        }
      }
      const float3 t = safe_normalize(tangent - ns * dot(ns, tangent));
      const float3 b = cross(ns, t) * sign;
      result = make_float3(dot(n, t), dot(n, b), dot(n, ns)) * 0.5f + make_float3(0.5f);
    }
    pixels[i * channels + 0] = result.x;
    pixels[i * channels + 1] = result.y;
    pixels[i * channels + 2] = result.z;
  }
}

void SetBakeTarget(Scene *scene, const BakeSettings &bake)
{
  Object *const target = FindBakeObject(scene, bake.object, bake.instance);
  bool changed = false;
  for (Object *object : scene->objects) {
    if (object->get_is_bake_target() != (object == target)) {
      object->set_is_bake_target(object == target);
      object->tag_update(scene);
      changed = true;
    }
  }
  scene->bake_manager->set_use_camera(false);
  scene->bake_manager->set_baking(scene, target != nullptr);
  /* The kernel's target object index and triangle offset: again when they may have moved, with
   * object or geometry updates, which reset the render anyway (a bake update alone does too). */
  if (target &&
      (changed || scene->object_manager->need_update() || scene->geometry_manager->need_update()))
  {
    scene->bake_manager->tag_update();
  }
}

namespace {

void RasterizeBake(const BakeMesh &mesh,
                   const OutputDriver::Tile &tile,
                   std::vector<float> &primitive,
                   std::vector<float> &differential)
{
  const int width = tile.size.x;
  const int height = tile.size.y;
  const float fullWidth = float(tile.full_size.x);
  const float fullHeight = float(tile.full_size.y);
  /* Empty pixels have primitive -1 (as bits). Cycles' passes start zeroed, which is triangle 0,
   * so a target without triangles or UVs writes empty passes. */
  primitive.assign(size_t(width) * height * 3, 0.0f);
  differential.assign(size_t(width) * height * 4, 0.0f);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    primitive[i * 3 + 2] = AsFloat(~0u);
  }

  for (size_t t = 0; t < mesh.numTriangles; ++t) {
    /* Triangle corners in pixels of the tile, with the UV map's (0, 0) at the bottom left. */
    float2 p[3];
    for (int k = 0; k < 3; ++k) {
      const float2 st = mesh.uv[t * 3 + k];
      p[k] = make_float2(st.x * fullWidth - float(tile.offset.x),
                         st.y * fullHeight - float(tile.offset.y));
    }
    const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) -
                       (p[1].y - p[0].y) * (p[2].x - p[0].x);
    if (area == 0.0f || !std::isfinite(area)) {
      continue;
    }
    const int x0 = std::max(0, int(std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
    const int x1 = std::min(width - 1, int(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
    const int y0 = std::max(0, int(std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
    const int y1 = std::min(height - 1, int(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
    /* Barycentric weights of corners 0 and 1 (Blender's bake convention) and their
     * derivatives in pixels. */
    const float dw0dx = (p[1].y - p[2].y) / area, dw0dy = (p[2].x - p[1].x) / area;
    const float dw1dx = (p[2].y - p[0].y) / area, dw1dy = (p[0].x - p[2].x) / area;
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        const float px = float(x) + 0.5f, py = float(y) + 0.5f;
        const float w0 = ((p[1].x - px) * (p[2].y - py) - (p[1].y - py) * (p[2].x - px)) / area;
        const float w1 = ((p[2].x - px) * (p[0].y - py) - (p[2].y - py) * (p[0].x - px)) / area;
        const float w2 = 1.0f - w0 - w1;
        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
          continue;
        }
        const size_t pixel = size_t(y) * width + x;
        primitive[pixel * 3 + 0] = w0;
        primitive[pixel * 3 + 1] = w1;
        primitive[pixel * 3 + 2] = AsFloat(uint(t));
        differential[pixel * 4 + 0] = dw0dx;
        differential[pixel * 4 + 1] = dw0dy;
        differential[pixel * 4 + 2] = dw1dx;
        differential[pixel * 4 + 3] = dw1dy;
      }
    }
  }
}

}  // namespace

bool FillBakeTile(const BakeMesh &mesh, const OutputDriver::Tile &tile)
{
  std::vector<float> primitive, differential;
  RasterizeBake(mesh, tile, primitive, differential);

  /* The object of each pixel, when the bake object pass exists (Selected to Active): the target,
   * or the source surface the ray from the target pixel hits. Without differentials (Blender's
   * are zero too), so those pixels are not anti-aliased. */
  const size_t num = size_t(tile.size.x) * tile.size.y;
  std::vector<float> object(num, AsFloat(uint(mesh.objectIndex)));
  if (mesh.sources) {
    parallel_for(size_t(0), num, [&](const size_t i) {
      if (__float_as_uint(primitive[i * 3 + 2]) == ~0u) {
        return;
      }
      std::fill_n(&differential[i * 4], 4, 0.0f);
      if (!CastBakeRay(mesh, &primitive[i * 3], &object[i])) {
        primitive[i * 3 + 2] = AsFloat(~0u);
      }
    });
  }
  if (!tile.set_pass_pixels(kBakeObjectPass, 1, object.data()) && mesh.sources) {
    /* No object pass yet (passes not rebuilt): source triangles would be read as the target's. */
    RasterizeBake(BakeMesh(), tile, primitive, differential);
  }

  return tile.set_pass_pixels(kBakePrimitivePass, 3, primitive.data()) &&
         tile.set_pass_pixels(kBakeDifferentialPass, 4, differential.data());
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
