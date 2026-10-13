Cycles for Houdini 22
=====================

Blender's [Cycles](https://www.cycles-renderer.org) path tracer as a **Hydra render delegate for Houdini 22
(Solaris and husk)**, with NVIDIA GPU rendering (OptiX / CUDA), denoising, Cycles shader nodes laid out like
Blender, and Blender-style render settings.

This is a fork of [cjhosken/hdCycles](https://projects.blender.org/cjhosken/hdCycles) (itself based on Blender's
Cycles repository). The work for Houdini 22 lives on the `hdCycles-H22` branch.

> **Status: work in progress.** Daily lookdev with Cycles shaders, Karma/MaterialX materials, USD lights,
> volumes, primvars as shader attributes, AOVs, light groups, per-object and per-light settings, motion blur and
> OSL works, in the viewport, husk and TOPs; see [Known issues](#known-issues).

## Tested with

| | |
|---|---|
| Houdini | 22.0.429 (USD 26.05), Windows 11 |
| GPU | NVIDIA RTX 4090, OptiX 8.0 SDK, CUDA |
| Other plugins | Loads alongside Arnold (HtoA): Embree is linked statically without SYCL |

Linux and macOS builds have not been tested on Houdini 22.

## What works

| Area | State |
|---|---|
| Rendering in the Solaris viewport and with husk | Works |
| GPU rendering | OptiX and CUDA, chosen automatically (OptiX > CUDA > CPU), results match the CPU |
| Denoising | OpenImageDenoise (GPU or CPU) and OptiX |
| Shaders | All 96 user-facing Cycles shader nodes, as Houdini VOP nodes inside a **USD Cycles Material Builder** |
| Shader node UI | Blender 5.2 order, labels, menus, collapsible panels, hide rules and default values |
| MaterialX / Karma materials | Karma and MaterialX Material Builder materials are translated to Cycles nodes: Standard Surface, OpenPBR, UsdPreviewSurface and glTF PBR as the Principled BSDF, plus about 100 MaterialX nodes (images, math, noise, color, geometry, closures); node graph nodes from the MaterialX libraries are expanded. Matches Karma within a few percent in the tests |
| USD lights | Sphere, rect, disk, distant and dome lights match Karma within 1% (normalized and not) |
| Render settings | **Cycles Render Settings LOP**, a Cycles tab on the stock Render Settings LOP, and viewport display options, all laid out like Blender's render properties |
| Interactive edits | Moving/editing lights, objects, materials and settings updates the render. Motion blur, the shutter and primvar AOVs change without restarting the render. Texture files changed on disk reload on the next material edit or with **Reload Textures** in the viewport's renderer menu; UDIM (`<UDIM>`) textures |
| Viewport | Free view and looking through a camera |
| Stability | No crash on quit, renderer switch or device change |
| Units | Stages with `metersPerUnit` 1 and 0.01 |
| Volumes | VDB files, `.bgeo` volumes, and VDB or native Houdini volumes made in SOPs inside a LOP stage; volumes without a material show their density |
| Primvars | USD primvars are Cycles attributes for the Attribute, UV Map, Vertex Color, Normal Map/Tangent and USD Primvar Reader nodes, MaterialX geometry nodes and OSL `getattribute`: every interpolation on meshes (also subdivided), curves and points, constant primvars on volumes, float/double/half/int/bool scalars, vectors and arrays. Houdini's attribute names work too: `Cd`, `Alpha`, `uv`, `v`, `accel` find `displayColor`, `displayOpacity`, `st`, `velocities`, `accelerations` and the other way round. Face-varying normals (Houdini vertex `N`) are used. **Instance primvars:** per-point primvars of point instancers (Houdini Instancer LOP, SOP Import of packed primitives) and constant primvars on native instances reach the prototypes' shaders; the prototype's own primvar wins, then the innermost instancer, like Karma |
| Geometry | Meshes, curves, points, and implicit USD shapes (Sphere, Cube, Cone, Cylinder, Capsule, Plane) as smooth meshes |
| Motion blur | Object transforms, mesh deformation (time samples or velocities) and the camera, over the camera shutter (Motion Blur in the render settings) |
| AOVs | Pass toggles on the Cycles Render Settings LOP like Blender's View Layer Passes (Data, Light, Cryptomatte, Shader AOV, Light Groups) plus primvar AOVs, any Cycles pass as a render var, shader AOVs from the AOV Output node, Karma's common light path expressions and the Karma Render Settings LOP's AOVs mapped to Cycles passes, Cryptomatte with manifests, one image per pass (Split Render Product by Render Var), husk image filters. Every render var shows in the viewport's Render Outputs menu and in slap comps (live, and the Slap Comp Import COP) |
| Batch rendering | husk, the USD Render ROP and TOPs (USD Render Scene, USD Render Files, ROP Fetch). The Cycles Render Settings LOP is the stage's default render settings, like Karma's |
| Per-light settings | Max Bounces, Multiple Importance, Shadow Caustics, ray visibility and Light Group: Cycles tab of the Light LOP |
| Per-object settings | Ray visibility, shadow catcher, holdout, shadow caustics, shadow terminator, AO distance: Cycles tab of the Render Geometry Settings LOP |
| OSL | **Open Shading Language** render setting (CPU and OptiX). **Cycles Script** node for `.osl`/`.oso` files like Blender's Script node, and Houdini's own OSL shader nodes (OSL Generic Shader Builder) in Cycles materials |
| Tab menu | Cycles nodes grouped like Blender's Add menu (Input, Output, Shader, Displacement, Color, Texture, Utilities/Math, Utilities/Vector) |

## Known issues

### Rendering
- **AOVs:** Cycles has no light path expression engine: only Karma's common LPEs (diffuse, glossy reflection,
  glossy transmission and volume; combined, direct, indirect; the beauty, direct emission, albedo) map to Cycles
  passes, others render black. A Blender light pass and the LPE of the same light path (DiffDir and
  directdiffuse) cannot be rendered together. Karma's shadow/unshadowed LPEs and its geometric normal, facing
  ratio, camera-space position, element and motion vector AOVs have no Cycles pass and render black.
