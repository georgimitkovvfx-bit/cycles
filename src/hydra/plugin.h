/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include <pxr/imaging/hd/rendererPlugin.h>
#if PXR_VERSION >= 2408
#  include <pxr/imaging/hd/sceneIndexPlugin.h>
#  include <pxr/usdImaging/usdImaging/apiSchemaAdapter.h>
#endif

PXR_NAMESPACE_OPEN_SCOPE

class HdCyclesPlugin final : public PXR_NS::HdRendererPlugin {
 public:
  HdCyclesPlugin();
  ~HdCyclesPlugin() override;

#if PXR_VERSION >= 2511
  bool IsSupported(HdRendererCreateArgs const &rendererCreateArgs,
                   std::string *reasonWhyNot = nullptr) const override;
#endif
#if PXR_VERSION < 2608
  bool IsSupported(bool gpuEnabled) const override;
#endif

  PXR_NS::HdRenderDelegate *CreateRenderDelegate() override;
  PXR_NS::HdRenderDelegate *CreateRenderDelegate(
      const PXR_NS::HdRenderSettingsMap & /*settingsMap*/) override;

  void DeleteRenderDelegate(PXR_NS::HdRenderDelegate *) override;
};

#if PXR_VERSION >= 2408
class HdCycles_ImplicitSurfaceSceneIndexPlugin final : public PXR_NS::HdSceneIndexPlugin {
 protected:
  HdSceneIndexBaseRefPtr _AppendSceneIndex(const HdSceneIndexBaseRefPtr &inputScene,
                                           const HdContainerDataSourceHandle &inputArgs) override;
};

/* Keyless API schema adapter (runs for every prim): the ids of UsdGeomPoints as the vertex
 * primvar "ids", which UsdImaging does not pass to Hydra. */
class HdCycles_PointsIdsAdapter final : public PXR_NS::UsdImagingAPISchemaAdapter {
 public:
  using BaseAdapter = PXR_NS::UsdImagingAPISchemaAdapter;

  HdContainerDataSourceHandle GetImagingSubprimData(
      const UsdPrim &prim,
      const TfToken &subprim,
      const TfToken &appliedInstanceName,
      const UsdImagingDataSourceStageGlobals &stageGlobals) override;

  HdDataSourceLocatorSet InvalidateImagingSubprim(
      const UsdPrim &prim,
      const TfToken &subprim,
      const TfToken &appliedInstanceName,
      const TfTokenVector &properties,
      UsdImagingPropertyInvalidationType invalidationType) override;
};
#endif

PXR_NAMESPACE_CLOSE_SCOPE
