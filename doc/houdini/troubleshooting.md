# Troubleshooting

Start with the install check: it tells you whether the package loads, which device renders and
whether materials come out right.

```python
exec(open(hou.text.expandString("$CYCLES_ROOT/../test/check_install.py")).read())
```

(run it in **Windows > Python Shell**; see [install.md](install.md#checking-the-install) for the
command-line version). To see what Cycles is doing, start Houdini or `husk` with the environment
variable `CYCLES_LOGGING=1` (and `CYCLES_LOGGING_LEVEL=debug` for more detail); warnings from the
delegate are printed to the console either way.

## Common problems

### Cycles is not in the renderer menu

- Houdini was open while you installed: restart it.
- `cycles.json` is not in a folder Houdini reads packages from, or the `hdCycles` folder is not next
  to it, or it was renamed. `check_install.py` reports "package loaded (CYCLES_ROOT)" as failed.
- The Houdini version is not 22.0.429. The plugin is built against the USD libraries of that exact
  version and does not load in other versions.
- Another plugin's libraries clash with Cycles. The shareable build avoids the known case (the SYCL
  runtime shipped with Arnold); if you built from source, see [developers.md](developers.md).

### The viewport stays black, or the render does not start

- **First render:** the first GPU render after installing or a driver update loads the GPU kernels,
  and the first OptiX render with Open Shading Language compiles all OSL shaders. Wait a little
  (with OSL, possibly minutes).
