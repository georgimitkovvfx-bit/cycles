/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/bake.h"
#include "scene/attribute.h"
#include "scene/bake.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

const char *const kBakePrimitivePass = "BakePrimitive";
const char *const kBakeDifferentialPass = "BakeDifferential";

namespace {

Object *FindBakeObject(Scene *scene, const std::string &name)
{
  if (name.empty()) {
    return nullptr;
  }
  for (Object *object : scene->objects) {
    if (object->name == name && object->get_geometry() && object->get_geometry()->is_mesh()) {
      return object;
    }
  }
  return nullptr;
}

float AsFloat(const uint bits)
{
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

}  // namespace

void SetBakeTarget(Scene *scene, const std::string &name)
{
  Object *const target = FindBakeObject(scene, name);
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
  /* The kernel's target object index (only on changes, an update resets the render). */
  if (changed) {
    scene->bake_manager->tag_update();
  }
}

bool FillBakeTile(Scene *scene, const std::string &name, const OutputDriver::Tile &tile)
{
  const Object *const object = FindBakeObject(scene, name);
  if (!object) {
    return false;
  }
  const Mesh *const mesh = static_cast<const Mesh *>(object->get_geometry());
  const Attribute *const uvAttr = mesh->attributes.find(ATTR_STD_UV);
  if (!uvAttr || mesh->num_triangles() == 0) {
    return false;
  }
  const float2 *const uvs = uvAttr->data<float2>();
  const array<int> &triangles = mesh->get_triangles();
  /* UV of corner k of triangle t: per corner (face-varying) or per vertex. */
  const auto uv = [&](const size_t t, const int k) -> float2 {
    return uvAttr->element == ATTR_ELEMENT_CORNER ? uvs[t * 3 + k] : uvs[triangles[t * 3 + k]];
  };
  if (uvAttr->element != ATTR_ELEMENT_CORNER && uvAttr->element != ATTR_ELEMENT_VERTEX) {
    return false;
  }

  const int width = tile.size.x;
  const int height = tile.size.y;
  const float fullWidth = float(tile.full_size.x);
  const float fullHeight = float(tile.full_size.y);
  /* Empty pixels have primitive -1 (as bits). */
  std::vector<float> primitive(size_t(width) * height * 3, 0.0f);
  std::vector<float> differential(size_t(width) * height * 4, 0.0f);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    primitive[i * 3 + 2] = AsFloat(~0u);
  }

  for (size_t t = 0; t < mesh->num_triangles(); ++t) {
    /* Triangle corners in pixels of the tile, with the UV map's (0, 0) at the bottom left. */
    float2 p[3];
    for (int k = 0; k < 3; ++k) {
      const float2 st = uv(t, k);
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

  return tile.set_pass_pixels(kBakePrimitivePass, 3, primitive.data()) &&
         tile.set_pass_pixels(kBakeDifferentialPass, 4, differential.data());
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
