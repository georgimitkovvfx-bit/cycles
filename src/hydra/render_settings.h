/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/renderSettings.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Render settings prim. Hydra syncs it before materials and geometry, so the primvar AOVs of the
 * active render settings are known when shaders are built and primvars exported. The render
 * itself is still driven by the AOV bindings of the render pass. */
class HdCyclesRenderSettings final : public HdRenderSettings {
 public:
  HdCyclesRenderSettings(const SdfPath &bprimId);

  void Finalize(HdRenderParam *renderParam) override;

 protected:
  void _Sync(HdSceneDelegate *sceneDelegate,
             HdRenderParam *renderParam,
             const HdDirtyBits *dirtyBits) override;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