- **Motion blur:** meshes only for deformation (not curves, points or volumes), no instancer motion and no
  motion vector pass. LOP expressions give no motion: cache samples with the Motion Blur LOP, as for Karma.
- **Textures:** a texture file overwritten in place, with nothing changed in the scene, updates after
  **Reload Textures** (viewport renderer menu) or any material edit. Only the `<UDIM>` tile token is supported
  (not `<UVTILE>`), and `op:` paths to Copernicus images are not.
- **Primvars:** matrix, string and int64 primvars are not exported. Per-object settings (`cycles:object:*`) are not
  read per instance.
- **MaterialX limits:** noise nodes use Cycles' noise (same character, different pattern); the OpenPBR coat is
  approximated; BSDF layering is an add; matrix nodes, hex tiling, some compositing nodes and MaterialX volumes
  are not supported. Editing a parameter rebuilds the whole shader.
- **OSL:** on the GPU only OptiX supports it, and shaders cannot trace rays there: the Ambient Occlusion and
  Raycast nodes do nothing (Blender has the same limits). The first OptiX render compiles every OSL shader and
  can take minutes. Without a color attribute the Vertex Color node differs from SVM (also in Blender). Builds
  need an OSL built against Houdini's OpenImageIO (see [BUILDING.md](BUILDING.md)). `getattribute` in a Script
  node finds attributes named by a string constant or a parameter default, not names computed at run time.
- The Principled Hair BSDF needs a recheck of OptiX vs CPU output.

### Viewport
- Orbiting pivoted at the camera (like a first-person camera); fixed by declaring the depth AOV as OpenGL
  style depth, still to be confirmed in the GUI.

### UI
- Port locations are wrong on some shader nodes (the Principled BSDF inputs listed last is fixed).
- The Cycles Render Settings LOP needs UX touch-ups.
- Color Ramp and the curve nodes expose raw arrays instead of Houdini ramp parameters; angles are in radians.

### Missing features
- The Fast GI method menu is not exposed. Implicit shapes have no UVs.
- Viewport resolution divider (the viewport always renders at full resolution).
- HIP (AMD) and oneAPI (Intel) devices are disabled; NVIDIA and CPU only.

## Roadmap

In progress, roughly in this order:

1. Motion blur for curves, points, volumes and instancers; the motion vector pass; Fast GI method.
2. Texture coordinates on implicit shapes, the viewport resolution divider, Blender render defaults, curve shape.
3. Viewport responsiveness (start at low resolution, more frequent updates).
4. Ramps and curves as Houdini ramp parameters, angles in degrees, the Raycast node's attributes.
5. Confirm the orbit fix and the Reload Textures command in the GUI.

