/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/render_settings.h"
#include "hydra/session.h"

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdCyclesRenderSettings::HdCyclesRenderSettings(const SdfPath &bprimId) : HdRenderSettings(bprimId)
{
}

void HdCyclesRenderSettings::Finalize(HdRenderParam *renderParam)
{
  const SceneLock lock(renderParam);
  static_cast<HdCyclesSession *>(renderParam)->SetRenderSettingsPrimvarAovs(GetId(), false, {});
  static_cast<HdCyclesSession *>(renderParam)->SetRenderSettingsMotionPass(GetId(), false, false);

  HdRenderSettings::Finalize(renderParam);
}

void HdCyclesRenderSettings::_Sync(HdSceneDelegate * /*sceneDelegate*/,
                                   HdRenderParam *renderParam,
                                   const HdDirtyBits *dirtyBits)
{
  if (!(*dirtyBits & (DirtyActive | DirtyRenderProducts))) {
    return;
  }

  std::vector<std::pair<std::string, std::string>> primvarAovs;
  bool motionPass = false;
  for (const RenderProduct &product : GetRenderProducts()) {
    for (const RenderProduct::RenderVar &var : product.renderVars) {
      motionPass |= HdCyclesSession::GetPassType(TfToken(var.sourceName)) == PASS_MOTION;
      // AOVs are bound under the name of the render var prim
      const std::string primvar = HdCyclesSession::GetPrimvar(var.sourceName, var.sourceType);
      const std::pair<std::string, std::string> aov(var.varPath.GetName(), primvar);
      if (!primvar.empty() &&
          std::find(primvarAovs.begin(), primvarAovs.end(), aov) == primvarAovs.end())
      {
        primvarAovs.push_back(aov);
      }
    }
  }

  const SceneLock lock(renderParam);
  static_cast<HdCyclesSession *>(renderParam)
      ->SetRenderSettingsPrimvarAovs(GetId(), IsActive(), std::move(primvarAovs));
  static_cast<HdCyclesSession *>(renderParam)
      ->SetRenderSettingsMotionPass(GetId(), IsActive(), motionPass);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
