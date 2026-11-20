/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "util/thread.h"

#include <pxr/imaging/hd/renderDelegate.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

struct SceneLock {
  SceneLock(const PXR_NS::HdRenderParam *renderParam);
  ~SceneLock();

  CCL_NS::Scene *scene;

 private:
  CCL_NS::thread_scoped_lock sceneLock;
};

class HdCyclesSession final : public PXR_NS::HdRenderParam {
 public:
  HdCyclesSession(CCL_NS::Session *session_, const bool keep_nodes);
  HdCyclesSession(const CCL_NS::SessionParams &params);
  ~HdCyclesSession() override;

  void UpdateScene();

  double GetStageMetersPerUnit() const
  {
    return _stageMetersPerUnit;
  }

  void SetStageMetersPerUnit(const double stageMetersPerUnit)
  {
    _stageMetersPerUnit = stageMetersPerUnit;
  }

  PXR_NS::HdRenderPassAovBinding GetDisplayAovBinding() const
  {
    return _displayAovBinding;
  }

  void SetDisplayAovBinding(const PXR_NS::HdRenderPassAovBinding &aovBinding)
  {
    _displayAovBinding = aovBinding;
  }

  const PXR_NS::HdRenderPassAovBindingVector &GetAovBindings() const
  {
    return _aovBindings;
  }

  void SyncAovBindings(const PXR_NS::HdRenderPassAovBindingVector &aovBindings);

  /* `cycles:background:transparent` (Blender's Film › Transparent): -1 = not set, the default depends on
   * whether the stage has a dome light. */
  void SetTransparentBackground(const int transparent)
  {
    _backgroundDirty = _backgroundDirty || transparent != _transparentBackground;
    _transparentBackground = transparent;
  }

  void RemoveAovBinding(PXR_NS::HdRenderBuffer *renderBuffer);

  /* Motion blur: the render camera's shutter (frames, relative to the current frame), set when the camera syncs
   * (before the rprims); `cycles:motion_blur` can turn it off. On and open: 3 motion steps, evenly over the shutter
   * (Cycles' layout), else none. */
  void SetShutter(const float open, const float close)
  {
    _shutterOpen = open;
    _shutterClose = close;
  }
  /* Set by a host that never renders while Hydra syncs (cycles_usd_render): each rprim then fills its own geometry
   * outside the scene lock, so Hydra's parallel sync is parallel. Interactive hosts (Blender's viewport) leave it
   * off: the session thread may update the scene meanwhile. */
  void SetExclusiveSync(const bool exclusive)
  {
    _exclusiveSync = exclusive;
  }
  bool ExclusiveSync() const
  {
    return _exclusiveSync;
  }
  void SetMotionBlurAllowed(const bool allowed)
  {
    _motionBlurAllowed = allowed;
  }
  int MotionSteps() const
  {
    return (_motionBlurAllowed && _shutterClose > _shutterOpen) ? 3 : 0;
  }
  float ShutterOpen() const
  {
    return _shutterOpen;
  }
  float ShutterClose() const
  {
    return _shutterClose;
  }
  /* Shutter offset (frames) of a time-ordered motion step. */
  float MotionTime(const int step) const
  {
    const int steps = MotionSteps();
    return steps > 1 ? _shutterOpen + (_shutterClose - _shutterOpen) * float(step) / float(steps - 1) : 0.0f;
  }

  CCL_NS::Session *session;
  bool keep_nodes;

 private:
  const bool _ownCyclesSession;
  double _stageMetersPerUnit = 0.01;
  PXR_NS::HdRenderPassAovBindingVector _aovBindings;
  PXR_NS::HdRenderPassAovBinding _displayAovBinding;
  float _shutterOpen = 0.0f;
  float _shutterClose = 0.0f;
  bool _motionBlurAllowed = true;
  bool _exclusiveSync = false;
  int _transparentBackground = -1;
  bool _backgroundDirty = false; /* the setting changed: update the background once */
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