## Building

Full instructions: [BUILDING.md](BUILDING.md), section *Hydra Render Delegate for Houdini*. In short, on Windows
with Visual Studio 2022, CMake, Ninja, the CUDA Toolkit and the OptiX 8 SDK headers:

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

- Build Embree 4 as a static library without SYCL (`EMBREE_STATIC_LIB=ON`, `EMBREE_SYCL_SUPPORT=OFF`). The
  precompiled Embree needs the SYCL runtime, which conflicts with other plugins such as Arnold.
- The install step runs Houdini's `hython` to generate the shader nodes and render settings UI, so it needs a
  Houdini license.

## Using it in Houdini

1. Copy `install/houdini/packages/cycles.json` into `Documents/houdini22.0/packages` (adjust the path inside it if
   you move the install), or start Houdini with `PXR_PLUGINPATH_NAME=<install>/houdini/dso/usd_plugins`.
2. In Solaris, pick **Cycles** in the viewport's renderer menu.
3. Materials: in a Material Library LOP, create a **USD Cycles Material Builder** and build the shader from the
   Cycles nodes inside it.
4. Settings: use the **Cycles Render Settings** LOP (or the Cycles tab of the Render Settings LOP). Viewport-only
   settings are in the viewport's display options.
5. Batch rendering: `husk -R Cycles scene.usd`.
6. OSL: turn on **Open Shading Language** in the Device panel of the render settings (CPU or OptiX). Add a
   **Cycles Script** node (Tab menu Cycles > Script), pick an `.osl` or `.oso` file and press **Load Shader**: the
   node gets the shader's inputs and outputs (its type is saved in the hip file). Houdini's OSL Generic Shader
   Builder also works inside a Cycles material.
7. Motion blur: turn on **Motion Blur** in the render settings; the shutter is the camera's (Shutter Open/Close).
   Changing either no longer restarts the viewport render.
   Animated LOP parameters only have the current frame: cache samples with the **Motion Blur LOP**, as for Karma.
8. Attributes: an **Attribute** node (or USD Primvar Reader, MaterialX geometry nodes, OSL `getattribute`) reads
   any primvar by name: Houdini names work (`Cd`, `uv`, `Alpha`), as do per-point attributes of instancers.
9. Textures: after overwriting a texture file, use **Reload Textures** in the viewport's renderer menu.
10. Light groups: set a **Light Group** on the Cycles tab of the Light LOP (Render Geometry Settings LOP for
   emissive objects), and add the same name under **Light Groups** in the passes of the Cycles Render Settings
   LOP, which writes it as `Combined_<name>`.

**Device:** `AUTO` by default (best GPU, then CPU). Override with the `cycles:device` render setting
(`AUTO`, `OPTIX`, `CUDA`, `CPU`) or the `CYCLES_DEVICE` environment variable, e.g.
`CYCLES_DEVICE=CPU husk -R Cycles scene.usd`.

## Changes from the source repository

