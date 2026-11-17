# Installing hdCycles

hdCycles is shared as a zip file named like `hdCycles-houdini22.0.429-win64-<version>.zip`. It is a
Houdini package: a small `cycles.json` file that tells Houdini where the plugin is, and an
`hdCycles` folder with the plugin itself. Nothing is installed system-wide and no environment
variables need to be set by hand.

## Requirements

- Windows 10 or 11, 64-bit.
- Houdini 22.0.429. Other Houdini 22.0 builds are untested; Houdini 21 and 22.5 will not load the
  plugin, because it is compiled against the USD libraries of this exact Houdini version.
- For GPU rendering: an NVIDIA RTX 20 series GPU or newer and a driver of R535 or newer. Without a
  supported GPU, Cycles renders on the CPU.

## Installing the zip

1. Close Houdini (and any `husk` or `hython` processes). Windows locks the plugin DLL while it is
   loaded, so files cannot be replaced while Houdini runs.
2. Unzip the archive into your Houdini packages folder, normally
   `C:\Users\<you>\Documents\houdini22.0\packages\`. Create the `packages` folder if it does not
   exist. You should end up with:

   ```
   packages\
     cycles.json
     hdCycles\
       README.txt
       houdini\        the Houdini side: plugin DLL, HDAs, render settings UI, shelf tool
       lib\            precompiled CUDA and OptiX kernels
       shader\         OSL headers and compiled OSL shaders of the Cycles nodes
       license\        licenses of Cycles and its libraries
       test\           check_install.py and a test scene
   ```

3. Do not rename the `hdCycles` folder: `cycles.json` looks for it next to itself.
4. Remove any other Cycles or hdCycles package from the packages folder (see
   [Other Cycles packages](#other-cycles-packages)).
5. Start Houdini. In Solaris, the renderer menu of the viewport now lists **Cycles**.

The first GPU render after installing, and again after a GPU driver update, takes longer than
usual while Cycles loads its GPU kernels.

### What the package file does

`cycles.json` is a standard Houdini package file:

```json
{
  "env": [
    { "CYCLES_ROOT": { "value": "$HOUDINI_PACKAGE_PATH/hdCycles/houdini", "method": "replace" } },
    { "CYCLES_ICON": { "value": "$CYCLES_ROOT/icons/cycles.png", "method": "replace" } },
    { "PATH": { "value": "$CYCLES_ROOT/bin", "method": "append" } }
  ],
  "hpath": "$CYCLES_ROOT"
}
```

Houdini sets `$HOUDINI_PACKAGE_PATH` to the folder that contains the package file while it reads
it, so `CYCLES_ROOT` always points to the `hdCycles\houdini` folder next to `cycles.json`.
`CYCLES_ROOT` is added to `HOUDINI_PATH`, which is how Houdini finds the render delegate
(`dso\usd_plugins`), its registration (`UsdRenderers.json`), the shader and LOP nodes (`otls`), the
render settings tabs (`soho\parameters`), the shader translator (`husdplugins`) and the shelf tool
(`toolbar`). `$CYCLES_ROOT/bin` is appended to `PATH` for the runtime DLLs and the OSL compiler.

### Installing somewhere else

To keep the plugin outside the packages folder (for example on a shared drive), put the
`hdCycles` folder where you want it and change the `CYCLES_ROOT` value in `cycles.json` to the
absolute path of its `houdini` folder, for example `"value": "<install folder>/hdCycles/houdini"`. Keep
`cycles.json` in a folder Houdini reads packages from (your `packages` folder, or a folder listed
in `HOUDINI_PACKAGE_DIR`). Use forward slashes in the path.

Builds made from source contain their own package file at
`<install folder>/houdini/packages/cycles.json`, with the absolute install path already filled in;
copy it into your packages folder (see [developers.md](developers.md)).

## Checking the install

The zip contains a check script that verifies the package, renders a small test scene with `husk`
and checks the colors of the result. It takes about a minute (longer on the very first GPU run).

From a command prompt:

```
"C:\Program Files\Side Effects Software\Houdini 22.0.429\bin\hython.exe" "%USERPROFILE%\Documents\houdini22.0\packages\hdCycles\test\check_install.py"
```

Or inside Houdini, in **Windows > Python Shell**:

```python
exec(open(hou.text.expandString("$CYCLES_ROOT/../test/check_install.py")).read())
```

Every line should say `PASS`. The script also prints the device Cycles rendered with (your GPU, or
CPU) and the path of the rendered image. These are the checks:

| Check | What it means when it fails |
|---|---|
| package loaded (CYCLES_ROOT) | Houdini did not read `cycles.json`, or the `hdCycles` folder is not next to it |
| render delegate registered | The plugin DLL was not found or did not load |
| render delegate from this package | Another hdCycles build was loaded instead of this one |
| no other Cycles UsdRenderers.json | Another Cycles package is installed and can override the viewport registration |
| Cycles shader nodes | The shader HDAs (`otls`) are missing |
| Cycles Render Settings LOP | The Cycles Render Settings LOP HDA is missing |
| husk render | `husk -R Cycles` failed; the last lines of its log are printed |
| image size, no NaN or Inf pixels | The rendered image is incomplete or broken |
| Principled BSDF / UsdPreviewSurface / Emission / floor | The materials did not render with the expected colors |

To look at the test scene yourself, add a **Sublayer LOP** in `/stage`, set its file to
`$CYCLES_ROOT/../test/cycles_test.usda` and switch the viewport renderer to Cycles. You should see a
red sphere (Cycles Principled BSDF), a blue sphere (UsdPreviewSurface) and a small glowing green
sphere (Cycles Emission) on a grey floor.

## Living with other renderers and plugins

hdCycles is one more Hydra render delegate. Karma, Storm and other delegates keep working, and you
switch between them in the viewport's renderer menu or with `husk -R <renderer>`.

The plugin is built so that it can load next to Arnold (HtoA): its Embree ray tracing library is
linked into the plugin, and the SYCL runtime that conflicts with other plugins is not shipped. It
uses Houdini's own copies of OpenImageIO, OpenColorIO, OpenVDB, OpenImageDenoise, TBB, USD and
MaterialX, so it does not bring library versions that clash with Houdini.

### Other Cycles packages

Only one Cycles render delegate can be active at a time. Other builds of hdCycles (for example an
older version of this package, or the upstream `cjhosken/hdCycles` plugin) register the same
renderer name, and whichever Houdini finds first wins, including its viewport settings. A symptom
of a stale package is orbiting in the viewport that pivots around the camera instead of the object
under the cursor.

Before installing, remove every other package file that loads Cycles (look for other `.json` files
in your packages folder that mention Cycles or hdCycles) and any `PXR_PLUGINPATH_NAME` entry
pointing to another Cycles build in `houdini.env`. `check_install.py` reports another Cycles
registration found on the Houdini path.

## Updating

Close Houdini, delete the old `cycles.json` and `hdCycles` folder, and unzip the new version in
their place. Unzipping over an old version can leave files behind that the new version no longer
uses.

## Uninstalling

Close Houdini and delete `packages\cycles.json` and the `packages\hdCycles` folder. Hip files that
use Cycles nodes still open without the plugin, but the Cycles shader nodes and the Cycles Render
Settings LOP show up as unknown node types until the plugin is installed again.
