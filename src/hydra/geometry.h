/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/rprim.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

template<typename Base, typename CyclesBase> class HdCyclesGeometry : public Base {
 public:
  HdCyclesGeometry(const PXR_NS::SdfPath &rprimId);

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits,
            const PXR_NS::TfToken &reprToken) override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

 protected:
  void _InitRepr(const PXR_NS::TfToken &reprToken, PXR_NS::HdDirtyBits *dirtyBits) override;

  PXR_NS::HdDirtyBits _PropagateDirtyBits(PXR_NS::HdDirtyBits bits) const override;

  virtual void Populate(PXR_NS::HdSceneDelegate *sceneDelegate,
                        PXR_NS::HdDirtyBits dirtyBits,
                        bool &rebuild) = 0;

  CyclesBase *_geom = nullptr;
  bool _instanceIdAttribute = true; /* HdCyclesSession::InstanceIdAov */
  /* Motion blur for this sync (HdCyclesSession): steps evenly over the shutter, 0 = none. */
  int _motionSteps = 0;
  float _shutterOpen = 0.0f;
  float _shutterClose = 0.0f;
  float MotionTime(const int step) const
  {
    return _motionSteps > 1 ? _shutterOpen + (_shutterClose - _shutterOpen) * float(step) /
                                                 float(_motionSteps - 1) :
                              0.0f;
  }
  std::vector<CCL_NS::Object *> _instances;

 private:
  void Initialize(PXR_NS::HdRenderParam *renderParam);

  void InitializeInstance(const int index);

  PXR_NS::GfMatrix4d _geomTransform;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
