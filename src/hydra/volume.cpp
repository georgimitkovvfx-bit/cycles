/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/volume.h"
#include "hydra/field.h"
#include "hydra/geometry.inl"
#include "hydra/util.h"
#include "scene/volume.h"

#include <pxr/imaging/hd/volumeFieldBindingSchema.h>

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

  if (dirtyBits & HdChangeTracker::DirtyVolumeField) {
    const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
    HdVolumeFieldBindingSchema bindings = HdVolumeFieldBindingSchema::GetFromParent(
        prim.dataSource);

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
        const ustring name(fieldName.GetString());

        AttributeStandard std = ATTR_STD_NONE;
        if (name == Attribute::standard_name(ATTR_STD_VOLUME_DENSITY)) {
          std = ATTR_STD_VOLUME_DENSITY;
        }
        else if (name == Attribute::standard_name(ATTR_STD_VOLUME_COLOR)) {
          std = ATTR_STD_VOLUME_COLOR;
        }
        else if (name == Attribute::standard_name(ATTR_STD_VOLUME_FLAME)) {
          std = ATTR_STD_VOLUME_FLAME;
        }
        else if (name == Attribute::standard_name(ATTR_STD_VOLUME_HEAT)) {
          std = ATTR_STD_VOLUME_HEAT;
        }
        else if (name == Attribute::standard_name(ATTR_STD_VOLUME_TEMPERATURE)) {
          std = ATTR_STD_VOLUME_TEMPERATURE;
        }
        else if (name == Attribute::standard_name(ATTR_STD_VOLUME_VELOCITY)) {
          std = ATTR_STD_VOLUME_VELOCITY;
        }

        // Skip attributes that are not needed
        if ((std != ATTR_STD_NONE && _geom->need_attribute(scene, std)) ||
            _geom->need_attribute(scene, name))
        {
          Attribute *const attr = (std != ATTR_STD_NONE) ?
                                      _geom->attributes.add(std) :
                                      _geom->attributes.add(name, TypeFloat, ATTR_ELEMENT_VOXEL);
          attr->data_voxel_for_write() = openvdbAsset->GetImageHandle();
        }
      }
    }

    _geom->merge_grids(scene);

    rebuild = true;
  }

  if (dirtyBits & HdChangeTracker::DirtyPrimvar) {
    const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
    const HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);

    /* Constant primvars are object attributes: Cycles clears the other attributes of a volume when
     * it builds the volume mesh. Fields of the same name win, like any geometry attribute. */
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
