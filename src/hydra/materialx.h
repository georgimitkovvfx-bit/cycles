/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include "util/unique_ptr.h"

#include <pxr/imaging/hd/materialNetworkSchema.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Render contexts of MaterialX material networks, in order of preference: plain MaterialX
 * (USD "mtlx:surface" outputs) and Karma (Houdini's Karma Material Builder, "kma:surface"). */
const PXR_NS::TfTokenVector &MaterialXRenderContexts();

/* Whether the terminals of a network connect to MaterialX nodes ("ND_..." node ids). */
bool IsMaterialXNetwork(const PXR_NS::HdMaterialNetworkSchema &network);

/* Translate a MaterialX material network (shader nodes with MaterialX node definition ids, like
 * "ND_standard_surface_surfaceshader") to a Cycles shader graph. Surface shaders are translated
 * to the Principled BSDF, other nodes to their Cycles equivalents, and nodes that are defined as
 * node graphs in the MaterialX libraries are expanded. Returns nothing if no terminal could be
 * translated. */
CCL_NS::unique_ptr<CCL_NS::ShaderGraph> BuildMaterialXShaderGraph(
    const PXR_NS::SdfPath &materialId,
    const PXR_NS::HdMaterialNetworkSchema &network,
    const PXR_NS::HdContainerDataSourceHandle &primContainer);

/* The MaterialX nodes LinkMaterialXNode translated into one Cycles graph. */
struct MaterialXLinkCache;

/* Link an output of a MaterialX node of a network of other nodes (with its upstream nodes) to a
 * Cycles input, or set it as constant: MaterialX nodes feeding a Karma Hair (kma_hair) in a Karma
 * Material Builder. Returns false if the output could not be translated. `cache` (empty at first,
 * one per graph) translates a node feeding several inputs once. */
bool LinkMaterialXNode(CCL_NS::ShaderGraph *graph,
                       const PXR_NS::SdfPath &materialId,
                       const PXR_NS::HdMaterialNetworkSchema &network,
                       const PXR_NS::HdContainerDataSourceHandle &primContainer,
                       const PXR_NS::TfToken &nodeName,
                       const PXR_NS::TfToken &outputName,
                       CCL_NS::ShaderInput *input,
                       std::shared_ptr<MaterialXLinkCache> &cache);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