Compared with `main` of [cjhosken/hdCycles](https://projects.blender.org/cjhosken/hdCycles), this fork adds the
commits below (about 16,700 lines, most of them the generated `blender_node_layouts.json`). The Houdini shader
node generator is ported from that repository's `shaders` branch, by Christopher Hosken. Everything else is new.

### Build and packaging for Houdini 22

| Change | Why | Commit |
|---|---|---|
| Link `hdsi` when building against Houdini | Houdini 22's USD 26.05 libraries leave it out, so linking failed | `ca48d2cb4` |
| Use Houdini's OpenImageDenoise | Houdini loads its own OIDN 2.3 with the same file name as the precompiled 2.5 | `d7c53a3c0` |
| Fix MaterialXCore not being linked | The CMake block used an undefined variable | `d04a1e39e` |
| Fix the plugin resource path | It was one directory off, so the CUDA/OptiX kernels in `lib/` were not found | `5fae6c8aa` |
| Register the delegate in `UsdRenderers.json` | Without it Cycles is missing from the Solaris renderer menu and `husk -R Cycles` | `301887910` |
| Allow a static Embree without SYCL (`EMBREE_ROOT_DIR`), don't ship the SYCL runtime | The precompiled Embree needs `sycl8.dll`, which clashes with other plugins (Arnold) and stops Cycles loading | `4edfd12f9` |
| Generate shader nodes, render settings UI and the Cycles Render Settings LOP at install time with `hython` | New Houdini install step (`src/hydra/houdini/CMakeLists.txt`) | `3653a9084`, `16f63cd20`, `8abcbdb5f` |
| Allow OSL with an OSL built against Houdini's OpenImageIO (`OSL_ROOT_DIR`), static OSL, link USD `ar` | The precompiled OSL links Blender's OpenImageIO 3.1, which cannot be mixed with Houdini's 2.5 | `5476bf7e4`, `298aa4da1` |

### Rendering fixes

| Change | Commit |
|---|---|
| **Lights match Karma.** Dome lights were π times too bright and distant lights 4 times. Sphere light radius ignored stage units. Normalized lights used the area in meters instead of stage units (10000x too bright on a centimeter stage). | `49383cb65` |
| **Material sockets were not found** for Houdini shader nodes. Sockets now match by identifier or UI name, ignoring case, spaces, underscores and dots. Materials ending in an Emission or Volume output used to fall back to the default shader. | `fe9d20c86` |
| **Uninitialized values** (core Cycles): curves and ramps without points, and Set Normal without a direction, read garbage that differed between CPU and GPU. They now use Blender's defaults. | `64e207902` |
| **Crash on teardown** when quitting Houdini, switching renderer or changing device: OpenGL objects were deleted without a context. A fence leaked on every display update. | `576f64f74` |
| **Moving or rotating a light** did not update the render (the object was not tagged for update). | `379ba058f` |
| **Frozen, squashed viewport image when looking through a camera.** Houdini binds buffers the size of the viewer but sets the viewport to the camera gate. The delegate now renders at the buffer size and conforms the camera aspect. | `ac7c0f2fc` |
| **Volumes without a material rendered nothing** (they got the default surface shader). They now get Cycles' default volume shader, which shows the density like Karma. | `2b69b0a9a` |
| **Volumes made in SOPs failed to load** ("could not open file op:/stage/..."). Houdini passes these as `op:` paths, not files. The delegate now gets the grids from Houdini through `USD_SopVol` (`SOPgetVDBVolumePrimitive`), sharing them instead of copying. This needs the Houdini SDK headers and `libGEO`/`libUT`/`libCE`, so it is only built for Houdini (`houdini_volume.cpp`). | `0c125583d` |
| **Native Houdini volumes did not render** (`houdiniFieldAsset`, from SOPs or `.bgeo` files). They come from `SOPgetHoudiniVolumePrimitive` and are converted to OpenVDB grids with `GU_PrimVDB::buildFromPrimVolume` (links `libGT`/`libGU` too). | `4a3b4831e` |
| **Implicit USD shapes did not render** (Sphere, Cube, Cone, Cylinder, Capsule, Plane). A scene index plugin applies `HdsiImplicitSurfaceSceneIndex` for Cycles, like Storm and hdPrman. | `df65e282f` |
| **Volume shaders made the volume disappear.** Houdini's material builder authors every Cycles shader as the surface terminal, also volume shaders; a volume output on the surface terminal now goes to the volume input. | `a3710bf34` |
| **8 bit image textures rendered linear.** Cycles image nodes start as scene linear and an empty color space was looked up by name; Houdini's ACES OpenColorIO configuration has no `default_byte` role, which automatic detection relied on (core Cycles fix, plus the delegate treating an empty color space as automatic). | `9a09aff94`, `29ce63d07` |
| **The sample subset settings had no effect:** the session overwrote them. | `6c119c43c` |
| **Implicit spheres, cylinders, cones and capsules were faceted** 10-segment meshes (a sphere 8% small). A scene index meshes them with 64 segments and normals; a radius 1 sphere matches the analytic size within 0.2%. | `5ef0a65ff` |
| **Orbiting the viewport pivoted at the camera:** `UsdRenderers.json` declared the depth as camera distance while the viewport gets window depth. | `fa48caaca` |
| **Render vars were missing from the Houdini viewport** and its slap comps: Houdini only binds render vars with a valid default AOV descriptor, queried by prim name. | `d1a7ddf18` |
| **OSL attribute lookups with an empty name read garbage** (core Cycles): the hash of `""` is the id of attribute map terminators, so e.g. the Principled Volume's default Color Attribute made OSL mesh volumes render 0 on the CPU, 0.135 on OptiX instead of the SVM result, and change between runs (Blender 5.2 crashes in the same path). | `2f851d714` |
| **Scalar constant primvars rendered garbage** (how Houdini writes detail attributes), other numeric types rendered 0, and primvar values were copied without a size check (Houdini array attributes overflowed). | `c3b4eb536` |
| **USD Primvar Reader float output** into a float input read the constant alpha (1); normal/point/vector readers were dropped. | `45a5dafeb` |
| **UsdPreviewSurface emission and specular color were ignored** (mapped to Principled BSDF sockets that no longer exist). | `7071a7f7f` |
| **Vertex Color node read float and float2 attributes from the float3 array** (core Cycles, SVM): wrong values, CPU and OptiX differed. | `88c9261a2` |
| **Point instancer orientations and scales were applied in the wrong order** (also nested instancers): instances with orient or pscale, e.g. from the Instancer LOP or SOP Import, landed in the wrong place. | `dfcda7df8` |
| **Crash re-tessellating a subdivision mesh** when only its points changed. | `459f60fb3` |
| **Integer render vars (`primid`) were empty in husk:** outside the viewport ID buffers are floats (object index, 0 for none, like Karma's). | `6490beb06` |
| **The depth AOV was camera distance in meters** (1e10 for background) instead of Hydra's 0..1 window depth, which the Houdini viewport uses to composite guides. It is converted with the camera projection in the viewport (`houdini:viewport`); husk renders keep the distance. | `14fb1d2c8` |

### Render delegate features

| Change | Commit |
|---|---|
| `AUTO` device, the new default: OptiX, then CUDA, HIP, Metal, oneAPI, then CPU. Before, the default was CPU, which made the viewport slow. The device setting also accepts tokens, the form Houdini writes menu values in. | `a4e3c43b6` |
| Film and background render settings (`cycles:film:*`, `cycles:background:*`: exposure, pixel filter, transparent background). An explicit transparency setting now overrides the automatic one. | `ff9948176` |
| Changing the device or thread count restarts the delegate. | `16f63cd20` |
| **Open Shading Language** render setting (`cycles:shading_system`), a restart setting like the device. | `c454d7dd4` |
| **OSL script nodes:** `cycles_script` shaders with the shader file in `filepath`, and shaders implemented by an OSL source asset or source code (`info:OSL:sourceAsset`, read through the asset resolver when it is not a file, as Houdini provides its OSL shader nodes). | `4a9ce323c`, `298aa4da1` |
| `shader_defaults.json` in the resource directory overrides Cycles socket defaults per node type. Houdini only writes parameters that differ from the UI default, so the delegate must use the same defaults as the UI. | `2595a4dd6` |
| **AOVs:** render vars can name any Cycles pass, with a default format from the pass; other names are shader AOVs written by AOV Output nodes. Extra material terminals (e.g. `outputs:cycles:aov`) pull AOV Output nodes into the network. | `49debc368` |
| **Light path expression render vars:** Karma's common LPEs map to the Cycles light passes holding the same light paths, with the albedo like an LPE. AOVs of the same Cycles pass type share one pass (the kernel writes one buffer per type). | `87d538e45` |
| **Render vars named differently from their source** (husk names AOVs after the render var prim) found no pass and were black. | `b8d649ce2` |
| **Viewport color AOV readable:** with the OpenGL display driver the color buffer had no data, so Houdini's Slap Comp Import found no color. | `ca84bfc1c` |
| **Cryptomatte:** `CryptoObject00`, `CryptoMaterial00`, `CryptoAsset00`, ... render vars; objects, assets and materials named after their prims; the Cryptomatte metadata and manifests go into the image through the render stats. | `5515a46ac` |
| **MaterialX materials** (`materialx.cpp`): HdMtlx builds a MaterialX document per terminal (render contexts `mtlx` and `kma`), surfaces map to the Principled BSDF, about 100 standard/pbr library nodes map to Cycles nodes and node graph nodes are expanded from the MaterialX libraries. Built when USD provides `hdMtlx`. | `afc6f7453` |
| **Karma Render Settings LOP AOVs** (`ray:hitP`, `ray:hitPz`, `ray:hitN`, `ray:primid`, `st`, `AO`) map to Cycles passes. | `0c0a29a66` |
| **Light groups and per-light settings:** `lightgroup:<name>` render vars, `inputs:cycles:light:*` light attributes (max bounces, MIS, shadow caustics, ray visibility, light group). | `4b97f56e6` |
| **Primvar AOVs:** render vars with source type `primvar`, written by every shader; the list comes from the render settings prim, which Hydra syncs before materials. | `cd9120fca` |
| **Motion blur:** three evenly spaced motion steps over the camera shutter for transforms, mesh points (time samples or velocities) and the camera. | `acb24e055` |
| **Primvars as attributes:** all numeric types and scalar constants (`c3b4eb536`); all interpolations on points and curves, constant primvars on volumes as object attributes (`6d86055d6`); Houdini attribute names as aliases made only when a shader asks for them, the UV map as face-varying float2, `displayColor`/`Cd` as the vertex color at every interpolation, volume standard names, the constant color on every instance (`0c7b5de34`); face-varying normals as corner normals (`a77312a17`). | `c3b4eb536`, `6d86055d6`, `0c7b5de34`, `a77312a17` |
| **Instance primvars as object attributes:** point instancer and native instance primvars, composed through nested instancers, stored only for names the shaders read. | `f751e8b3f` |
| **Re-sync with scene indices:** with USD 26 scene indices Hydra never re-synced prims a delegate marked dirty. A pass-through scene index at the end of the chain (`resync_scene_index.cpp`) lets the delegate re-sync materials and prims, so motion blur and shutter changes, primvar AOVs and materials that start reading a primvar update without restarting the render. | `b5598ad66`, `b128d02b8`, `24e5ce736`, `014578e4b` |
| **Texture reload:** image files are identified by path, modification time and size (core Cycles), so a changed or late-created file loads on the next material rebuild; materials always rebuild on parameter changes; a **Reload Textures** renderer command; UDIM `<UDIM>` tiles for native and MaterialX images; asset parameters that do not resolve keep their authored path. | `78dd64030`, `43e22cfbd`, `a39c407be`, `74d5ebeba`, `0ad76428d` |
| **OSL script nodes request the attributes they read** (core Cycles): `getattribute` names found in the byte code (string constants and parameter defaults), so a Script node reading a primvar no longer needs another node requesting it. | `888453431` |
| **Per-object settings** from constant `cycles:object:<setting>` primvars: `visible_camera`/`_diffuse`/`_glossy`/`_transmission`/`_volume_scatter`/`_shadow`, `is_shadow_catcher`, `use_holdout`, shadow caustics, shadow terminator offsets, `ao_distance`. | `30aa47d9d` |

### Houdini user interface

| Change | Commit |
|---|---|
| **96 Cycles shader nodes** as VOP HDAs, plus a **USD Cycles Material Builder** shelf tool and icon (ported from the `shaders` branch, made to work on Windows) | `3653a9084` |
| **Shader nodes laid out like Blender 5.2:** Blender's order, labels, menus, collapsible panels, hide rules and default values. `blender_node_layouts.py` runs inside Blender and dumps how each node is drawn; `blender_layout.py` maps that onto the Cycles nodes. Parameter names are unchanged, so existing scenes still load. | `c9c2640e7` |
| **Slider ranges from Blender's node declarations,** read from the Blender source because the Python API only gives generic ranges. Fixes sliders from −10000 to 10000, and IOR inputs that were clamped below their defaults. | `702fd2b44` |
| **Cycles tab on the Render Settings LOP and in the viewport display options,** laid out like Blender's render properties: Sampling, Denoise, Path Guiding, Light Paths, Clamping, Caustics, Fast GI, Volumes, Film, Performance. Generated from the delegate's own settings, so defaults and menus always match. | `16f63cd20`, `de6ced211` |
| **Cycles Render Settings LOP:** one node that creates the render settings, product and var prims (`Products/renderproduct`, `Vars/beauty`) with every Cycles setting authored, like Karma's. | `8abcbdb5f`, `bb487a793` |
| **Tab menu sub-menus like Blender's Add menu,** parsed from Blender's `node_add_menu_shader.py`; nodes not in Blender's menu (Mix (Legacy), Scene Time, ...) in the closest sub-menu. | `f400ddc23`, `3d7bbdf1a` |
| **Inputs in the network editor in Blender's order:** inputs outside parameter folders were listed last ("Other"), e.g. Base Color on the Principled BSDF. | `b23302f41` |
| **More render settings:** Sample Subset, Path Guiding debug options, alpha threshold and mist pass. | `54e75e1eb` |
| **Render passes on the Cycles Render Settings LOP** like Blender's View Layer Passes (Data, Light, Cryptomatte, Shader AOV, with Blender's EXR names) plus Karma-named light path expressions. | `f7d18bc95`, `fc3d31d29` |
| **Image Filters** on the Cycles Render Settings LOP: husk slap comps from the Image Filter LOP. | `86ab0b082` |
| **AOV Output node in Cycles material builders:** a pass-through between the shader and the material output, so Houdini exports it. | `44ada1aaf` |
| **Cycles Script node** for OSL shaders, like Blender's Script node: Load Shader compiles the file with the installed `oslc` and turns the node into a type with the shader's ports, embedded in the hip file. | `afb0980ee` |
| **Default render settings:** the Cycles Render Settings LOP sets itself as the stage's render settings (on by default, like Karma's), so husk, ROPs and TOPs no longer render upstream Karma settings without the Cycles passes. | `bd4a5b6d2` |
| **Light settings and light groups:** Cycles tab on the Light LOP (`HdCyclesPlugin_Light.ds`), Light Group for objects, Light Groups on the Cycles Render Settings LOP. | `a9c2a8a5a` |
| **Split Render Product by Render Var** and **Primvar AOVs** on the Cycles Render Settings LOP. | `46182e5a1`, `4cca27455` |
| **Motion Blur** in the render settings (since `b128d02b8` motion blur and the camera shutter no longer restart the delegate). | `3f3fd20e3` |
| **Cycles tab on the Render Geometry Settings LOP** (`HdCyclesPlugin_Geometry.ds`) for the per-object settings, laid out like Blender's object Visibility panel. | `30aa47d9d` |

