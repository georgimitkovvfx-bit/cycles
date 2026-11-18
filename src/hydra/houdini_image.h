/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <string>

namespace HdCycles {

/* Image file formats only Houdini reads (.rat) as a file Cycles can read: a TIFF copy with the
 * same data type in a cache directory, converted with Houdini's image library the first time
 * (again when the file changes). A <UDIM> pattern converts every tile and gives the pattern of
 * the copies. Other file names, and files that fail to convert, are returned as they are. */
std::string ResolveHoudiniImage(const std::string &filename);

}  // namespace HdCycles
