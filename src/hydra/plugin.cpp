/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/plugin.h"
#include "hydra/implicit_mesh.h"
#include "hydra/render_delegate.h"
#include "util/log.h"
#include "util/path.h"

#include <pxr/base/arch/fileSystem.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/thisPlugin.h>
#include <pxr/base/tf/envSetting.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#if PXR_VERSION >= 2408
#  include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#  include <pxr/imaging/hdsi/velocityMotionResolvingSceneIndex.h>
#endif

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_ENV_SETTING(CYCLES_LOGGING, false, "Enable Cycles logging")
TF_DEFINE_ENV_SETTING(CYCLES_LOGGING_LEVEL, "warning", "Cycles logging level")

HdCyclesPlugin::HdCyclesPlugin()
{
  const PlugPluginPtr plugin = PLUG_THIS_PLUGIN;
  // Initialize Cycles paths relative to the plugin resource path
  const std::string rootPath = PXR_NS::ArchAbsPath(plugin->GetResourcePath());
  CCL_NS::path_init(std::move(rootPath));

  if (TfGetEnvSetting(CYCLES_LOGGING)) {
    CCL_NS::log_level_set(TfGetEnvSetting(CYCLES_LOGGING_LEVEL));
  }
}

HdCyclesPlugin::~HdCyclesPlugin() {}

#if PXR_VERSION >= 2511
bool HdCyclesPlugin::IsSupported(HdRendererCreateArgs const & /*rendererCreateArgs*/,
                                 std::string * /*reasonWhyNot*/) const
{
  return true;
}
#endif
#if PXR_VERSION < 2608
bool HdCyclesPlugin::IsSupported(bool /*gpuEnabled*/) const
{
  return true;
}
#endif

HdRenderDelegate *HdCyclesPlugin::CreateRenderDelegate()
{
  return CreateRenderDelegate({});
}

HdRenderDelegate *HdCyclesPlugin::CreateRenderDelegate(const HdRenderSettingsMap &settingsMap)
{
  return new HD_CYCLES_NS::HdCyclesDelegate(settingsMap);
}

void HdCyclesPlugin::DeleteRenderDelegate(HdRenderDelegate *renderDelegate)
{
  delete renderDelegate;
}

#if PXR_VERSION >= 2408
TF_REGISTRY_FUNCTION(TfType)
{
  HdSceneIndexPluginRegistry::Define<PXR_NS::HdCycles_ImplicitSurfaceSceneIndexPlugin>();
}

HdSceneIndexBaseRefPtr HdCycles_ImplicitSurfaceSceneIndexPlugin::_AppendSceneIndex(
    const HdSceneIndexBaseRefPtr &inputScene, const HdContainerDataSourceHandle & /*inputArgs*/)
{
  return HD_CYCLES_NS::HdCyclesImplicitMeshSceneIndex::New(inputScene);
}

/* Cycles has no implicit surfaces: Sphere, Cube, Cone, Cylinder, Capsule and Plane become meshes before they reach
 * the render delegate. At the start of the chain, so every later scene index sees meshes. */
TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
  HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
      "Cycles",
      TfToken("HdCycles_ImplicitSurfaceSceneIndexPlugin"),
      nullptr,
      0,
      HdSceneIndexPluginRegistry::InsertionOrderAtStart);

  /* velocities / accelerations / angular velocities → motion of points and instances over the shutter (UsdGeom's
   * rules), for every host: husk and Houdini's viewport (Karma resolves them itself), usdview, Blender. The frame
   * rate comes from the scene globals (husk's is 24; cycles_usd_render has its own), as in hdCycles-h22. */
  HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
      "Cycles",
      [](const std::string & /*renderInstanceId*/,
         const HdSceneIndexBaseRefPtr &inputScene,
         const HdContainerDataSourceHandle & /*inputArgs*/) -> HdSceneIndexBaseRefPtr {
        return HdsiVelocityMotionResolvingSceneIndex::New(inputScene);
      },
      nullptr,
      0,
      HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
}
#endif

// USD's type system accounts for namespace, so we'd have to register our name as
// HdCycles::HdCyclesPlugin in plugInfo.json, which isn't all that bad for JSON,
// but those colons may cause issues for any USD specific tooling. So just put our
// plugin class in the pxr namespace (which USD's type system will elide).
TF_REGISTRY_FUNCTION(TfType)
{
  HdRendererPluginRegistry::Define<PXR_NS::HdCyclesPlugin>();
}

PXR_NAMESPACE_CLOSE_SCOPE