### Documentation

`BUILDING.md` gained a Houdini section: CUDA/OptiX options, static Embree, the Houdini license needed for install, and
where everything is in Houdini (`1ed26fc06`, `d8c47dbe5`, `be4a8103b`, `f6d08340b`). This README replaces the upstream
one (`38f5f148b`).

### New files

```
src/hydra/houdini/
  CMakeLists.txt                    install step: HDAs, render settings UI, LOP, package, shelf
  UsdRenderers.json                 registers "Cycles" with Houdini
  blender_node_layouts.json         Blender 5.2 shader node layouts (generated in Blender)
  packages/cycles.json.in           Houdini package file
  toolbar/cycles.shelf, icons/      USD Cycles Material Builder tool
  src/shaders.cpp                   dumps the Cycles shader node registry and render settings to JSON
  src/shader2hda.py                 builds the shader node HDAs
  src/blender_node_layouts.py       runs in Blender, dumps node layouts (+ ranges from Blender source)
  src/blender_layout.py             maps Blender layouts onto Cycles nodes
  src/rendersettings2ds.py          Cycles tabs (render settings, geometry) + viewport options (.ds)
  src/cyclesrendersettings2hda.py   Cycles Render Settings LOP
  src/script2hda.py, script_node.py Cycles Script node (OSL)
```

