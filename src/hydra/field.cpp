/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/field.h"
#include "hydra/util.h"

#include "util/log.h"

#ifdef WITH_OPENVDB
#  include "hydra/session.h"
#  include "scene/image_vdb.h"
#  include "scene/scene.h"

#  include <filesystem>
#  include <map>
#  include <memory>
#  include <mutex>
#  include <tuple>
#endif

#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/volumeFieldSchema.h>
#include <pxr/usd/sdf/assetPath.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

#ifdef WITH_OPENVDB
class HdCyclesVolumeLoader : public VDBImageLoader {
 public:
  HdCyclesVolumeLoader(const std::string &filePath, const std::string &gridName)
      : VDBImageLoader(gridName)
  {
    grid = shared_grid(filePath, gridName);
  }

 private:
  /* Fields naming the same file and grid share one grid: Cycles' image manager tells VDB images apart by their grid
   * pointer, so a VDB used by many Volumes (smoke puffs, a fog bank per shot) is then read and stored once, not once
   * per Volume. The key has the file's time and size: a rewritten file is read again, not served from a grid an
   * older field still holds. Weak: a grid no loader holds any more is read again too. */
  static openvdb::GridBase::ConstPtr shared_grid(const std::string &filePath, const std::string &gridName)
  {
    using Key = std::tuple<std::string, std::string, std::filesystem::file_time_type, std::uintmax_t>;
    static std::mutex mutex;
    static std::map<Key, std::weak_ptr<const openvdb::GridBase>> grids;
    std::error_code error;
    const auto time = std::filesystem::last_write_time(filePath, error);
    const std::uintmax_t size = error ? 0 : std::filesystem::file_size(filePath, error);
    const std::lock_guard<std::mutex> lock(mutex);
    std::weak_ptr<const openvdb::GridBase> &cached = grids[Key(filePath, gridName, time, size)];
    if (openvdb::GridBase::ConstPtr found = cached.lock()) {
      return found;
    }
    openvdb::GridBase::ConstPtr loaded;
    /* Disable delay loading and file copying, this has poor performance on network drives. */
    const bool delay_load = false;
    try {
      openvdb::io::File file(filePath);
#  ifdef OPENVDB_USE_DELAYED_LOADING
      file.setCopyMaxBytes(0);
#  endif
      if (file.open(delay_load)) {
        loaded = file.readGrid(gridName);
      }
    }
    catch (const openvdb::IoError &e) {
      LOG_ERROR << "Error loading OpenVDB file: " << e.what();
    }
    catch (...) {
      LOG_ERROR << "Error loading OpenVDB file: Unknown error";
    }
    cached = loaded;
    return loaded;
  }
};
#endif

HdCyclesField::HdCyclesField(const SdfPath &bprimId, const TfToken & /*typeId*/) : HdField(bprimId)
{
}

HdCyclesField::~HdCyclesField() = default;

HdDirtyBits HdCyclesField::GetInitialDirtyBitsMask() const
{
  return DirtyBits::DirtyParams;
}

void HdCyclesField::Sync(HdSceneDelegate *sceneDelegate,
                         HdRenderParam *renderParam,
                         HdDirtyBits *dirtyBits)
{
#ifdef WITH_OPENVDB
  const SdfPath &id = GetId();

  if (*dirtyBits & DirtyBits::DirtyParams) {
    const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
    HdVolumeFieldSchema schema = HdVolumeFieldSchema::GetFromParent(prim.dataSource);

    SdfAssetPath assetPath;
    if (auto ds = schema.GetFilePath()) {
      assetPath = ds->GetTypedValue(0.0f);
    }

    std::string filename = assetPath.GetResolvedPath();
    if (filename.empty()) {
      filename = assetPath.GetAssetPath();
    }

    if (!filename.empty()) {
      TfToken fieldName;
      if (auto ds = schema.GetFieldName()) {
        fieldName = ds->GetTypedValue(0.0f);
      }

      if (!fieldName.IsEmpty()) {
        unique_ptr<ImageLoader> loader = make_unique<HdCyclesVolumeLoader>(filename,
                                                                           fieldName.GetString());

        const SceneLock lock(renderParam);

        ImageParams params;
        params.frame = 0.0f;

        _handle = lock.scene->image_manager->add_image(std::move(loader), params, false);
      }
    }
  }
#else
  (void)sceneDelegate;
  (void)renderParam;
#endif

  *dirtyBits = DirtyBits::Clean;
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
