# hdCycles for Houdini 22

hdCycles for Houdini 22 brings Blender's Cycles path tracer into SideFX Houdini as a Hydra render
delegate. Once it is installed, **Cycles** appears next to Karma in the renderer menu of the Solaris
viewport, and `husk`, the USD Render ROP and TOPs can render with it. The Cycles core follows
Blender's Cycles 5.3, so materials, render settings and results behave the way Blender users
expect, while scenes are built with Houdini's own USD tools (LOPs, Material Library, Light LOPs,
Render Settings LOPs).

This documentation is written for Houdini artists first. Developers who want to build the plugin
or change it will find the internals in [developers.md](developers.md).

## What you get

| Area | What is supported |
|---|---|
| Rendering | Solaris viewport (free view and through a camera), `husk`, USD Render ROP, TOPs (USD Render Scene, USD Render Files, ROP Fetch) |
| Devices | NVIDIA GPUs through OptiX or CUDA, chosen automatically, and the CPU. AMD (HIP) and Intel (oneAPI) GPUs are not enabled |
| Denoising | OpenImageDenoise (GPU or CPU) and the OptiX denoiser; DLSS Ray Reconstruction for the viewport in builds made with the DLSS SDK |
| Shaders | All user-facing Cycles shader nodes (99 nodes) as VOP nodes inside a **USD Cycles Material Builder**, laid out like Blender's shader editor |
| Other materials | Karma and MaterialX materials (Standard Surface, OpenPBR, glTF PBR, about 100 MaterialX nodes) and UsdPreviewSurface are translated to Cycles |
| OSL | Open Shading Language on the CPU and with OptiX, a **Cycles Script** node for `.osl`/`.oso` files, and Houdini's OSL Generic Shader Builder |
| Lights | USD sphere, rect, disk, distant and dome lights, matching Karma's intensities; light and shadow linking; per-light Cycles settings and light groups |
| Objects | Meshes (with subdivision), curves, points, volumes, point instancers and native instances, implicit USD shapes; per-object ray visibility, holdout, shadow catcher and more |
| Volumes | VDB files, `.bgeo` volumes, and VDB or native Houdini volumes made in SOPs |
| AOVs | Blender's view layer passes, Karma's common light path expressions, Cryptomatte, light groups, shader AOVs, primvar AOVs, split image per render var |
| Motion blur | Transforms, deforming meshes, curves, points, instancers, volumes with velocity fields and the camera |
| Render settings | A **Cycles Render Settings LOP**, a Cycles tab on the stock Render Settings LOP and Cycles options in the viewport's display options, all laid out like Blender's render properties |

## Pages

| Page | Contents |
|---|---|
| [install.md](install.md) | Installing the shareable zip, checking the install, other Cycles packages, uninstalling |
| [quickstart.md](quickstart.md) | Your first render in the viewport and with `husk`; choosing the device |
| [render-settings.md](render-settings.md) | Where render settings live, and a reference of every Cycles render setting |
| [materials.md](materials.md) | USD Cycles Material Builder, the shader nodes, material settings, UsdPreviewSurface and MaterialX/Karma translation |
| [osl.md](osl.md) | Open Shading Language: the render setting, the Cycles Script node, Houdini's OSL nodes |
| [lights.md](lights.md) | USD lights, intensities, per-light settings, light and shadow linking, dome lights |
| [world.md](world.md) | World and environment shaders |
| [objects.md](objects.md) | Per-object settings, primvars in shaders, instances, implicit shapes, curves, points, subdivision |
| [volumes.md](volumes.md) | VDB and Houdini volumes, volume shaders and volume settings |
| [aovs.md](aovs.md) | Render passes, light path expressions, Cryptomatte, light groups, shader and primvar AOVs |
| [motion-blur.md](motion-blur.md) | Motion blur, shutter, motion steps, velocity blur and the Vector pass |
| [rendering.md](rendering.md) | `husk`, TOPs, crop windows, denoisers, texture cache, performance tips |
| [troubleshooting.md](troubleshooting.md) | Known issues and fixes for common problems |
| [developers.md](developers.md) | Building from source, source layout, generated UI, adding settings and nodes, tests |

## Tested versions

| | |
|---|---|
| Houdini | 22.0.429 (USD 26.05) on Windows 11, 64-bit |
| GPU | NVIDIA RTX 4090 with OptiX 8.0 and CUDA 12.8 kernels |
| Other plugins | Loads alongside Arnold (HtoA) |
| Stage units | `metersPerUnit` 1 (meters) and 0.01 (centimeters) |

The shareable build is made for Houdini 22.0.429 on Windows only. Other Houdini 22.0 builds have not
been tested, and Houdini 21 or 22.5 will not load it. Linux and macOS builds have not been tested
with Houdini 22.

GPU rendering needs an NVIDIA RTX 20 series card or newer (the build contains kernels for the
RTX 20, 30, 40 and 50 series) and a driver new enough for OptiX 8 (R535 or newer). Other GPUs
render on the CPU.

## Credits and license

Cycles is developed by the Blender Foundation and contributors and is licensed under the Apache
License 2.0. The Houdini integration builds on the hdCycles work of Christopher Hosken
(`cjhosken/hdCycles`). Houdini is a product of SideFX. This project is not affiliated with or
endorsed by SideFX or the Blender Foundation.
