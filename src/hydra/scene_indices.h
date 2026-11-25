/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Registers the Cycles scene indices with HdSceneIndexPluginRegistry for the renderer "Cycles"
 * (all but the implicit surfaces, which plugin.cpp registers as a plugin class), once per
 * process. Call before the render index is created. */
void HdCyclesRegisterSceneIndices();

HDCYCLES_NAMESPACE_CLOSE_SCOPE