New Houdini-only source: `src/hydra/houdini_volume.{h,cpp}` (volumes from SOPs and native Houdini volumes).
New source built with `hdMtlx`: `src/hydra/materialx.{h,cpp}` (MaterialX translation).

New source: `src/hydra/resync_scene_index.{h,cpp}` (re-sync scene index).

Modified upstream files: `src/hydra/{camera,curves,display_driver,field,geometry,instancer,light,material,mesh,node_util,output_driver,plugin,pointcloud,render_delegate,render_pass,session,util,volume}`,
`src/hydra/resources/plugInfo.json`,
`src/hydra/CMakeLists.txt`, `src/cmake/external_libs.cmake`, `src/cmake/dependency_targets.cmake`,
`src/cmake/Modules/FindUSDHoudini.cmake`, `src/cmake/Modules/FindUSDPixar.cmake`,
`src/kernel/geom/attribute.h`, `src/kernel/svm/vertex_color.h`, `src/scene/{image_oiio,osl,shader_nodes}`, `src/util/colorspace.cpp`, `BUILDING.md`, `README.md`.

Full history: `git log main..hdCycles-H22`.

## License and credits

Cycles is licensed under the Apache License 2.0, see [LICENSE](LICENSE). Cycles is developed by the Blender
Foundation and contributors ([AUTHORS](AUTHORS)); the Houdini Hydra integration builds on the work of
[cjhosken/hdCycles](https://projects.blender.org/cjhosken/hdCycles). Houdini is a product of SideFX; this project
is not affiliated with SideFX or the Blender Foundation.
