/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* The scene indices the delegate needs before the render index, registered for the renderer
 * "Cycles": by the Hydra plugin (plugin.cpp) and by programs that link the delegate and build
 * their own scene index chain (cycles_usd_render), which never load the plugin library. */

#include "hydra/scene_indices.h"
#include "hydra/link_prefix_scene_index.h"
#include "hydra/mesh_light.h"
#include "hydra/render_pass_scene_index.h"
#include "hydra/resync_scene_index.h"

#include <mutex>

#if PXR_VERSION >= 2408
#  include <pxr/imaging/hd/retainedDataSource.h>
#  include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#  include <pxr/imaging/hd/tokens.h>
#  include <pxr/imaging/hdGp/generativeProceduralResolvingSceneIndex.h>
#  include <pxr/imaging/hdsi/lightLinkingSceneIndex.h>
#  include <pxr/imaging/hdsi/nurbsApproximatingSceneIndex.h>
#  include <pxr/imaging/hdsi/renderPassPruneSceneIndex.h>
#  include <pxr/imaging/hdsi/tetMeshConversionSceneIndex.h>
#endif

HDCYCLES_NAMESPACE_OPEN_SCOPE

void HdCyclesRegisterSceneIndices()
{
#if PXR_VERSION >= 2408
  static std::once_flag once;
  std::call_once(once, [] {
    /* Nor tetrahedral meshes (their surface triangles as a mesh) or NURBS (curves approximated as
     * basis curves, patches as meshes), like Storm. Registered before the procedurals at the start
     * of the phase: after them in the chain, so procedurals can make these too. */
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Cycles",
        [](const std::string & /*renderInstanceId*/,
           const HdSceneIndexBaseRefPtr &inputScene,
           const HdContainerDataSourceHandle & /*inputArgs*/) -> HdSceneIndexBaseRefPtr {
          return HdsiNurbsApproximatingSceneIndex::New(
              HdsiTetMeshConversionSceneIndex::New(inputScene));
        },
        nullptr,
        0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);

    /* The active UsdRenderPass (husk --pass): husk only makes it the scene globals'
     * activeRenderPassPrim, the render delegate applies its prune, renderVisibility,
     * cameraVisibility and matte collections. After the procedurals, so their prims are in the
     * pass. */
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Cycles",
        [](const std::string & /*renderInstanceId*/,
           const HdSceneIndexBaseRefPtr &inputScene,
           const HdContainerDataSourceHandle & /*inputArgs*/) -> HdSceneIndexBaseRefPtr {
          return HdCyclesRenderPassSceneIndex::New(
              HdsiRenderPassPruneSceneIndex::New(inputScene));
        },
        nullptr,
        0,
        HdSceneIndexPluginRegistry::InsertionOrderAtEnd);

    /* Light prims for the geometry lights, so the light linking below computes their links. */
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Cycles",
        [](const std::string & /*renderInstanceId*/,
           const HdSceneIndexBaseRefPtr &inputScene,
           const HdContainerDataSourceHandle & /*inputArgs*/) -> HdSceneIndexBaseRefPtr {
          return HdCyclesGeometryLightSceneIndex::New(inputScene);
        },
        nullptr,
        0,
        HdSceneIndexPluginRegistry::InsertionOrderAtEnd);

    /* Collections below the host's prefix (Blender's Stage View), before the light linking. */
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Cycles",
        [](const std::string & /*renderInstanceId*/,
           const HdSceneIndexBaseRefPtr &inputScene,
           const HdContainerDataSourceHandle & /*inputArgs*/) -> HdSceneIndexBaseRefPtr {
          return HdCyclesLinkPrefixSceneIndex::New(inputScene);
        },
        nullptr,
        0,
        HdSceneIndexPluginRegistry::InsertionOrderAtEnd);

    /* Light and shadow linking: categories of the light linking collections on lights, and the
     * categories each geometry prim belongs to. */
    const VtArray<TfToken> lightTypes(HdLightPrimTypeTokens().begin(),
                                      HdLightPrimTypeTokens().end());
    const VtArray<TfToken> geometryTypes = {HdPrimTypeTokens->mesh,
                                            HdPrimTypeTokens->basisCurves,
                                            HdPrimTypeTokens->points,
                                            HdPrimTypeTokens->particleField,
                                            HdPrimTypeTokens->volume};
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Cycles",
        [](const std::string & /*renderInstanceId*/,
           const HdSceneIndexBaseRefPtr &inputScene,
           const HdContainerDataSourceHandle &inputArgs) -> HdSceneIndexBaseRefPtr {
          return HdsiLightLinkingSceneIndex::New(inputScene, inputArgs);
        },
        HdRetainedContainerDataSource::New(
            HdsiLightLinkingSceneIndexTokens->lightPrimTypes,
            HdRetainedTypedSampledDataSource<VtArray<TfToken>>::New(lightTypes),
            HdsiLightLinkingSceneIndexTokens->lightFilterPrimTypes,
            HdRetainedTypedSampledDataSource<VtArray<TfToken>>::New(
                VtArray<TfToken>{HdPrimTypeTokens->lightFilter}),
            HdsiLightLinkingSceneIndexTokens->geometryPrimTypes,
            HdRetainedTypedSampledDataSource<VtArray<TfToken>>::New(geometryTypes)),
        0,
        HdSceneIndexPluginRegistry::InsertionOrderAtEnd);

    /* Hydra generative procedurals (hdGp plugins), which USD only resolves for every renderer
     * with HDGP_INCLUDE_DEFAULT_RESOLVER; resolving procedurals twice finds none the second time. */
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Cycles",
        [](const std::string & /*renderInstanceId*/,
           const HdSceneIndexBaseRefPtr &inputScene,
           const HdContainerDataSourceHandle & /*inputArgs*/) -> HdSceneIndexBaseRefPtr {
          return HdGpGenerativeProceduralResolvingSceneIndex::New(inputScene);
        },
        nullptr,
        /* Registered after the implicit surfaces at the start of the same phase: first in the
         * chain, so the shapes procedurals make become meshes too. */
        0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);

    /* Last in the chain, after every scene index that adds prims, so that the render delegate can
     * have any prim synced again (HdCyclesDelegate::Update), and the velocities of all prims are
     * resolved, also of prims added by procedurals. */
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Cycles",
        [](const std::string & /*renderInstanceId*/,
           const HdSceneIndexBaseRefPtr &inputScene,
           const HdContainerDataSourceHandle & /*inputArgs*/) -> HdSceneIndexBaseRefPtr {
          return HdCyclesResyncSceneIndex::New(inputScene);
        },
        nullptr,
        1000,
        HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
  });
#endif
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
