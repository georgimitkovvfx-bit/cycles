/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/camera.h"
#include "hydra/session.h"
#include "hydra/util.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "session/session.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/cameraSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usdGeom/tokens.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

extern Transform convert_transform(const GfMatrix4d &matrix);
/* USD cameras look along -Z. Cycles' panoramic projections look along +X (the mirror ball along
 * -Y), rotate them onto -Z like Blender's blender_camera_matrix. Needs the camera type set. */
static Transform convert_camera_transform(const GfMatrix4d &matrix, const Camera *cam)
{
  const Transform t = convert_transform(matrix);
  if (cam->get_camera_type() != CAMERA_PANORAMA) {
    return t * transform_scale(1.0f, 1.0f, -1.0f);
  }
  if (cam->get_panorama_type() == PANORAMA_MIRRORBALL) {
    return t *
           make_transform(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
  }
  return t *
         make_transform(0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f);
}

namespace {

/* Value of a Cycles camera setting, empty if not authored: the "primvars:cycles:camera:<setting>"
 * primvar the Camera LOP authors, or a "cycles:camera:<setting>" attribute (earlier builds; these
 * do not update a running render: UsdImaging dirties no camera locator for them). Attributes
 * outside the camera schema are in its namespaced properties, by namespace. */
VtValue GetCameraSetting(const HdContainerDataSourceHandle &primDs, const std::string &setting)
{
  const TfToken name("cycles:camera:" + setting);
  if (auto ds = HdPrimvarsSchema::GetFromParent(primDs).GetPrimvar(name).GetPrimvarValue()) {
    return ds->GetValue(0.0f);
  }
  const HdCameraSchema cameraSchema = HdCameraSchema::GetFromParent(primDs);
  if (const HdContainerDataSourceHandle cameraDs = cameraSchema.GetContainer()) {
    if (auto ds = HdSampledDataSource::Cast(cameraDs->Get(name))) {
      return ds->GetValue(0.0f);
    }
  }
  const HdSampledDataSourceContainerContainerSchema namespaces =
      cameraSchema.GetNamespacedProperties();
  for (const TfToken &ns : {TfToken("cycles"), name}) {
    const HdSampledDataSourceContainerSchema properties = namespaces.Get(ns);
    if (!properties) {
      continue;
    }
    for (const TfToken &key : {name, TfToken("camera:" + setting)}) {
      if (auto ds = properties.Get(key)) {
        return ds->GetValue(0.0f);
      }
    }
  }
  return VtValue();
}

template<typename T>
T GetCameraSetting(const HdContainerDataSourceHandle &primDs,
                   const std::string &setting,
                   const T &fallback)
{
  return VtValue::Cast<T>(GetCameraSetting(primDs, setting)).GetWithDefault(fallback);
}

std::string GetCameraStringSetting(const HdContainerDataSourceHandle &primDs,
                                   const std::string &setting,
                                   const std::string &fallback)
{
  const VtValue value = GetCameraSetting(primDs, setting);
  if (value.IsHolding<TfToken>()) {
    return value.UncheckedGet<TfToken>().GetString();
  }
  return value.IsHolding<std::string>() ? value.UncheckedGet<std::string>() : fallback;
}

/* Blender's Shutter Curve from the "shutter_curve" ramp of the Camera LOP, in Houdini's ramp
 * convention: the values, with their positions in "shutter_curve_pos" and interpolation in
 * "shutter_curve_basis". Values without positions are evenly spaced. Empty when not authored.
 * shortcut: the spline bases (catmull-rom, monotonecubic, bezier, bspline, hermite) are all
 * evaluated as Catmull-Rom; evaluate each one like Houdini if the shapes need to match. */
std::vector<float> ReadShutterCurve(const HdContainerDataSourceHandle &primDs)
{
  const VtValue valuesValue = VtValue::Cast<VtFloatArray>(
      GetCameraSetting(primDs, "shutter_curve"));
  if (valuesValue.IsEmpty() || valuesValue.UncheckedGet<VtFloatArray>().empty()) {
    return {};
  }
  const VtFloatArray &values = valuesValue.UncheckedGet<VtFloatArray>();
  const size_t count = values.size();
  VtFloatArray positions = VtValue::Cast<VtFloatArray>(
                               GetCameraSetting(primDs, "shutter_curve_pos"))
                               .GetWithDefault(VtFloatArray());
  std::vector<std::string> bases(count, "linear");
  const VtValue basisValue = GetCameraSetting(primDs, "shutter_curve_basis");
  if (basisValue.IsHolding<VtTokenArray>() && basisValue.GetArraySize() == count) {
    for (size_t i = 0; i < count; ++i) {
      bases[i] = basisValue.UncheckedGet<VtTokenArray>()[i].GetString();
    }
  }
  else if (basisValue.IsHolding<VtStringArray>() && basisValue.GetArraySize() == count) {
    bases.assign(basisValue.UncheckedGet<VtStringArray>().begin(),
                 basisValue.UncheckedGet<VtStringArray>().end());
  }
  else if (basisValue.IsHolding<TfToken>() || basisValue.IsHolding<std::string>()) {
    bases.assign(count,
                 basisValue.IsHolding<TfToken>() ? basisValue.UncheckedGet<TfToken>().GetString() :
                                                   basisValue.UncheckedGet<std::string>());
  }
  if (positions.size() != count) {
    positions.resize(count);
    for (size_t i = 0; i < count; ++i) {
      positions[i] = count > 1 ? float(i) / float(count - 1) : 0.0f;
    }
  }
  /* Keys sorted by position. */
  std::vector<size_t> keys(count);
  for (size_t i = 0; i < count; ++i) {
    keys[i] = i;
  }
  std::stable_sort(keys.begin(), keys.end(), [&](const size_t a, const size_t b) {
    return positions[a] < positions[b];
  });
  const auto value = [&](const ptrdiff_t k) {
    return values[keys[std::clamp<ptrdiff_t>(k, 0, ptrdiff_t(count) - 1)]];
  };

  std::vector<float> curve(RAMP_TABLE_SIZE);
  for (int i = 0; i < RAMP_TABLE_SIZE; ++i) {
    const float x = float(i) / float(RAMP_TABLE_SIZE - 1);
    /* Last key at or before x. */
    ptrdiff_t k = 0;
    while (k + 1 < ptrdiff_t(count) && positions[keys[k + 1]] <= x) {
      ++k;
    }
    if (x <= positions[keys[0]] || k + 1 >= ptrdiff_t(count)) {
      curve[i] = value(x <= positions[keys[0]] ? 0 : k);
      continue;
    }
    const float x0 = positions[keys[k]];
    const float x1 = positions[keys[k + 1]];
    const float t = x1 > x0 ? (x - x0) / (x1 - x0) : 0.0f;
    const std::string &basis = bases[keys[k]];
    if (basis == "constant") {
      curve[i] = value(k);
    }
    else if (basis == "linear") {
      curve[i] = value(k) + (value(k + 1) - value(k)) * t;
    }
    else {
      const float p0 = value(k - 1), p1 = value(k), p2 = value(k + 1), p3 = value(k + 2);
      curve[i] = 0.5f *
                 (2.0f * p1 + (p2 - p0) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                  (3.0f * p1 - p0 - 3.0f * p2 + p3) * t * t * t);
    }
    /* A weight: never negative. */
    curve[i] = std::max(curve[i], 0.0f);
  }
  return curve;
}

/* Parameters of a custom lens script from primvar values. */
class ScriptParamQuery final : public OSLCameraParamQuery {
 public:
  explicit ScriptParamQuery(const std::map<std::string, VtValue> &params) : _params(params) {}

  bool get_float(ustring name, vector<float> &data) override
  {
    const VtValue value = Find(name);
    if (value.IsHolding<double>()) {
      data = {float(value.UncheckedGet<double>())};
      return true;
    }
    const VtValue floats = PrimvarAsFloats(value);
    if (floats.IsEmpty()) {
      return false;
    }
    const float *begin = static_cast<const float *>(HdGetValueData(floats));
    const HdTupleType tuple = HdGetValueTupleType(floats);
    data.assign(begin, begin + tuple.count * HdGetComponentCount(tuple.type));
    return true;
  }

  bool get_int(ustring name, vector<int> &data) override
  {
    const VtValue value = Find(name);
    if (value.IsHolding<int>()) {
      data = {value.UncheckedGet<int>()};
    }
    else if (value.IsHolding<bool>()) {
      data = {int(value.UncheckedGet<bool>())};
    }
    else if (value.IsHolding<VtIntArray>()) {
      data.assign(value.UncheckedGet<VtIntArray>().begin(),
                  value.UncheckedGet<VtIntArray>().end());
    }
    else {
      return false;
    }
    return true;
  }

  bool get_string(ustring name, std::string &data) override
  {
    const VtValue value = Find(name);
    if (value.IsHolding<std::string>()) {
      data = value.UncheckedGet<std::string>();
    }
    else if (value.IsHolding<TfToken>()) {
      data = value.UncheckedGet<TfToken>().GetString();
    }
    else if (value.IsHolding<SdfAssetPath>()) {
      const SdfAssetPath &path = value.UncheckedGet<SdfAssetPath>();
      data = path.GetResolvedPath().empty() ? path.GetAssetPath() : path.GetResolvedPath();
    }
    else {
      return false;
    }
    return true;
  }

 private:
  VtValue Find(const ustring &name) const
  {
    const auto it = _params.find(name.string());
    return it == _params.end() ? VtValue() : it->second;
  }

  const std::map<std::string, VtValue> &_params;
};

}  // namespace

HdCyclesCamera::HdCyclesCamera(const SdfPath &sprimId) : HdCamera(sprimId)
{
  // Synchronize default values
  _horizontalAperture = _data.GetHorizontalAperture() * GfCamera::APERTURE_UNIT;
  _verticalAperture = _data.GetVerticalAperture() * GfCamera::APERTURE_UNIT;
  _horizontalApertureOffset = _data.GetHorizontalApertureOffset() * GfCamera::APERTURE_UNIT;
  _verticalApertureOffset = _data.GetVerticalApertureOffset() * GfCamera::APERTURE_UNIT;
  _focalLength = _data.GetFocalLength() * GfCamera::FOCAL_LENGTH_UNIT;
  _clippingRange = _data.GetClippingRange();
  _fStop = _data.GetFStop();
  _focusDistance = _data.GetFocusDistance();
}

HdCyclesCamera::~HdCyclesCamera() = default;

HdDirtyBits HdCyclesCamera::GetInitialDirtyBitsMask() const
{
  return DirtyBits::AllDirty;
}

void HdCyclesCamera::Sync(HdSceneDelegate *sceneDelegate,
                          HdRenderParam *renderParam,
                          HdDirtyBits *dirtyBits)
{
  if (*dirtyBits == DirtyBits::Clean) {
    return;
  }

  const SdfPath &id = GetId();
  const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
  const HdContainerDataSourceHandle &primDs = prim.dataSource;

  HdCameraSchema cameraSchema = HdCameraSchema::GetFromParent(primDs);
  HdContainerDataSourceHandle cameraDs = cameraSchema.GetContainer();

  /* Read the shutter window from the camera prim so animated transforms can
   * be sampled across it for motion blur. */
  float shutterOpen = 0.0f;
  float shutterClose = 0.0f;
  if (auto ds = cameraSchema.GetShutterOpen()) {
    shutterOpen = float(ds->GetTypedValue(0.0f));
  }
  if (auto ds = cameraSchema.GetShutterClose()) {
    shutterClose = float(ds->GetTypedValue(0.0f));
  }
  _shutterOpen = shutterOpen;
  _shutterClose = shutterClose;

  auto *const session = static_cast<HdCyclesSession *>(renderParam);
  const std::vector<float> motionTimes = session->GetMotionTimes();
  session->SetCameraShutter(shutterOpen, shutterClose);
  if (session->GetMotionTimes() != motionTimes) {
    session->RequestResync(false, true);
  }

  /* Zoom blur, and zoom in the motion pass: the focal length at the first and last motion step
   * (the shutter ends, or the previous and next frame). shortcut: an animated aperture is not
   * blurred, sample it too when that is needed. */
  _zoomPre = _zoomPost = 1.0f;
  if (auto ds = cameraSchema.GetFocalLength()) {
    const std::vector<float> times = session->GetMotionTimes(shutterOpen, shutterClose);
    const float focalLength = ds->GetTypedValue(0.0f);
    if (times.size() > 1 && focalLength > 0.0f) {
      const float pre = ds->GetTypedValue(times.front());
      const float post = ds->GetTypedValue(times.back());
      if (pre > 0.0f && post > 0.0f) {
        _zoomPre = focalLength / pre;
        _zoomPost = focalLength / post;
      }
    }
  }

  if (*dirtyBits & DirtyBits::DirtyTransform) {
    HdXformSchema xformSchema = HdXformSchema::GetFromParent(primDs);
    if (auto matrixDs = xformSchema.GetMatrix()) {
      // Motion blur: transforms at the evenly spaced motion steps, rendered at the center one
      std::vector<float> times = session->GetMotionTimes(shutterOpen, shutterClose);
      if (times.empty()) {
        times.push_back(0.0f);
      }
      _transformSamples.Resize(times.size());
      for (size_t i = 0; i < times.size(); ++i) {
        _transformSamples.times[i] = times[i];
        _transformSamples.values[i] = matrixDs->GetTypedValue(times[i]);
      }
      _transform = _transformSamples.values[times.size() / 2];
      _data.SetTransform(_transform);
    }
  }

  if (*dirtyBits & DirtyBits::DirtyWindowPolicy) {
    /* The window policy is not part of the camera schema; it lives on the
     * legacy data source. Leave the default of `CameraUtilFit`. */
  }

  if (*dirtyBits & DirtyBits::DirtyClipPlanes) {
    if (auto ds = cameraSchema.GetClippingPlanes()) {
      const VtArray<GfVec4d> clipPlanes = ds->GetTypedValue(0.0f);
      _clipPlanes.assign(clipPlanes.cbegin(), clipPlanes.cend());
    }
  }

  if (*dirtyBits & DirtyBits::DirtyParams) {
    if (auto ds = cameraSchema.GetProjection()) {
      const TfToken projection = ds->GetTypedValue(0.0f);
      _projection = (projection == HdCameraSchemaTokens->orthographic) ? Orthographic :
                                                                         Perspective;
      _data.SetProjection(_projection != Orthographic ? GfCamera::Perspective :
                                                        GfCamera::Orthographic);
    }

    if (auto ds = cameraSchema.GetHorizontalAperture()) {
      const float horizontalAperture = ds->GetTypedValue(0.0f);
      _horizontalAperture = horizontalAperture;
      _data.SetHorizontalAperture(horizontalAperture / GfCamera::APERTURE_UNIT);
    }

    if (auto ds = cameraSchema.GetVerticalAperture()) {
      const float verticalAperture = ds->GetTypedValue(0.0f);
      _verticalAperture = verticalAperture;
      _data.SetVerticalAperture(verticalAperture / GfCamera::APERTURE_UNIT);
    }

    if (auto ds = cameraSchema.GetHorizontalApertureOffset()) {
      const float horizontalApertureOffset = ds->GetTypedValue(0.0f);
      _horizontalApertureOffset = horizontalApertureOffset;
      _data.SetHorizontalApertureOffset(horizontalApertureOffset / GfCamera::APERTURE_UNIT);
    }

    if (auto ds = cameraSchema.GetVerticalApertureOffset()) {
      const float verticalApertureOffset = ds->GetTypedValue(0.0f);
      _verticalApertureOffset = verticalApertureOffset;
      _data.SetVerticalApertureOffset(verticalApertureOffset / GfCamera::APERTURE_UNIT);
    }

    if (auto ds = cameraSchema.GetFocalLength()) {
      const float focalLength = ds->GetTypedValue(0.0f);
      _focalLength = focalLength;
      _data.SetFocalLength(focalLength / GfCamera::FOCAL_LENGTH_UNIT);
    }

    if (auto ds = cameraSchema.GetClippingRange()) {
      const GfVec2f range = ds->GetTypedValue(0.0f);
      const GfRange1f clippingRange(range[0], range[1]);
      _clippingRange = clippingRange;
      _data.SetClippingRange(clippingRange);
    }

    if (auto ds = cameraSchema.GetFStop()) {
      const float fStop = ds->GetTypedValue(0.0f);
      _fStop = fStop;
      _data.SetFStop(fStop);
    }

    if (auto ds = cameraSchema.GetFocusDistance()) {
      const float focusDistance = ds->GetTypedValue(0.0f);
      _focusDistance = focusDistance;
      _data.SetFocusDistance(focusDistance);
    }

    /* USD exposure (exposure, exposure:time/iso/fStop/responsivity) like Karma applies it. */
    _exposureScale = 1.0f;
    if (auto ds = cameraSchema.GetLinearExposureScale()) {
      _exposureScale = ds->GetTypedValue(0.0f);
    }
    else if (auto ds = cameraSchema.GetExposure()) {
      _exposureScale = exp2f(ds->GetTypedValue(0.0f));
    }

    const CyclesSettings defaults;
    CyclesSettings &settings = _settings;
    settings.blades = std::max(0, GetCameraSetting(primDs, "blades", defaults.blades));
    settings.bladesRotation = GetCameraSetting(primDs, "blades_rotation", defaults.bladesRotation);
    settings.apertureRatio = GetCameraSetting(primDs, "aperture_ratio", defaults.apertureRatio);
    settings.rollingShutterType = GetCameraStringSetting(
        primDs, "rolling_shutter_type", defaults.rollingShutterType);
    settings.rollingShutterDuration = GetCameraSetting(
        primDs, "rolling_shutter_duration", defaults.rollingShutterDuration);
    settings.panoramaType = GetCameraStringSetting(primDs, "panorama_type", defaults.panoramaType);
    settings.fisheyeFov = GetCameraSetting(primDs, "fisheye_fov", defaults.fisheyeFov);
    settings.fisheyeLens = GetCameraSetting(primDs, "fisheye_lens", defaults.fisheyeLens);
    settings.latitudeMin = GetCameraSetting(primDs, "latitude_min", defaults.latitudeMin);
    settings.latitudeMax = GetCameraSetting(primDs, "latitude_max", defaults.latitudeMax);
    settings.longitudeMin = GetCameraSetting(primDs, "longitude_min", defaults.longitudeMin);
    settings.longitudeMax = GetCameraSetting(primDs, "longitude_max", defaults.longitudeMax);
    for (int i = 0; i < 5; ++i) {
      settings.fisheyePolynomial[i] = GetCameraSetting(
          primDs, "fisheye_polynomial_k" + std::to_string(i), defaults.fisheyePolynomial[i]);
    }
    settings.centralCylindricalRangeUMin = GetCameraSetting(
        primDs, "central_cylindrical_range_u_min", defaults.centralCylindricalRangeUMin);
    settings.centralCylindricalRangeUMax = GetCameraSetting(
        primDs, "central_cylindrical_range_u_max", defaults.centralCylindricalRangeUMax);
    settings.centralCylindricalRangeVMin = GetCameraSetting(
        primDs, "central_cylindrical_range_v_min", defaults.centralCylindricalRangeVMin);
    settings.centralCylindricalRangeVMax = GetCameraSetting(
        primDs, "central_cylindrical_range_v_max", defaults.centralCylindricalRangeVMax);
    settings.customScript = GetCameraStringSetting(primDs, "custom_script", "");
    if (const VtValue script = GetCameraSetting(primDs, "custom_script");
        script.IsHolding<SdfAssetPath>())
    {
      const SdfAssetPath &path = script.UncheckedGet<SdfAssetPath>();
      settings.customScript = path.GetResolvedPath().empty() ? path.GetAssetPath() :
                                                               path.GetResolvedPath();
    }
    settings.shutterCurve = ReadShutterCurve(primDs);
    settings.scriptParams.clear();
    const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(primDs);
    static const std::string scriptPrefix = "cycles:camera:script:";
    for (const TfToken &name : primvars.GetPrimvarNames()) {
      if (TfStringStartsWith(name.GetString(), scriptPrefix)) {
        if (auto ds = primvars.GetPrimvar(name).GetPrimvarValue()) {
          settings.scriptParams[name.GetString().substr(scriptPrefix.size())] = ds->GetValue(0.0f);
        }
      }
    }
  }

  *dirtyBits = DirtyBits::Clean;
}

void HdCyclesCamera::Finalize(HdRenderParam *renderParam)
{
  HdCamera::Finalize(renderParam);
}

void HdCyclesCamera::ApplyCameraSettings(HdRenderParam *renderParam,
                                         Camera *cam,
                                         CameraUtilConformWindowPolicy windowPolicy,
                                         float aspectRatio) const
{
  const bool panorama = ApplyCyclesSettings(
      _settings, cam, static_cast<HdCyclesSession *>(renderParam)->session->scene.get());
  ApplyCameraSettings(renderParam, _data, windowPolicy, cam, aspectRatio, panorama);

  array<Transform> motion(_transformSamples.count);
  for (size_t i = 0; i < _transformSamples.count; ++i) {
    motion[i] = convert_camera_transform(_transformSamples.values[i], cam);
  }
  cam->set_motion(motion);

  /* The projection at the first and last motion step: the Cycles field of view is the vertical
   * one, its tangent scales with the inverse focal length. */
  if (cam->get_camera_type() == CAMERA_PERSPECTIVE) {
    const float tanHalfFov = tanf(cam->get_fov() * 0.5f);
    cam->set_fov_pre(2.0f * atanf(tanHalfFov * _zoomPre));
    cam->set_fov_post(2.0f * atanf(tanHalfFov * _zoomPost));
  }
  cam->set_use_perspective_motion(cam->get_fov_pre() != cam->get_fov() ||
                                  cam->get_fov_post() != cam->get_fov());

  // Motion steps are evenly spaced over the camera shutter (in frames)
  if (_shutterClose > _shutterOpen) {
    cam->set_shuttertime(float(_shutterClose - _shutterOpen));
    /* Only volume velocity blur uses these, to move the density from the frame time. The motion
     * steps of everything else are sampled at their times over the shutter. */
    cam->set_motion_position(MOTION_POSITION_CENTER);
    cam->set_motion_time_offset(float(-_shutterOpen / (_shutterClose - _shutterOpen)));
  }
  else {
    // Cycles' defaults, not the shutter of the camera rendered before
    cam->set_shuttertime(1.0f);
    cam->set_motion_position(MOTION_POSITION_CENTER);
    cam->set_motion_time_offset(FLT_MAX);
  }
}

void HdCyclesCamera::ApplyCameraSettings(HdRenderParam *renderParam,
                                         const GfCamera &dataUnconformedWindow,
                                         CameraUtilConformWindowPolicy windowPolicy,
                                         Camera *cam,
                                         float aspectRatio,
                                         const bool panorama)
{
  const float width = cam->get_full_width();
  const float height = cam->get_full_height();

  auto data = dataUnconformedWindow;
  CameraUtilConformWindow(
      &data, windowPolicy, (aspectRatio != 0.0f) ? aspectRatio : width / height);

  // Each socket is set once per render pass: a value changed back still restarts the render
  if (panorama) {
    // Set by ApplyCyclesSettings
  }
  else if (data.GetProjection() == GfCamera::Orthographic) {
    cam->set_camera_type(CAMERA_ORTHOGRAPHIC);
  }
  else {
    cam->set_camera_type(CAMERA_PERSPECTIVE);
  }

  const float metersPerUnit = static_cast<HdCyclesSession *>(renderParam)->GetStageMetersPerUnit();

  auto viewplane = data.GetFrustum().GetWindow();
  auto focalLength = 1.0f;
  if (data.GetProjection() == GfCamera::Perspective) {
    viewplane *= 2.0 / viewplane.GetSize()[1];  // Normalize viewplane
    focalLength = data.GetFocalLength() * GfCamera::FOCAL_LENGTH_UNIT;

    cam->set_fov(GfDegreesToRadians(data.GetFieldOfView(GfCamera::FOVVertical)));
    /* The motion pass projects with these at the previous and next frame, motion blur at the
     * shutter ends: no zoom here, a camera prim sets its zoom after this. */
    cam->set_fov_pre(cam->get_fov());
    cam->set_fov_post(cam->get_fov());
  }

  /* Lens properties are in tenths of a scene unit, convert sensor size to meters. Panoramic
   * fisheye lenses use the sensor (the aperture conformed to the image) in the unit of the
   * aperture and their lens, which Houdini authors in millimeters like Blender's. */
  const float sensorScale = panorama ? 1.0f : GfCamera::APERTURE_UNIT * metersPerUnit;
  cam->set_sensorwidth(data.GetHorizontalAperture() * sensorScale);
  cam->set_sensorheight(data.GetVerticalAperture() * sensorScale);

  cam->set_nearclip(data.GetClippingRange().GetMin());
  cam->set_farclip(data.GetClippingRange().GetMax());

  // The view plane of panoramic cameras is the whole image
  cam->set_viewplane_left(panorama ? 0.0f : viewplane.GetMin()[0]);
  cam->set_viewplane_right(panorama ? 1.0f : viewplane.GetMax()[0]);
  cam->set_viewplane_bottom(panorama ? 0.0f : viewplane.GetMin()[1]);
  cam->set_viewplane_top(panorama ? 1.0f : viewplane.GetMax()[1]);

  if (data.GetFStop() != 0.0f &&
      !static_cast<HdCyclesSession *>(renderParam)->depthOfFieldDisabled)
  {
    cam->set_focaldistance(data.GetFocusDistance());
    cam->set_aperturesize(focalLength / (2.0f * data.GetFStop()));
  }
  else {
    cam->set_aperturesize(0.0f);  // No depth of field, also after a camera with it
  }

  cam->set_matrix(convert_camera_transform(data.GetTransform(), cam));
}

void HdCyclesCamera::ApplyCameraSettings(HdRenderParam *renderParam,
                                         const GfMatrix4d &worldToViewMatrix,
                                         const GfMatrix4d &projectionMatrix,
                                         const std::vector<GfVec4d> & /*clipPlanes*/,
                                         Camera *cam,
                                         float aspectRatio)
{
  GfCamera data;
  data.SetFromViewAndProjectionMatrix(worldToViewMatrix, projectionMatrix);

  ApplyCyclesSettings(
      CyclesSettings(), cam, static_cast<HdCyclesSession *>(renderParam)->session->scene.get());
  ApplyCameraSettings(renderParam, data, CameraUtilFit, cam, aspectRatio);

  // No camera prim: no motion blur, not the transforms and shutter of the camera rendered before
  array<Transform> motion;
  cam->set_motion(motion);
  cam->set_use_perspective_motion(false);
  cam->set_shuttertime(1.0f);
  cam->set_motion_position(MOTION_POSITION_CENTER);
  cam->set_motion_time_offset(FLT_MAX);
}

bool HdCyclesCamera::ApplyCyclesSettings(const CyclesSettings &settings, Camera *cam, Scene *scene)
{
  // Bokeh shape of the depth of field
  cam->set_blades(uint(settings.blades));
  cam->set_bladesrotation(GfDegreesToRadians(settings.bladesRotation));
  cam->set_aperture_ratio(std::max(settings.apertureRatio, 0.01f));

  cam->set_rolling_shutter_type(settings.rollingShutterType == "top" ?
                                    Camera::ROLLING_SHUTTER_TOP :
                                    Camera::ROLLING_SHUTTER_NONE);
  cam->set_rolling_shutter_duration(std::clamp(settings.rollingShutterDuration, 0.0f, 1.0f));

  /* Constant (Cycles' default) without a curve, not the curve of the camera rendered before. */
  array<float> shutterCurve(RAMP_TABLE_SIZE);
  for (int i = 0; i < RAMP_TABLE_SIZE; ++i) {
    shutterCurve[i] = settings.shutterCurve.empty() ? 1.0f : settings.shutterCurve[i];
  }
  cam->set_shutter_curve(shutterCurve);

  /* A custom lens (OSL camera shader, CPU and OptiX) replaces the projection, like Blender's
   * Custom camera type. Only the render camera, the dicing camera keeps the projection. */
  if (scene && cam == scene->camera) {
    if (!settings.customScript.empty()) {
      ScriptParamQuery params(settings.scriptParams);
      cam->set_osl_camera(scene, params, settings.customScript);
      if (!cam->script_name.empty()) {
        cam->set_camera_type(CAMERA_CUSTOM);
        return true;
      }
    }
    else if (!cam->script_name.empty()) {
      cam->clear_osl_camera(scene);
    }
  }

  /* Panoramic cameras replace the USD projection. Their view plane is the whole image, the
   * sensor (aperture) is used by the fisheye lens types. */
  const SocketType *panoramaSocket = Camera::get_node_type()->find_input(ustring("panorama_type"));
  const ustring panoramaType(settings.panoramaType);
  if (settings.panoramaType == "none" || !panoramaSocket->enum_values->exists(panoramaType)) {
    return false;
  }
  cam->set_camera_type(CAMERA_PANORAMA);
  cam->set_panorama_type(PanoramaType((*panoramaSocket->enum_values)[panoramaType]));
  cam->set_fisheye_fov(GfDegreesToRadians(settings.fisheyeFov));
  cam->set_fisheye_lens(settings.fisheyeLens);
  cam->set_latitude_min(GfDegreesToRadians(settings.latitudeMin));
  cam->set_latitude_max(GfDegreesToRadians(settings.latitudeMax));
  cam->set_longitude_min(GfDegreesToRadians(settings.longitudeMin));
  cam->set_longitude_max(GfDegreesToRadians(settings.longitudeMax));
  cam->set_fisheye_polynomial_k0(settings.fisheyePolynomial[0]);
  cam->set_fisheye_polynomial_k1(settings.fisheyePolynomial[1]);
  cam->set_fisheye_polynomial_k2(settings.fisheyePolynomial[2]);
  cam->set_fisheye_polynomial_k3(settings.fisheyePolynomial[3]);
  cam->set_fisheye_polynomial_k4(settings.fisheyePolynomial[4]);
  cam->set_central_cylindrical_range_u_min(
      GfDegreesToRadians(settings.centralCylindricalRangeUMin));
  cam->set_central_cylindrical_range_u_max(
      GfDegreesToRadians(settings.centralCylindricalRangeUMax));
  cam->set_central_cylindrical_range_v_min(settings.centralCylindricalRangeVMin);
  cam->set_central_cylindrical_range_v_max(settings.centralCylindricalRangeVMax);
  return true;
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
