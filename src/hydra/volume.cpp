/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/volume.h"
#include "hydra/field.h"
#include "hydra/geometry.inl"
#include "hydra/util.h"
#include "scene/volume.h"

#include <pxr/imaging/hd/sceneGlobalsSchema.h>
#include <pxr/imaging/hd/volumeFieldBindingSchema.h>

#include <unordered_map>

HDCYCLES_NAMESPACE_OPEN_SCOPE

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
   (openvdbAsset)
   (houdiniFieldAsset)
);
// clang-format on

HdCyclesVolume::HdCyclesVolume(const SdfPath &rprimId) : HdCyclesGeometry(rprimId) {}

HdCyclesVolume::~HdCyclesVolume() = default;

HdDirtyBits HdCyclesVolume::GetInitialDirtyBitsMask() const
{
  HdDirtyBits bits = HdCyclesGeometry::GetInitialDirtyBitsMask();
  bits |= HdChangeTracker::DirtyVolumeField;
  return bits;
}

void HdCyclesVolume::Populate(HdSceneDelegate *sceneDelegate, HdDirtyBits dirtyBits, bool &rebuild)
{
  Scene *const scene = (Scene *)_geom->get_owner();

  /* USD/Hydra convention is that volume density is in object space. */
  _geom->set_object_space(true);

  /* Motion blur from a velocity field: the kernel moves the density by velocity times
   * velocity_scale over the shutter, with velocities per second like Blender's default. The
   * frame rate is the one velocities of points use (HdsiVelocityMotionResolvingSceneIndex). A
   * deformation, so with the deformation samples. */
  const std::vector<float> motionTimes = GetObjectMotionTimes(nullptr, true);
  float velocityScale = 0.0f;
  if (!motionTimes.empty()) {
    double fps = 24.0;
    if (HdDoubleDataSourceHandle fpsDs =
            HdSceneGlobalsSchema::GetFromSceneIndex(
                sceneDelegate->GetRenderIndex().GetTerminalSceneIndex())
                .GetTimeCodesPerSecond())
    {
      fps = fpsDs->GetTypedValue(0.0f);
    }
    velocityScale = float((motionTimes.back() - motionTimes.front()) / fps);
  }
  /* The velocity field is only loaded with motion blur. */
  const bool motionChanged = (velocityScale != 0.0f) != (_geom->get_velocity_scale() != 0.0f);
  _geom->set_velocity_scale(velocityScale);
  /* Cycles only compiles the kernel with object motion for objects or geometry with motion. */
  _geom->set_use_motion_blur(velocityScale != 0.0f);

  if ((dirtyBits & HdChangeTracker::DirtyVolumeField) || motionChanged) {
    const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
    HdVolumeFieldBindingSchema bindings = HdVolumeFieldBindingSchema::GetFromParent(
        prim.dataSource);

    /* Only the fields the shaders read now: the volume mesh is built from all voxel attributes,
     * which the volume keeps when it is rebuilt. */
    for (auto it = _geom->attributes.attributes.begin(); it != _geom->attributes.attributes.end();)
    {
      if (it->element & ATTR_ELEMENT_VOXEL) {
        _geom->attributes.remove(it++);
      }
      else {
        ++it;
      }
    }

    /* A field kept for the bounds when the shaders need none (density first). */
    bool anyLoaded = false;
    HdCyclesField *boundsField = nullptr;
    ustring boundsName;

    for (const TfToken &fieldName : bindings.GetVolumeFieldBindingNames()) {
      auto pathDs = bindings.GetVolumeFieldBinding(fieldName);
      if (!pathDs) {
        continue;
      }
      const SdfPath fieldId = pathDs->GetTypedValue(0.0f);

      const HdRenderIndex &renderIndex = sceneDelegate->GetRenderIndex();
      auto *openvdbAsset = static_cast<HdCyclesField *>(
          renderIndex.GetBprim(_tokens->openvdbAsset, fieldId));
      if (!openvdbAsset) {
        openvdbAsset = static_cast<HdCyclesField *>(
            renderIndex.GetBprim(_tokens->houdiniFieldAsset, fieldId));
      }
      if (openvdbAsset) {
        /* Houdini's velocity is "vel" (a vector grid, or native volumes that SOP Import binds
         * as vel_x/_y/_z, which Cycles merges into one grid) or "v" (rasterized points). */
        static const std::unordered_map<std::string, std::string> aliases = {
            {"vel", "velocity"},
            {"v", "velocity"},
            {"vel_x", "velocity_x"},
            {"vel_y", "velocity_y"},
            {"vel_z", "velocity_z"},
        };
        const ustring authored(fieldName.GetString());
        const auto alias = aliases.find(authored.string());
        const ustring name(alias != aliases.end() ? alias->second : authored.string());

        const AttributeStandard std = Attribute::name_volume_standard(name);

        // Skip attributes that are not needed
        if ((std != ATTR_STD_NONE && _geom->need_attribute(scene, std)) ||
            _geom->need_attribute(scene, name))
        {
          Attribute *const attr = (std != ATTR_STD_NONE) ?
                                      _geom->attributes.add(std) :
                                      _geom->attributes.add(name, TypeFloat, ATTR_ELEMENT_VOXEL);
          attr->data_voxel_for_write() = openvdbAsset->GetImageHandle();
          anyLoaded = true;
        }
        /* Shaders can also read a velocity field by its own name. */
        if (name != authored && _geom->need_attribute(scene, authored)) {
          _geom->attributes.add(authored, TypeFloat, ATTR_ELEMENT_VOXEL)->data_voxel_for_write() =
              openvdbAsset->GetImageHandle();
          anyLoaded = true;
        }
        if (!boundsField || std == ATTR_STD_VOLUME_DENSITY) {
          boundsField = openvdbAsset;
          boundsName = name;
        }
      }
    }

    /* Cycles builds the volume mesh from the loaded fields: with none (an absorption or emission
     * shader reading no field) the volume rendered empty, Karma fills the field's extent. */
    if (!anyLoaded && boundsField) {
      const AttributeStandard std = Attribute::name_volume_standard(boundsName);
      Attribute *const attr = (std != ATTR_STD_NONE) ?
                                  _geom->attributes.add(std) :
                                  _geom->attributes.add(boundsName, TypeFloat, ATTR_ELEMENT_VOXEL);
      attr->data_voxel_for_write() = boundsField->GetImageHandle();
    }

    _geom->merge_grids(scene);

    rebuild = true;
  }

  /* Also when the instance primvars replaced the object attributes (HdCyclesGeometry::Sync). */
  if (dirtyBits & (HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyInstancer |
                   HdChangeTracker::DirtyMaterialId))
  {
    const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
    const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);

    /* Blender's volume Step Size, in object space: 0 is automatic (from the voxel size). */
    _geom->set_step_size(std::max(
        0.0f,
        VtValue::Cast<float>(GetObjectSetting(primvars, "volume_step_size")).GetWithDefault(0.0f)));

    /* Constant primvars are object attributes: Cycles clears the other attributes of a volume when
     * it builds the volume mesh. Fields of the same name win, like any geometry attribute. Set
     * after the instance primvars, so they win over those like any prototype primvar. */
    for (const TfToken &primvarName :
         PrimvarNamesAtInterpolation(primvars, HdInterpolationConstant))
    {
      const std::vector<ustring> names = RequestedPrimvarNames(
          _geom, scene, primvars, primvarName, ATTR_STD_NONE);
      if (names.empty()) {
        continue;
      }
      const VtValue value = PrimvarAsFloats(ReadPrimvar(primvars, primvarName));
      for (const ustring &name : names) {
        for (Object *instance : _instances) {
          SetObjectAttribute(instance, name, value);
        }
      }
    }
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