- **Another Cycles package:** an older hdCycles build can be loaded instead of this one, with its
  own bugs. Remove other Cycles packages (see [install.md](install.md#other-cycles-packages)) and
  run `check_install.py`: it checks "render delegate from this package" and "no other Cycles
  UsdRenderers.json".
- **Dome light and Display Environment Maps:** with a dome light, Houdini draws the dome's texture
  behind the Cycles image, and where the render is transparent you see only the texture. Turn off
  *Display Environment Maps* in the viewport's display options to check.
- **Exposure:** check Film > Exposure in the render settings and the exposure of your lights.
- **No GPU:** if the GPU is not supported (or the driver is too old), Cycles renders on the CPU,
  which is slower but not black. Check the device with `CYCLES_LOGGING=1`.

### Orbiting pivots around the camera

The viewport tumbles around the camera instead of the object under the cursor. This happens with
older Cycles packages, whose registration describes the depth differently. Remove other Cycles
packages; this build's registration fixes it.

### The render uses the CPU instead of the GPU

- Device is set to CPU in the render settings or display options, or `CYCLES_DEVICE=CPU` is set in
  your environment or `houdini.env`.
- The GPU is not an NVIDIA RTX 20 series or newer, or the driver is older than R535. Other GPUs
  (GTX 10 series, AMD, Intel) render on the CPU. HIP and oneAPI devices are not enabled in this
  build; choosing them falls back to the CPU.
- A device chosen explicitly that is not available also falls back to the CPU.

### Changing a setting does nothing in the viewport

Some settings are only read when the render session is created (Device, Threads, Open Shading
Language, Curves Shape, Start at Low Resolution, the texture cache settings and Texture Size
Limit). Houdini restarts the viewport render when they change; if a render does not pick up the
new value, restart it from the viewport's renderer menu. **Threads** never applies in the viewport,
only in `husk`.

### The install fails because a file is in use

Windows locks `hdCycles.dll` while any process uses it: Houdini, `husk`, `hython` or a TOPs
render. Close them all before unzipping a new version (or before installing a build from source).

### License errors when running check_install.py

`hython` needs a Houdini license like Houdini itself. If the command-line check fails with a
license error, run the Python Shell version inside Houdini instead.

### Colors look wrong with an ACES / ACEScg OpenColorIO configuration

- **Image textures:** leave the color space on automatic for color textures (8-bit images are read
  as sRGB, float images as scene linear), and use a raw or non-color space for data textures.
  Automatic detection works with Houdini's ACES configuration.
- **Cryptomatte and data passes:** `husk` converts render vars with color types from the OCIO
  `scene_linear` role to the output color space. The Cycles Render Settings LOP authors Cryptomatte,
  Position, Normal, UV, Vector and denoising normal render vars with float types so they are
  written untouched. If you author render vars yourself, use `float`, `float3` or `float4` for data
  passes, not `color3f` or `color4f`.

### Cryptomatte does not work

- Use the Cryptomatte toggles of the Cycles Render Settings LOP. The Cryptomatte render vars of the
  Karma Render Settings LOP do not produce Cryptomatte with Cycles.
- Write EXR files; the metadata and manifest go into the EXR header.
- Three render vars per layer (`CryptoObject00`, `01`, `02`) is normal for the default of 6 levels.

### An AOV is black

- Light path expressions that do not match one of Karma's common expressions render black, as do
  Karma AOVs without a Cycles pass (see [aovs.md](aovs.md#karmas-aovs)).
- A shader AOV is black on materials without an AOV Output node of that name.
- The Vector pass is empty with motion blur on.
- A Blender light pass and the light path expression that uses the same Cycles pass cannot be
  rendered together.

### Nothing is motion blurred

- Turn on Motion Blur in the render settings and give the camera an open shutter (Shutter Open
  lower than Shutter Close).
- Motion from LOP parameter expressions exists only at the current frame: cache it with a Motion
  Blur LOP. See [motion-blur.md](motion-blur.md).

### A texture does not update after I changed the file

Use **Reload Textures** in the viewport's renderer menu, or edit the material.

## Known issues

These are the open limits of the current version that affect users.

**Lights and world**

- Cylinder lights do not render (Cycles has no cylinder light).
- Dome lights with a Z pole axis (`poleAxis` of DomeLight_1) are not supported.
- At most 64 light linking and 64 shadow linking sets; point instances use their prototype's links.

**Materials and textures**

- UsdPreviewSurface: `occlusion` and `displacement` are not translated; `ior` is ignored in the
  specular workflow; UsdUVTexture scale and bias only apply when authored as values; its r, g and b
  outputs are the luminance; UsdTransform2d is not translated.
- MaterialX: Cycles' noise instead of MaterialX's noise patterns, approximated OpenPBR coat, layering
  as an add, no matrix nodes, hex tiling, some compositing nodes or volume materials. Editing a
  parameter rebuilds the whole shader.
- Only the `<UDIM>` tile token is supported (not `<UVTILE>`), and Copernicus (`op:`) images cannot
  be used as textures.
- Ramps: Houdini's monotone cubic keys approximate Blender's curve handles; Min X and Max X stay
  visible. Animated angles in hip files saved before angles were shown in degrees stay in radians.
  The Raycast node has 4 attribute slots.
- Some shader nodes show their inputs in an unexpected order in the network editor.

**OSL**

- On the GPU only OptiX supports OSL, and OSL shaders cannot trace rays there (Ambient Occlusion
  and Raycast nodes do nothing). The first OptiX render compiles every OSL shader.
- `getattribute()` in a Script node only finds attribute names given as string constants or
  parameter defaults.

**Geometry**

- Matrix, string and int64 primvars are not exported. Per-object settings are not read per
  instance.
- Curve bases other than Catmull-Rom (B-spline, Bezier) render as Catmull-Rom curves. Subdivision
  has no dicing settings.
- Gaussian splats (`particleField`), panoramic cameras and portal lights are not supported.

**Motion blur**

- Curve and point widths do not blur; instancers that change their instance count within the
  shutter get no motion; animated focal length gives no zoom in the Vector pass.

**AOVs**

- Cycles has no light path expression engine; only Karma's common expressions are mapped.
- Karma's shadow and unshadowed expressions, geometric normal, facing ratio, camera-space position,
  element ID and motion vector AOVs render black.

**Viewport**

- Passes the viewport does not display refresh about 0.25 seconds after a change and then once per
  second.
- With a dome light, the viewport image takes a slower path and the low resolution start of each
  change is not shown; transparent pixels show Houdini's environment map display.
- DLSS has no upscaling modes, and needs an RTX GPU with driver 620 or newer. The shareable build
  does not include DLSS.
- Camera LOPs saved with earlier builds author `cycles:camera:*` attributes: changes to them need
  a render restart. Cameras from this build author `primvars:cycles:camera:*` and update live.

**Other**

- Path guiding is not enabled; its settings do nothing.
- AMD (HIP) and Intel (oneAPI) GPUs are not supported.
- The Huang hair model gives a slightly different noise pattern on the CPU and the GPU for the same
  seed (the average is the same).
- Running two GPU renders at the same time on one GPU can stall one of them.
- The Cycles Render Settings LOP still needs some layout polish.
