# Rendering

This page covers final renders with `husk`, ROPs and TOPs, image framing, the denoisers, the
texture cache, and tips for speed and viewport responsiveness.

## husk

`husk` is Houdini's command-line USD renderer. Render a USD file with Cycles like this:

```
husk -R Cycles scene.usd
```

`-R Cycles` (or `--renderer HdCyclesPlugin`) picks the Cycles delegate; `husk --list-renderers`
lists it. The usual `husk` options apply, for example:

| Option | Purpose |
|---|---|
| `-o <file>` | Output image, instead of the render product's file name |
| `-f <frame>`, `-n <count>` | First frame and number of frames |
| `-s <prim path>` | Render settings prim to use, instead of the stage's default |

Run `husk --help` for the full list.

Without an `-s` option, `husk` uses the stage's default render settings prim; the Cycles Render
Settings LOP sets itself as the default (see [render-settings.md](render-settings.md)). With no
render settings and no `-o`, the image is written to `cycles.exr`.

Environment variables that help with `husk`:

| Variable | Effect |
|---|---|
| `CYCLES_DEVICE=CPU` (or `OPTIX`, `CUDA`, `AUTO`) | Device to use when the render settings do not set one |
| `CYCLES_LOGGING=1` | Print Cycles' log; the line `Path tracing on: ...` names the device used |
| `CYCLES_LOGGING_LEVEL=debug` | More detailed log |

**Threads:** the **Threads** setting (`cycles:threads`, Performance panel) limits the CPU threads
of a `husk` render; 0 uses all cores.

