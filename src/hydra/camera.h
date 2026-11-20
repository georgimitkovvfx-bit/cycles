/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <map>
#include <string>

#include "hydra/config.h"

#include <pxr/base/gf/camera.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/timeSampleArray.h>

#include <string>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesCamera final : public PXR_NS::HdCamera {
 public:
  HdCyclesCamera(const PXR_NS::SdfPath &sprimId);
  ~HdCyclesCamera() override;

  /* The camera window is conformed with the window policy to the aspect ratio of the image, or to
   * the given aspect ratio when it is not zero (for images displayed with non-square pixels).
   * Panoramic cameras get a view plane of the whole image and the sensor in millimeters. */
  void ApplyCameraSettings(PXR_NS::HdRenderParam *renderParam,
                           CCL_NS::Camera *cam,
                           PXR_NS::CameraUtilConformWindowPolicy windowPolicy,
                           float aspectRatio = 0.0f) const;

  static void ApplyCameraSettings(PXR_NS::HdRenderParam *renderParam,
                                  const PXR_NS::GfCamera &dataUnconformedWindow,
                                  PXR_NS::CameraUtilConformWindowPolicy windowPolicy,
                                  CCL_NS::Camera *cam,
                                  float aspectRatio = 0.0f,
                                  bool panorama = false);
  static void ApplyCameraSettings(PXR_NS::HdRenderParam *renderParam,
                                  const PXR_NS::GfMatrix4d &worldToViewMatrix,
                                  const PXR_NS::GfMatrix4d &projectionMatrix,
                                  const std::vector<PXR_NS::GfVec4d> &clipPlanes,
                                  CCL_NS::Camera *cam,
                                  float aspectRatio = 0.0f);

  /* USD camera exposure as a multiplier of the film exposure (1 without a camera prim). */
  float GetExposureScale() const
  {
    return _exposureScale;
  }

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

 private:
  /* Cycles camera settings from "primvars:cycles:camera:<setting>" primvars, like Blender's
   * camera properties. Angles in degrees. */
  struct CyclesSettings {
    int blades = 0;
    float bladesRotation = 0.0f;
    float apertureRatio = 1.0f;
    std::string rollingShutterType = "none";
    float rollingShutterDuration = 0.1f;
    std::string panoramaType = "none";
    float fisheyeFov = 180.0f;
    float fisheyeLens = 10.5f;
    float latitudeMin = -90.0f;
    float latitudeMax = 90.0f;
    float longitudeMin = -180.0f;
    float longitudeMax = 180.0f;
    // Blender's defaults
    float fisheyePolynomial[5] = {-1.1735143712967577e-05f,
                                  -0.019988736953434998f,
                                  -3.3525322965709175e-06f,
                                  3.099275275886036e-06f,
                                  -2.6064646454854524e-08f};
    float centralCylindricalRangeUMin = -180.0f;
    float centralCylindricalRangeUMax = 180.0f;
    float centralCylindricalRangeVMin = -1.0f;
    float centralCylindricalRangeVMax = 1.0f;
    /* Custom (OSL) lens: script file, and its parameters from the
     * "primvars:cycles:camera:script:<parameter>" primvars. */
    std::string customScript;
    std::map<std::string, PXR_NS::VtValue> scriptParams;
    /* The script is the shipped one for Karma's Physical Lens (not set by the user). */
    bool karmaLens = false;
    /* Blender's Shutter Curve: weight over the shutter at RAMP_TABLE_SIZE evenly spaced times,
     * empty for a constant one. */
    std::vector<float> shutterCurve;
  };
  /* Returns whether the camera is panoramic (or custom). The scene is for custom lenses. */
  static bool ApplyCyclesSettings(const CyclesSettings &settings,
                                  CCL_NS::Camera *cam,
                                  CCL_NS::Scene *scene);

  CyclesSettings _settings;
  float _exposureScale = 1.0f;
  PXR_NS::GfCamera _data;
  PXR_NS::HdTimeSampleArray<PXR_NS::GfMatrix4d, 2> _transformSamples;
  /* Zoom over the motion steps: the focal length at the center step over the one at the first
   * and last step. */
  float _zoomPre = 1.0f;
  float _zoomPost = 1.0f;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
