/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "session/output_driver.h"

#include <pxr/base/tf/token.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesOutputDriver final : public CCL_NS::OutputDriver {
 public:
  HdCyclesOutputDriver(HdCyclesSession *renderParam);

 private:
  void write_render_tile(const Tile &tile) override;
  bool update_render_tile(const Tile &tile) override;

  void update_render_buffers(const Tile &tile, bool final);
  // The viewport depth as window depth
  void convert_units(const PXR_NS::TfToken &aovName,
                     float *data,
                     size_t num_pixels,
                     size_t channels) const;

  HdCyclesSession *const _renderParam;
  // Time of the last progressive viewport update of the AOVs that are not displayed
  double _aovsUpdateTime = 0.0;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
