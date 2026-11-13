/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/renderBuffer.h>

#include <mutex>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesRenderBuffer final : public PXR_NS::HdRenderBuffer {
 public:
  HdCyclesRenderBuffer(const PXR_NS::SdfPath &bprimId);
  ~HdCyclesRenderBuffer() override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

  bool Allocate(const PXR_NS::GfVec3i &dimensions,
                PXR_NS::HdFormat format,
                bool multiSampled) override;

  unsigned int GetWidth() const override
  {
    return _width;
  }

  unsigned int GetHeight() const override
  {
    return _height;
  }

  unsigned int GetDepth() const override
  {
    return 1u;
  }

  PXR_NS::HdFormat GetFormat() const override
  {
    return _format;
  }

  bool IsMultiSampled() const override
  {
    return false;
  }

  void *Map() override;

  void Unmap() override;

  bool IsMapped() const override;

  void Resolve() override;

  bool IsConverged() const override;

  void SetConverged(bool converged);

  bool IsResourceUsed() const;

  // Whether a resource (the display driver's texture) is set; Houdini reads the pixels otherwise
  bool HasResource() const
  {
    return _hasResource;
  }

  PXR_NS::VtValue GetResource(bool multiSampled = false) const override;

  void SetResource(const PXR_NS::VtValue &resource);

  void WritePixels(const float *pixels,
                   const PXR_NS::GfVec2i &offset,
                   const PXR_NS::GfVec2i &dims,
                   const int channels,
                   bool isId = false);

  /* The Cycles session thread writes the pixels while Hydra allocates and maps the buffer on
   * its own threads: hold this while reading the size and writing the data. */
  std::unique_lock<std::recursive_mutex> LockData()
  {
    return std::unique_lock(_dataMutex);
  }

 private:
  void _Deallocate() override;

  unsigned int _width = 0u;
  unsigned int _height = 0u;
  PXR_NS::HdFormat _format = PXR_NS::HdFormatInvalid;
  size_t _dataSize = 0;
  bool _floatIds = false;

  std::vector<uint8_t> _data;
  std::recursive_mutex _dataMutex;
  PXR_NS::VtValue _resource;
  mutable std::atomic_bool _resourceUsed = false;
  std::atomic_bool _hasResource = false;

  std::atomic_int _mapped = 0;
  std::atomic_bool _converged = false;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
