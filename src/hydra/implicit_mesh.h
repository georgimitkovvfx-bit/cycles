/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Ported from hdCycles-h22 (https://projects.blender.org/devanshutak25/hdCycles-h22, branch hdCycles-H22, commit
 * 3e0bcd4, Apache-2.0) as hdcycles-blender patch 0026: outside Blender (Houdini, usdview) nothing else turns USD's
 * implicit shapes into meshes for Cycles. FindUVPrimvar is h22's (util.cpp), copied in here. */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/filteringSceneIndex.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

TF_DECLARE_REF_PTRS(HdCyclesImplicitMeshSceneIndex);

/* Turns the implicit USD shapes (Sphere, Cylinder, Cone, Capsule, Cube, Plane) into meshes. The
 * curved shapes get enough segments to look smooth in a ray traced render: the 10 segments around
 * of HdsiImplicitSurfaceSceneIndex are made for a rasterizer that subdivides them. The cube gets
 * its own points per face, so it renders flat. All shapes get texture coordinates (st) in the
 * layout of Blender's primitives, unless they have authored ones. */
class HdCyclesImplicitMeshSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
 public:
  static HdCyclesImplicitMeshSceneIndexRefPtr New(const HdSceneIndexBaseRefPtr &inputSceneIndex);

  HdSceneIndexPrim GetPrim(const SdfPath &primPath) const override;
  SdfPathVector GetChildPrimPaths(const SdfPath &primPath) const override;

 protected:
  HdCyclesImplicitMeshSceneIndex(const HdSceneIndexBaseRefPtr &inputSceneIndex);

  void _PrimsAdded(const HdSceneIndexBase &sender,
                   const HdSceneIndexObserver::AddedPrimEntries &entries) override;
  void _PrimsRemoved(const HdSceneIndexBase &sender,
                     const HdSceneIndexObserver::RemovedPrimEntries &entries) override;
  void _PrimsDirtied(const HdSceneIndexBase &sender,
                     const HdSceneIndexObserver::DirtiedPrimEntries &entries) override;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
