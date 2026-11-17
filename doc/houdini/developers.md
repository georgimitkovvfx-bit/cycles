# Developer guide

This page is for people who build hdCycles for Houdini from source or change it. It summarizes the
build, explains how the source is organized, how the Houdini user interface is generated, and how
to add a render setting or a shader node. The full build instructions are in
[BUILDING.md](../../BUILDING.md), section *Hydra Render Delegate for Houdini*.

## Building

### Requirements (Windows)

- Visual Studio 2022 (or its Build Tools), CMake and Ninja, Git with Git LFS, Python 3.
- Houdini 22.0.429. The plugin must be built against Houdini's own USD libraries (`HOUDINI_ROOT`).
- For NVIDIA GPU rendering: the CUDA Toolkit (12.x) and the OptiX SDK headers (8.0 or newer).
- A Houdini license for the install step (see below).

### Configure, build and install

```
.\make.bat update
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DHOUDINI_ROOT="C:/Program Files/Side Effects Software/Houdini 22.0.429" ^
  -DWITH_CYCLES_HYDRA_RENDER_DELEGATE=ON ^
  -DWITH_CYCLES_DEVICE_CUDA=ON -DWITH_CYCLES_DEVICE_OPTIX=ON ^
  -DWITH_CYCLES_DEVICE_HIP=OFF -DWITH_CYCLES_DEVICE_ONEAPI=OFF ^
  -DWITH_CYCLES_STANDALONE_GUI=OFF ^
  -DWITH_CYCLES_CUDA_BINARIES=ON ^
  -DOPTIX_ROOT_DIR="<path to OptiX SDK>" ^
  -DCYCLES_CUDA_BINARIES_ARCH="sm_75;sm_86;sm_89;sm_120;compute_75" ^
  -DEMBREE_ROOT_DIR="<path to static Embree without SYCL>"
cmake --build build
cmake --install build
```

`make.bat update` downloads Blender's precompiled libraries into `lib/`. Where Houdini ships a
library (OpenImageIO, OpenColorIO, OpenVDB, OpenImageDenoise, TBB, USD, MaterialX, Python), the
build uses Houdini's copy instead, so that the plugin does not load a second version into
Houdini's process. A full clean build takes about ten minutes, most of it compiling the CUDA and
OptiX kernels.

### Embree

The precompiled Embree depends on the SYCL runtime, which clashes with other plugins that ship
their own (for example Arnold) and stops Cycles from loading. Build Embree 4 as a static library
without SYCL (`EMBREE_STATIC_LIB=ON`, `EMBREE_SYCL_SUPPORT=OFF`) and pass it with
`EMBREE_ROOT_DIR`. Never ship `embree4.dll` or `sycl8.dll` with the plugin.

### OSL

OSL is off for Houdini builds unless an OSL built against Houdini's OpenImageIO is given: the
precompiled OSL links Blender's OpenImageIO, which cannot be mixed with Houdini's. Build OSL 1.15
as static libraries with `BUILD_SHARED_LIBS=OFF`, `OSL_USE_OPTIX=ON` and `USE_LLVM_BITCODE=ON`,
against Houdini's OpenImageIO and Imath, then configure with `WITH_CYCLES_OSL=ON`, `OSL_ROOT_DIR`,
`OpenImageIO_DIR` and `Imath_DIR`. Two small changes to OSL are needed for Houdini's OpenImageIO
2.5; [BUILDING.md](../../BUILDING.md) lists them. `oslc` is installed with the plugin for the
Cycles Script node.

### Optional features

| Option | Purpose |
|---|---|
| `WITH_CYCLES_DLSS=ON`, `DLSS_SDK_ROOT` | DLSS viewport denoiser; needs the headers of the NVIDIA DLSS SDK 310.9.1 (only its `include` folder). The DLSS runtime comes with the NVIDIA driver (620 or newer) |
| `WITH_CYCLES_PATH_GUIDING`, `openpgl_DIR` | Path guiding. The precompiled OpenPGL is static and fails to link next to a static Embree; either turn path guiding off or build OpenPGL as a shared library against Houdini's TBB. Guided renders are currently biased, so the shareable build has it off |

