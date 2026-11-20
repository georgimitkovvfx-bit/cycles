# Quick start

This page walks through a first Cycles render: in the Solaris viewport, then to an image file with
the USD Render ROP or `husk`. It assumes the plugin is installed and `check_install.py` passes (see
[install.md](install.md)).

## 1. Render in the viewport

1. Open a Solaris scene, or build a small one in `/stage`: for example a **Sphere LOP**, a
   **Grid LOP** as a floor, a **Camera LOP** and a **Light LOP** (or a **Dome Light LOP**).
2. In the viewport, open the renderer menu (the menu that normally says *Houdini GL* or
   *Karma CPU*) and choose **Cycles**.

Cycles starts rendering right away, from the free view or from the camera you look through. The
first samples after each change are rendered at a lower resolution and then refined, like in
Blender's viewport (Render Settings > Performance > Viewport > **Start at Low Resolution**). The
very first GPU render after an install or a driver update can take a while longer while the GPU
kernels load.

Edits to lights, objects, materials and most render settings update the render without restarting
it. A few settings are fixed when the render starts (the device, Open Shading Language, curve
shape, thread count, the start at low resolution option and the texture cache settings); changing
them makes Houdini restart the viewport render. See [render-settings.md](render-settings.md).

## 2. Add a Cycles material

1. Add a **Material Library LOP** after your geometry and dive inside it.
2. Press Tab and choose **Cycles > USD Cycles Material Builder**.
3. Dive into the builder. Inside, the Tab menu has a **Cycles** sub-menu with every Cycles shader
   node grouped like Blender's Add menu (Shader, Texture, Color, ...). Create a
   **Cycles Principled BSDF** and connect it to the surface input of the builder's output node.
4. Back at the Material Library LOP, assign the material to your geometry (the Geometry Path
   parameter of the material, or an **Assign Material LOP**).

Karma and MaterialX materials and UsdPreviewSurface materials also render with Cycles, so existing
lookdev keeps working. See [materials.md](materials.md).

## 3. Set up the render settings

Add a **Cycles Render Settings LOP** (in the LOP Tab menu under **Cycles** and under **Render**) at
the end of your node chain. Like Karma's render settings LOP, it creates the render settings,
render product and render var prims in one node, with the output picture, camera and resolution
at the top and every Cycles setting below, laid out like Blender's render properties.

It makes itself the stage's default render settings, so `husk`, the USD Render ROP, TOPs and the
viewport pick it up without further setup. **Cycles (Setup)** in the Tab menu (under **Cycles** and
**Rendering**) adds the Cycles Render Settings LOP with a USD Render ROP below it that renders with
Cycles, like Houdini's Karma (Setup) tool. The **Blender Defaults** button sets Blender's values
for final renders (4096 samples, denoising, Blackman-Harris filter, Blender's bounce limits and so
on), if you want results that match a default Blender scene.

The most common settings to change first:

| Setting | Where | Purpose |
|---|---|---|
| Max Samples | Sampling > Render | Samples per pixel (default 1024) |
| Noise Threshold | Sampling > Render | Adaptive sampling: stops pixels that are clean enough (on by default) |
| Denoise | Sampling > Render > Denoise | Denoise the image (off by default) |
| Max Bounces | Light Paths | Bounce limits per ray type (7 each by default) |
| Motion Blur | Motion Blur | Motion blur over the camera's shutter (off by default) |
| Passes | Passes | Extra AOVs, Cryptomatte and light groups, see [aovs.md](aovs.md) |

## 4. Render to disk

### USD Render ROP

Add a **USD Render ROP** (in `/out`, or a **USD Render ROP LOP** in the stage), point it at your LOP
node, set its renderer to **Cycles**, and render. The image goes to the **Output Picture** of the
Cycles Render Settings LOP (by default `$HIP/render/$HIPNAME.$OS.$F4.exr`).

### husk

Save the stage to a USD file (or use the USD file a ROP wrote) and run, from a command prompt with
Houdini's `bin` folder on the path:

```
husk -R Cycles scene.usd
```

`husk -R Cycles` renders with the stage's render settings, so the Cycles Render Settings LOP's
settings and passes are used. Without any render settings in the file, the image is written to
`cycles.exr`. Add `-o image.exr` to choose the output file, `-f` and `-n` to choose frames, and
`-s /Render/<prim>` to pick a specific render settings prim. `husk --list-renderers` should list
Cycles. More in [rendering.md](rendering.md).

## 5. Choose the device

By default Cycles uses the best available GPU: OptiX first, then CUDA, and the CPU when no
supported GPU is found. To change it:

- Set **Device** in the Device panel of the render settings (Cycles Render Settings LOP, the Cycles
  tab of the Render Settings LOP, or the viewport's display options). The choices are
  **Automatic (Best GPU)**, **GPU - OptiX**, **GPU - CUDA** and **CPU**. The menu also lists HIP
  and oneAPI, but those devices are not enabled in this build.
- Or set the `CYCLES_DEVICE` environment variable (`AUTO`, `OPTIX`, `CUDA` or `CPU`) when no device
  setting is authored, for example to force a CPU render from the command line:

  ```
  set CYCLES_DEVICE=CPU
  husk -R Cycles scene.usd
  ```

If the chosen device is not available, Cycles silently renders on the CPU; turn on **Abort
Without GPU** (Performance > Farm) to make husk fail instead. The device used is shown in the
render stats (`cyclesDevice`), or run `husk` with the environment variable `CYCLES_LOGGING=1`: the
log prints a line starting with `Path tracing on:` followed by the device name. GPU and CPU
renders of the same scene match; only the noise pattern differs.

With several GPUs, **GPU Devices** (Device panel) picks which ones render, and **Use CPU Too**
adds the CPU to husk renders, like Blender's Cycles Render Devices preferences. See
[render-settings.md](render-settings.md#device).

Changing the device restarts the render.

## Next steps

- [render-settings.md](render-settings.md) for every render setting.
- [lights.md](lights.md) for light settings and light linking.
- [aovs.md](aovs.md) for render passes and Cryptomatte.
- [troubleshooting.md](troubleshooting.md) if the viewport stays black or the render looks wrong.
