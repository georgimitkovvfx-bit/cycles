# Open Shading Language (OSL)

Cycles can run its shaders through Open Shading Language instead of its built-in shader virtual
machine (SVM). With OSL turned on you can use your own `.osl` shaders through the **Cycles Script**
node, like Blender's Script node, and Houdini's own OSL shader nodes inside Cycles materials.

## Turning OSL on

Turn on **Open Shading Language** in the Device panel of the render settings (`cycles:shading_system`,
on the Cycles Render Settings LOP, the Cycles tab of the Render Settings LOP or the viewport's
display options). Changing it restarts the render.

OSL works on these devices:

| Device | OSL |
|---|---|
| CPU | Supported |
| GPU - OptiX | Supported, with limits (below) |
| GPU - CUDA | Not supported: use OptiX or the CPU |

All built-in Cycles shader nodes also work with OSL. Renders with OSL and with SVM match, with the
exceptions listed under [Limits](#limits).

## The Cycles Script node

1. Inside a USD Cycles Material Builder, press Tab and choose **Cycles > Script > Cycles Script**.
2. Set **Shader File** to an `.osl` source file or a compiled `.oso` file.
3. Press **Load Shader**.

Load Shader compiles an `.osl` file with the OSL compiler that comes with the plugin (`oslc`, with
the Cycles OSL headers such as `stdcycles.h` on the include path), reads the shader's parameters,
and turns the node into a node type with the shader's inputs and outputs. The node is then labelled
**Cycles Script <shader name>**. Wire its outputs like any other node; closure outputs connect to
shader inputs or the material output.

A few things to know:

- The generated node type is named after the file name (`cycles_script_<file name>`) and is
  embedded in the hip file, so the hip file opens on machines that do not have the shader file.
  Two different shader files with the same file name share one node type: give them different
  names.
- Press **Load Shader** again after editing the shader to update the node's ports.
- Supported parameter types are `float`, `int`, `string`, `color`, `point`, `vector`, `normal` and
  `closure color`. Matrices, arrays and structs are skipped, like in Cycles.
- Parameter metadata such as ranges and help text becomes the parameter's range and help.
- At render time the delegate compiles and loads the shader from the **Shader File** path, so that
  file must be reachable from every machine that renders the scene (for example with `husk` on a
  render farm).
- `getattribute()` calls find primvars named by a string constant or a parameter's default value;
  names computed while the shader runs are not requested from the geometry.

Without OSL turned on (or on a CUDA device), a Script node renders nothing and the console prints
a warning that it needs Open Shading Language and a CPU or OptiX device.

## Houdini's OSL shader nodes

Houdini's **OSL Generic Shader Builder** (and other Houdini VOP nodes that describe an OSL shader
by its source code or compiled file) also work inside a Cycles material. Houdini exports them as
shaders with `info:implementationSource` set to `sourceAsset` or `sourceCode`; the delegate loads
the compiled shader through Houdini's asset resolver, or compiles the source code into a temporary
folder.

## Limits

- **On the GPU, only OptiX supports OSL.**
- **OSL shaders cannot trace rays on the GPU:** the **Ambient Occlusion** and **Raycast** nodes do
  nothing with OSL on OptiX. Blender has the same limit.
- **The first OptiX render compiles every OSL shader** of the scene and can take minutes for scenes
  with many materials. Later renders are faster.
- The **Color Attribute** node gives a different result than SVM when the geometry has no color
  attribute (Blender behaves the same way).
- Builds from source need an OSL that is built against Houdini's OpenImageIO; see
  [developers.md](developers.md#osl).
