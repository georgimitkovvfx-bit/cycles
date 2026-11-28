/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/usd/sdr/declare.h>
#include <pxr/usd/sdr/discoveryPlugin.h>
#include <pxr/usd/sdr/parserPlugin.h>

PXR_NAMESPACE_OPEN_SCOPE

/* Publishes Cycles' shader nodes in USD's shader registry (Sdr), as the identifiers the delegate's material
 * translation reads: `cycles_<node>` (USD Nodes' `info:id`, HDCYCLES.md).
 *
 * After upstream Cycles' pull requests #114 and #120 (the discovery and parser plugins reading Cycles' node
 * registry at run time, a library of its own, `sdrCycles`, so that hosts list the nodes without loading the
 * renderer), with what our hosts need: each input's USD type as USD Nodes and the delegate write it (`bool`,
 * `token` for menus and closures, `float2`, `asset` for file names), defaults of that type, and the node's label and
 * menu category (the Houdini package's Tab menu, Blender's Add menu: resources/sdr/categories.json). Cycles'
 * internal conversion nodes (`convert_*`) are not published. */
class SdrCyclesDiscoveryPlugin final : public SdrDiscoveryPlugin {
 public:
  SdrCyclesDiscoveryPlugin();
  ~SdrCyclesDiscoveryPlugin() override;

  SdrShaderNodeDiscoveryResultVec DiscoverShaderNodes(const Context &context) override;

  const SdrStringVec &GetSearchURIs() const override;
};

class SdrCyclesParserPlugin final : public SdrParserPlugin {
 public:
  SdrCyclesParserPlugin();
  ~SdrCyclesParserPlugin() override;

  SdrShaderNodeUniquePtr ParseShaderNode(const SdrShaderNodeDiscoveryResult &discoveryResult) override;

  const SdrTokenVec &GetDiscoveryTypes() const override;

  const TfToken &GetSourceType() const override;
};

PXR_NAMESPACE_CLOSE_SCOPE
