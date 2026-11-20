/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include "util/unique_ptr.h"

#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>

#include <functional>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesMaterial final : public PXR_NS::HdMaterial {
 public:
  HdCyclesMaterial(const PXR_NS::SdfPath &sprimId);
  ~HdCyclesMaterial() override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

  CCL_NS::Shader *GetCyclesShader() const
  {
    return _shader;
  }

  // Write primvars to AOVs, given as (pass name, primvar name)
  /* AOV outputs of the instanceId and primId object attributes, which every shader writes. */
  static void AddIdAOVs(CCL_NS::ShaderGraph *graph);

  static void AddPrimvarAOVs(CCL_NS::ShaderGraph *graph,
                             const std::vector<std::pair<std::string, std::string>> &aovs);

  // Tiles of the image textures with "<UDIM>" in the file name, found on disk
  static void SetUdimTiles(CCL_NS::ShaderGraph *graph);

 private:
  struct NodeDesc {
    CCL_NS::ShaderNode *node;
    const class UsdToCyclesMapping *mapping;
  };

  void UpdateParameters(NodeDesc &nodeDesc,
                        PXR_NS::HdMaterialNodeParameterContainerSchema params,
                        const PXR_NS::SdfPath &nodePath);

  /* linkOther links an output of a node that has no Cycles node (MaterialX nodes). */
  void UpdateConnections(
      NodeDesc &nodeDesc,
      PXR_NS::HdMaterialNodeSchema nodeSchema,
      const PXR_NS::SdfPath &nodePath,
      CCL_NS::ShaderGraph *shaderGraph,
      const std::function<bool(
          const PXR_NS::TfToken &, const PXR_NS::TfToken &, CCL_NS::ShaderInput *)> &linkOther);

  unique_ptr<CCL_NS::ShaderGraph> BuildShaderGraph(
      const PXR_NS::SdfPath &materialId,
      PXR_NS::HdMaterialNetworkSchema network,
      const PXR_NS::HdContainerDataSourceHandle &primContainer,
      CCL_NS::Scene *scene);

  CCL_NS::Shader *_shader = nullptr;
  /* Hash of this material's data when it was last synced as the material override. */
  size_t _overrideHash = 0;
  std::unordered_map<PXR_NS::SdfPath, NodeDesc, PXR_NS::SdfPath::Hash> _nodes;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
