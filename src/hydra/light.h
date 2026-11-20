/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "util/transform.h"
#include "util/types.h"

#include <pxr/base/vt/types.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/light.h>

#include <vector>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* The objects of a light that is instanced by a point instancer or a native instance. Hydra has
 * no instancing for sprims: UsdImaging gives the prototype's light sprim its instancer in the
 * instancedBy schema. The light's own object is the first instance, the objects of the others
 * share its light (or mesh) and copy its settings. */
class HdCyclesLightInstances {
 public:
  /* Reads the light transform and, for an instanced light, syncs its instancer and the instance
   * transforms. An instancer change makes the light DirtyTransform. */
  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            const PXR_NS::SdfPath &id,
            const PXR_NS::HdContainerDataSourceHandle &primDs,
            PXR_NS::HdDirtyBits *dirtyBits);
  /* World transform of the light's own object. */
  CCL_NS::Transform GetTransform() const;
  /* An instanced light without instances, to hide. */
  bool IsEmpty() const;
  /* Objects of the other instances, copies of the light's own `object`; `normalize` for a light
   * whose strength is normalized by its size. Scene locked. */
  void Update(CCL_NS::Scene *scene,
              HdCyclesSession *session,
              const CCL_NS::Object *object,
              bool normalize = false);
  void Finalize(HdCyclesSession *session, const PXR_NS::SdfPath &id);

 private:
  PXR_NS::GfMatrix4d _xform = PXR_NS::GfMatrix4d(1.0);
  PXR_NS::SdfPath _instancerId;
  PXR_NS::VtMatrix4dArray _transforms;
  std::vector<CCL_NS::Object *> _objects;
  /* Light of its own of an object, null when it shares the light of the first. */
  std::vector<CCL_NS::Light *> _lights;
  PXR_NS::HdChangeTracker *_tracker = nullptr;
};

class HdCyclesLight final : public PXR_NS::HdLight {
 public:
  HdCyclesLight(const PXR_NS::SdfPath &sprimId, const PXR_NS::TfToken &lightType);
  ~HdCyclesLight() override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

 private:
  void Initialize(PXR_NS::HdRenderParam *renderParam);

  void PopulateShaderGraph(const PXR_NS::HdContainerDataSourceHandle &lightContainer,
                           PXR_NS::HdRenderParam *renderParam);

  CCL_NS::Object *_object = nullptr;
  HdCyclesLightInstances _instances;
  CCL_NS::Light *_light = nullptr;
  // The light's own shader: a dome light uses its world material's shader instead when it has one
  // (HdCyclesSession::SetWorldMaterial)
  CCL_NS::Shader *_shader = nullptr;
  PXR_NS::TfToken _lightType;
  // Color times intensity of a dome light, baked into its world shader
  CCL_NS::float3 _domeStrength = CCL_NS::one_float3();
  // Quick adjustments of a dome light's world (Cycles tab): stops, factor, radians around +Y
  float _worldExposure = 0.0f;
  float _worldSaturation = 1.0f;
  float _worldRotation = 0.0f;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