### The install step needs Houdini

`cmake --install` runs Houdini's `hython` to generate the shader nodes, the render settings UI,
the Cycles Render Settings LOP and the Cycles Script node (see
[Generated user interface](#generated-user-interface)). This needs a Houdini license, and the
generators report failures only as CMake warnings: check the install log for
`shader2hda.py failed`, `rendersettings2ds.py failed` and similar lines. The install also fails
while Houdini (or `husk`/`hython`) has `hdCycles.dll` loaded.

### Using a build

The install folder contains `houdini/packages/cycles.json` with the absolute install path filled
in. Copy it to your Houdini packages folder, or for a quick test start Houdini with
`PXR_PLUGINPATH_NAME=<install folder>/houdini/dso/usd_plugins` (this loads only the render
delegate, without the HDAs and UI). If you move the install folder, edit the path in
`cycles.json`.

Install layout:

```
<install folder>/
  lib/                          CUDA cubins and OptiX PTX kernels (*.zst)
  source/                       kernel sources
  shader/                       OSL headers and compiled shaders of the Cycles nodes
  shader_defaults.json          Blender defaults of the shader nodes (written by shader2hda.py)
  license/
  houdini/                      = $CYCLES_ROOT, added to HOUDINI_PATH
    bin/                        runtime DLLs and oslc
    dso/usd/hdCycles.dll        the render delegate
    dso/usd_plugins/            plugInfo.json files (resource path = the install folder)
    otls/                       generated HDAs
    soho/parameters/            generated HdCyclesPlugin_*.ds render settings tabs
    husdplugins/                Cycles shader translator
    toolbar/cycles.shelf        USD Cycles Material Builder tool
    packages/cycles.json        Houdini package file
    UsdRenderers.json           renderer registration
```

## Source architecture

The repository is Blender's standalone Cycles repository with the Hydra render delegate in
`src/hydra`. Houdini-specific files are in `src/hydra/houdini`.

### `src/hydra`: the render delegate

| File | Role |
|---|---|
| `plugin.cpp` | `HdRendererPlugin` entry point; registers the scene index plugins that run for the renderer "Cycles" (implicit shapes, light linking, re-sync) |
| `render_delegate.cpp` | `HdCyclesDelegate`: supported prim types, render settings (`SetRenderSetting`, `GetRenderSetting`), device selection, session settings, AOV descriptors, Cryptomatte metadata in the render stats, the Reload Textures command |
| `session.cpp` | `HdCyclesSession` (the Hydra render param around `ccl::Session`): AOV bindings to Cycles passes, light path expression and Karma AOV mapping, Cryptomatte, light groups, primvar AOVs, motion times, background and transparency, light linking sets |
| `render_pass.cpp` | Drives rendering: camera, resolution, crop window, pixel aspect, render tags, convergence |
| `render_buffer.cpp`, `output_driver.cpp`, `display_driver.cpp` | AOV buffers; writing Cycles passes into them; the OpenGL display driver for the viewport |
| `render_settings.cpp` | Render settings Bprim: primvar AOVs and the motion pass from the render settings prim, synced before materials and geometry |
| `camera.cpp` | USD camera to Cycles camera: projection, depth of field, shutter, motion |
| `geometry.inl`, `mesh.cpp`, `curves.cpp`, `pointcloud.cpp`, `volume.cpp`, `instancer.cpp` | Rprims: transforms and motion, primvars as attributes, per-object settings (`cycles:object:*`), instancing and instance primvars |
| `field.cpp`, `houdini_volume.cpp` | Volume fields: OpenVDB files, and (Houdini builds only) SOP volumes and native Houdini volumes through Houdini's libraries |
| `implicit_mesh.cpp` | Scene index that meshes implicit USD shapes, with normals and texture coordinates |
| `light.cpp` | USD lights, Karma-matching intensities, per-light settings (`inputs:cycles:light:*`) |
| `material.cpp` | Builds Cycles shader graphs from Hydra material networks: Cycles nodes, UsdPreviewSurface, OSL script and source asset nodes, AOV Output pass-through, material settings (`config:cycles:*`), UDIMs, shader defaults |
| `materialx.cpp` | MaterialX translation (built when USD provides `hdMtlx`) |
| `resync_scene_index.cpp` | Pass-through scene index that lets the delegate re-sync prims it marks dirty, so motion, primvar AOV and material changes apply without a restart |
| `node_util.cpp`, `util.cpp` | Value conversion between USD and Cycles sockets; scene index and primvar helpers |
| `file_reader.cpp` | Loads a USD file into a Cycles session for the standalone `cycles` executable |

### `src/hydra/houdini`: the Houdini integration

| File | Role |
|---|---|
| `CMakeLists.txt` | Installs the package file, `UsdRenderers.json`, icons, shelf and shader translator; builds `shaders_node_dump`; runs the generators with `hython` at install time |
| `UsdRenderers.json` | Registers `HdCyclesPlugin` as "Cycles": menu priority, render purpose, OpenGL depth style, default `husk` output and the settings that restart the delegate |
| `packages/cycles.json.in` | Package file template |
| `toolbar/cycles.shelf` | The USD Cycles Material Builder tool, including its Settings tab |
| `husdplugins/shadertranslators/cycles.py` | Shader translator for render mask `cycles`: bakes ramp parameters into Cycles' 256-entry tables, writes degree parameters in radians |
| `src/shaders.cpp` | `shaders_node_dump`: writes the Cycles shader node registry to `shader_nodes.json` and the Integrator, Film and Background sockets to `render_settings.json` |
| `src/shader2hda.py` | One VOP HDA per shader node, with Blender's layout; writes `shader_defaults.json` |
| `src/blender_node_layouts.py`, `blender_node_layouts.json` | Run inside Blender to dump how Blender draws each shader node (labels, order, panels, menus, defaults, ranges, hide rules); the JSON is the committed result |
| `src/blender_layout.py` | Maps Blender's layouts onto the Cycles nodes (aliases, variants, Tab menus, ramps, angles, Raycast attributes); plain Python, testable without Houdini |
| `src/rendersettings2ds.py` | `LAYOUT`, `GEOMETRY_LAYOUT` and `LIGHT_LAYOUT`; writes the `HdCyclesPlugin_Global/Viewport/Geometry/Light.ds` dialog scripts |
| `src/cyclesrendersettings2hda.py` | Builds the Cycles Render Settings LOP from the same layout, plus the Passes folder |
| `src/script2hda.py`, `src/script_node.py` | The Cycles Script node and its Load Shader logic |

## Generated user interface

Nothing in Houdini's UI for Cycles is drawn by hand; it is generated at install time so that the
UI defaults always equal what the delegate uses for unauthored values (Houdini only writes values
that differ from the default):

1. The build runs `shaders_node_dump`, which iterates Cycles' node type registry and writes
   `shader_nodes.json` and `render_settings.json` (socket names, types, defaults, menus, ranges)
   into the build folder.
2. `shader2hda.py` builds one HDA per user-facing shader node (`convert_*` and the output node are
   skipped) into `houdini/otls`, laid out with `blender_node_layouts.json`, and writes
   `shader_defaults.json`, the Blender defaults that differ from Cycles' socket defaults. The
   delegate loads this file from its resource folder and applies it to every Cycles node it
   creates.
3. `rendersettings2ds.py` writes the render settings tabs to `houdini/soho/parameters`. Houdini
   loads `<plugin name>_Global.ds` (Render Settings LOP tab), `_Viewport.ds` (display options),
   `_Geometry.ds` (Render Geometry Settings LOP) and `_Light.ds` (Light LOP) for each render
   delegate. Each `cycles:...` parameter name is encoded with `hou.text.encodeParm`, and the
   Global, Geometry and Light tabs get Edit Properties style `_control` parameters.
4. `cyclesrendersettings2hda.py` builds the Cycles Render Settings LOP: a subnet of a Render Product,
   Render Var, Python Script (pass render vars), Render Settings LOP (every Cycles parameter set and
   referencing the top-level parameter), a split-products script and a Configure Layer LOP that
   sets the default render settings prim. It renders a self-test of the authored prims.
5. `script2hda.py` builds the Cycles Script node.

## Adding a render setting

**A socket of Cycles' Integrator, Film or Background node** needs no C++ change: the delegate
forwards every `cycles:integrator:<socket>`, `cycles:film:<socket>` and
`cycles:background:<socket>` setting to that socket. Add an `S(name, label, help, ...)` entry to
the right panel of `LAYOUT` in `rendersettings2ds.py`:

- the type, default and menu come from `render_settings.json`, so do not copy them;
- `range=(min, max, min_strict, max_strict)` sets the slider range;
- `viewport=True` also puts it in the viewport display options;
- `hidewhen`/`disablewhen` take Houdini conditions with `cycles:...` names, which the writer
  encodes;
- better menu labels go into `MENU_ITEMS`.

The Cycles Render Settings LOP and all `.ds` files pick the new entry up on the next install.

**A setting that is not a socket** (a session or scene parameter, or a setting the delegate maps
itself):

1. Add its token to `HD_CYCLES_RENDER_SETTINGS_TOKENS` in `render_delegate.h`.
2. Handle it in `HdCyclesDelegate::SetRenderSetting` (and return it in `GetRenderSetting`). An
   empty value means the setting was removed: go back to the default.
3. If it can only be applied when the session is created, read it in `GetSessionParams` or
   `GetSceneParams`, add it to `IsSessionSetting`, and add it to `restartrendersettings` in
   `UsdRenderers.json` so Houdini restarts the delegate when it changes.
4. Describe its type and default in `SESSION_SETTINGS` of `rendersettings2ds.py`, and add it to
   `LAYOUT`.

**Per-object or per-light settings** follow the same pattern with `GEOMETRY_SETTINGS` /
`GEOMETRY_LAYOUT` (read in `UpdateObjectSettings` in `geometry.inl`) and `LIGHT_SETTINGS` /
`LIGHT_LAYOUT` (read in `light.cpp`).

## Adding a shader node

New Cycles shader nodes appear automatically: `shaders_node_dump` finds every registered shader
node type, and `shader2hda.py` makes an HDA for it. To give it Blender's layout:

1. Update `blender_node_layouts.json` by running `src/blender_node_layouts.py` inside the matching
   Blender version, with the Blender source of the same commit for the socket ranges (see the
   script and [BUILDING.md](../../BUILDING.md)).
2. If the Cycles node does not match a Blender node one to one, add it to the tables in
   `blender_layout.py`: `ALIASES` (socket names), `VARIANT_MENUS` (Tab menu of nodes split by data
   type), `RAMPS` and `ANGLE_SOCKETS` (ramp and degree parameters), `LEADING_SOCKETS`,
   `INTERNAL_SOCKETS`.
3. If the node needs special handling in the delegate (outputs that depend on parameters, like
   the Raycast node, or a pass-through like AOV Output), add it to `material.cpp`.

Parameter names must stay the Cycles socket names: the delegate matches sockets by identifier or
UI name, ignoring case, spaces, underscores and dots, and existing scenes rely on the names.

## Testing

Every change is verified by rendering. The regression suites are kept outside the repository (they
contain machine-specific paths and reference renders); they follow these patterns:

- **husk renders of small USD scenes** checked numerically: each shader node on OptiX and the CPU
  with two seeds (noise, deviation and bias between devices), USD lights against Karma within 1%,
  render settings, AOVs and Cryptomatte IDs, motion blur, primvars, instancing, volumes,
  MaterialX and UsdPreviewSurface against Karma, crop windows, texture cache, light linking.
- **Interactive updates** with `husk` frame ranges: a multi-frame `husk` render runs in one
  delegate, like a viewport session, so a change between frames must give the same image as a
  fresh render of that frame.
- **hython tests** of the generated UI: HDA layouts and Tab menus, the parameters the LOPs author,
  ramps and angles, old hip files.
- **GUI tests** that start Houdini with private preferences and capture viewport flipbooks (never
  screen grabs), comparing Cycles with Karma, and a stress suite that compares the live viewport
  with a freshly restarted renderer after scripted edits.
- `check_install.py` in the shareable package is the minimal end-to-end test of an install.

When debugging a `husk` problem, render the same scene with Karma first to rule out a broken test
scene.
