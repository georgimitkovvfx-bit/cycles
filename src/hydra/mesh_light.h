/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/filteringSceneIndex.h>

#include <pxr/usd/sdf/path.h>

#include <vector>

namespace CCL_NS {
class Node;
}  // namespace CCL_NS

PXR_NAMESPACE_OPEN_SCOPE
class HdSceneDelegate;
PXR_NAMESPACE_CLOSE_SCOPE

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesSession;

/* Name of the light prim HdCyclesGeometryLightSceneIndex adds under a geometry light. */
const TfToken &GeometryLightChildName();

TF_DECLARE_REF_PTRS(HdCyclesGeometryLightSceneIndex);

/* Geometry lights (UsdLux LightAPI / MeshLightAPI on a gprim, Houdini's Geometry Light LOP) reach
 * Hydra as gprims with a "light" data source. HdsiLightLinkingSceneIndex computes light links
 * only for prims of a light type, so this adds a child prim of type "light" with the light and
 * collections of the gprim under each one; its light link and shadow link categories are read
 * by the gprim (HdCyclesGeometryLight). The render delegate has no Sprim of that type. */
class HdCyclesGeometryLightSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
 public:
  static HdCyclesGeometryLightSceneIndexRefPtr New(const HdSceneIndexBaseRefPtr &inputSceneIndex);

  HdSceneIndexPrim GetPrim(const SdfPath &primPath) const override;
  SdfPathVector GetChildPrimPaths(const SdfPath &primPath) const override;

 protected:
  HdCyclesGeometryLightSceneIndex(const HdSceneIndexBaseRefPtr &inputSceneIndex);

  void _PrimsAdded(const HdSceneIndexBase &sender,
                   const HdSceneIndexObserver::AddedPrimEntries &entries) override;
  void _PrimsRemoved(const HdSceneIndexBase &sender,
                     const HdSceneIndexObserver::RemovedPrimEntries &entries) override;
  void _PrimsDirtied(const HdSceneIndexBase &sender,
                     const HdSceneIndexObserver::DirtiedPrimEntries &entries) override;

 private:
  /* Geometry lights with a child prim announced. */
  SdfPathSet _lights;
};

/* The light of a geometry light: an emission shader replacing its materials, its light links
 * and light group, and its count in HdCyclesSession::meshLights. Scene locked. */
class HdCyclesGeometryLight {
 public:
  /* `primDs` of the gprim `id`. Call every Sync of the gprim, after its geometry and materials
   * are set. True when the light no longer sets the light group of the instances: the caller
   * applies the object's own settings again. */
  bool Sync(HdCyclesSession *session,
            CCL_NS::Scene *scene,
            HdSceneDelegate *sceneDelegate,
            const SdfPath &id,
            const HdContainerDataSourceHandle &primDs,
            CCL_NS::Geometry *geom,
            const std::vector<CCL_NS::Object *> &instances);

  void Finalize(HdCyclesSession *session, bool keepNodes);

 private:
  CCL_NS::Shader *_shader = nullptr;
  /* Shaders of the geometry without the light shader, by used shader index. */
  std::vector<CCL_NS::Node *> _materialShaders;
  /* The emission of _shader: strength, temperature (0 none), multiple importance sampling. */
  float _key[5] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
  bool _isLight = false;
  bool _lightGroup = false;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