**Render passes:** `husk --pass <name>` renders a USD render pass with its prune, visibility and
matte collections, see [aovs.md](aovs.md#render-passes).

**Farm machines:**

- **GPU Devices** (Device panel) picks the GPUs of a husk job, for example one job per GPU on a
  multi-GPU machine; **Use CPU Too** adds the CPU to a husk render. See
  [render-settings.md](render-settings.md#device).
- **Abort Without GPU** and **Abort on Missing Textures** (Performance > Farm) fail the job with
  exit code 1 instead of rendering on the CPU or with magenta textures. The reason is printed in
  the husk log. See [render-settings.md](render-settings.md#performance--farm).
- Scenes set up for Karma keep its samples, bounce limits, denoiser and aborts, see
  [render-settings.md](render-settings.md#karma-render-settings).

**Large images** are rendered in tiles, like in Blender: Cycles writes finished tiles to temporary
files in the system's temporary folder and assembles the image at the end. Make sure that folder
has space for very large renders.

## USD Render ROP and TOPs

The **USD Render ROP** renders with Cycles when its renderer is set to Cycles. It writes the stage
and runs `husk`, so everything above applies.

In TOPs, Cycles works with:

- **USD Render Scene**, rendering a LOP node,
- **USD Render Files**, rendering USD files,
- **ROP Fetch** pointing at a USD Render ROP.

Work items report their output images like Karma's. Render settings, passes and frame ranges come
from the Cycles Render Settings LOP in the stage.

Avoid running two GPU renders at the same time on one GPU (for example two `husk` processes, or a
`husk` render next to the viewport): one of them can stall for minutes. Limit the TOP scheduler to
one GPU render at a time per GPU.

## Framing: resolution, crop and pixel aspect

The resolution, pixel aspect ratio and camera come from the render settings (Camera, Resolution and
Pixel Aspect Ratio on the Cycles Render Settings LOP).

- **Crop windows** (`dataWindowNDC` on the render settings or product) render only the region
  inside the window, framed exactly as in the full image, like Karma.
- **Non-square pixels** (`pixelAspectRatio`) match Karma.
- **Aspect ratio conform policy** (`aspectRatioConformPolicy`: expand aperture, crop aperture,
  adjust aperture width or height, adjust pixel aspect) is applied when the camera's aperture and
  the image have different aspect ratios.
- **Depth of field** comes from the camera's F-Stop and Focus Distance (F-Stop 0 turns it off).
  Perspective and orthographic cameras are supported; panoramic cameras are not.

## Denoising

Turn on **Denoise** in Sampling > Render > Denoise, or with the denoise button on the viewport
toolbar. Denoising is off by default (Blender Defaults on the Cycles Render Settings LOP turns it
on).

| Denoiser | Runs on | Notes |
|---|---|---|
| OpenImageDenoise (default) | GPU or CPU | High quality for final renders. **Quality** (High, Balanced, Fast) trades quality for speed. **Use GPU** runs it on the GPU when available |
| OptiX | NVIDIA GPU | Fast, good for the viewport |
| DLSS | NVIDIA RTX GPU, driver 620 or newer | DLSS Ray Reconstruction, viewport only |

The other denoise settings:

- **Prefilter** (Accurate, Fast, None) filters the albedo and normal guiding passes before they are
  used. Keep Accurate unless the guiding passes are free of noise.
- **Start Sample** is the sample at which the viewport starts denoising.

**DLSS** only denoises viewport renders. Final renders, and viewports on GPUs or drivers without
DLSS, use OpenImageDenoise (final) or the fastest available denoiser (viewport) instead, and a
warning says which one. DLSS denoises every sample at full resolution; upscaling modes are not
available. The shareable zip build is made without the DLSS SDK, so there DLSS always falls back.

## Texture cache

Cycles can render from tiled, mipmapped `.tx` textures, loading only the tiles and mip levels a
render needs. This saves a lot of memory with large texture sets. The settings are in Performance >
Texture Cache:

| Setting | Effect |
|---|---|
| Texture Cache (on by default) | Use `.tx` files and other tiled, mipmapped images directly. For other images, use the cache file in the Cache Directory, or in a `blender_tx` folder next to the image, when one exists |
| Auto Generate (off by default) | Create missing cache files while rendering |
| Cache Directory | Where cache files are kept: an absolute folder, or a folder relative to each image. Empty uses a `blender_tx` folder next to each image |

**Simplify > Texture Size Limit** scales down every texture larger than the chosen size (128 to
8192 pixels), for previews or when GPU memory runs out.

All texture cache settings and the size limit restart the render when changed.

**Changed texture files** are reloaded on the next material edit, or immediately with **Reload
Textures** in the viewport's renderer menu (for example after overwriting a texture from another
application).

## Render stats

The viewport's render stats overlay (and husk's render stats) shows the Cycles version, the
samples done and the total, the estimated remaining time and the device used (the GPU name, or
the device names joined by ` + ` with Use CPU Too). With Sample Subset on, the total is the
subset's sample count. The husk stats (`cyclesSamples`, `cyclesDevice`) were checked; the viewport
overlay is *untested* in the GUI.

## Performance and responsiveness

**Viewport:**

- Keep **Start at Low Resolution** on (Performance > Viewport): each change first renders at a
  lower resolution, which keeps tumbling and editing responsive.
- The viewport image updates about every 0.2 seconds. Passes that are not displayed refresh about
  0.25 seconds after a change and then once per second.
- Use the OptiX denoiser for a clean viewport image early, with a low **Start Sample**.
- Lower **Max Samples** and the bounce limits in the viewport's display options for lookdev, or use
  **Fast GI Approximation**.
- With a dome light, the viewport image takes a slower path and the low resolution start is not
  shown.

**Final renders:**

- **Noise Threshold** (adaptive sampling, on by default) stops sampling pixels that are clean
  enough; lower the threshold for cleaner images, raise it for speed.
- **Light Tree** (on by default) is the most efficient way to sample many lights.
- Clamp **Indirect Light** (default 10) to reduce fireflies, at the cost of some energy.
- Turn **Filter Glossy** up to reduce caustic noise, or turn off **Reflective** and **Refractive**
  caustics where they are not needed.
- Use the texture cache and Texture Size Limit when memory is tight.

**First renders:** the first GPU render after installing or after a driver update loads the GPU
kernels, and the first OptiX render with Open Shading Language compiles every OSL shader; both can
take a while. Later renders start quickly.
