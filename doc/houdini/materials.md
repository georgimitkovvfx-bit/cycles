# Materials

Cycles renders three kinds of materials in Houdini:

1. **Cycles materials**, built from the Cycles shader nodes inside a **USD Cycles Material
   Builder**. This gives you every Cycles feature and matches Blender.
2. **Karma and MaterialX materials** (Karma Material Builder, MaterialX Standard Surface, OpenPBR,
   ...), which the delegate translates to Cycles nodes.
3. **UsdPreviewSurface** materials, also translated.

When a material has several networks, Cycles uses its own network (render context `cycles`) first,
then the MaterialX networks (render contexts `mtlx` and `kma`), then the universal network
(UsdPreviewSurface). So a material can carry a Cycles version next to its Karma version, and each
renderer picks its own.

## USD Cycles Material Builder

To create a Cycles material:

1. In a **Material Library LOP** (or another material network), press Tab and choose
   **Cycles > USD Cycles Material Builder**.
2. Dive inside. The Tab menu has a **Cycles** sub-menu with all Cycles shader nodes. The builder
   also accepts Houdini's USD Preview nodes.
3. Connect your shader (for example a **Cycles Principled BSDF**) to the surface input of the
   builder's output node, and a displacement network (for example **Cycles Displacement**) to its
   displacement input.

Volume shaders (Principled Volume, Volume Scatter, ...) can be connected to the surface input as
well: Houdini's builder authors every Cycles shader as the surface terminal, and the delegate sends
a volume shader to the Volume input of the Cycles material.

The builder is a regular Houdini material builder. Its parameters can be promoted, and the nodes
inside can be wired like any VOP network. When Houdini writes the material to USD, it uses the
Cycles shader translator that comes with the plugin, which converts ramps and angles (see
[Ramps, curves and angles](#ramps-curves-and-angles)).

### The Settings tab

Builders made with the shelf tool have a **Settings** tab like Blender's Material Properties >
Settings panel. The settings are authored as `config:cycles:<setting>` attributes on the material
prim, and only when they differ from their default. Unset settings use the Cycles defaults, which
are Blender's. Materials that are not Cycles networks (MaterialX, Karma, UsdPreviewSurface) use
Displacement and Bump for their displacement output, so it displaces the surface like in Karma.

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Pass Index | `config:cycles:pass_id` | 0 | Index number for the Material Index render pass (render var with source name `material_id`) |
| Surface > Displacement | `config:cycles:displacement_method` | Bump Only | How the displacement output is used: Bump Only (`bump`), Displacement Only (`true`, true displacement of the surface) or Displacement and Bump (`both`, true displacement with bump mapping for the finer details) |
| Surface > Emission Sampling | `config:cycles:emission_sampling_method` | Auto | Sampling strategy for emissive surfaces: None, Auto, Front, Back, Front and Back |
| Surface > Transparent Shadows | `config:cycles:use_transparent_shadow` | On | Use transparent shadows for this material if it contains a Transparent BSDF. Turning it off renders faster but shadows are not accurate |
| Surface > Bump Map Correction | `config:cycles:use_bump_map_correction` | On | Apply corrections to solve shadow terminator artifacts caused by bump mapping |
| Volume > Sampling | `config:cycles:volume_sampling_method` | Multiple Importance | Sampling method for volumes: Distance, Equiangular, Multiple Importance |
| Volume > Interpolation | `config:cycles:volume_interpolation_method` | Linear | Interpolation of smoke and fire volumes: Linear or Cubic |
| Volume > Step Rate | `config:cycles:volume_step_rate` | 1 | Scale of the distance between volume shader samples. Lower values give more accurate and detailed results, but take longer |

Builders created with older versions have no Settings tab: right-click the builder (or several
selected builders) and choose **Add Cycles Material Settings**.

True displacement subdivides nothing by itself: Cycles displaces the vertices the mesh has. Turn
on **Adaptive Subdivision** for the object (Render Geometry Settings LOP, Cycles tab) so Cycles
dices the mesh finely enough, also for polygon meshes (see [objects.md](objects.md#subdivision)).

## The shader nodes

Every user-facing Cycles shader node is a VOP node named **Cycles <Blender name>** (node type
`cycles_<cycles name>`). The nodes are laid out like Blender 5.3's shader nodes: the same parameter
order, labels, menus, collapsible panels, rules for hiding inputs and default values. Parameter
names are the Cycles socket names, so USD files are portable between Houdini and other Cycles
hosts.

The Tab menu groups the nodes like Blender's Add menu:

| Tab menu | Nodes |
|---|---|
| Cycles > Input | Ambient Occlusion, Attribute, Bevel, Camera Data, Color Attribute, Curves Info, Dome Texture (world materials, see [world.md](world.md)), Fresnel, Geometry, Layer Weight, Light Path, Object Info, Particle Info, Point Info, Raycast, Scene Time, Tangent, Texture Coordinate, UV Map, Volume Info, Wireframe |
| Cycles > Input > Constant | Color, Value |
| Cycles > Output | AOV Output |
| Cycles > Shader | Add Shader, Background, Diffuse BSDF, Emission, Glass BSDF, Glossy BSDF, Hair BSDF, Holdout, Metallic BSDF, Mix Closure Weight, Mix Shader, Principled BSDF, Principled Hair BSDF, Principled Volume, Ray Portal BSDF, Refraction BSDF, Sheen BSDF, Subsurface Scattering, Toon BSDF, Translucent BSDF, Transparent BSDF, Volume Absorption, Volume Coefficients, Volume Scatter |
| Cycles > Displacement | Bump, Displacement, Normal Map, Vector Displacement |
| Cycles > Color | Blackbody, Brightness/Contrast, Color Ramp, Combine Color, Gamma, Hue/Saturation/Value, Invert Color, Light Falloff, Mix (color), Mix (Legacy), RGB Curves, RGB to BW, Separate Color, Wavelength |
| Cycles > Texture | Brick, Checker, Environment, Gabor, Gradient, IES, Image, Magic, Noise, Sky, Voronoi, Wave and White Noise Texture |
| Cycles > Utilities > Math | Boolean Math, Clamp, Float Curve, Integer Math, Map Range (float), Math, Mix (float) |
| Cycles > Utilities > Vector | Combine XYZ, Get Vector Component, Map Range (vector), Mapping, Mix (vector, uniform and non-uniform factor), Normal, Parallax Occlusion, Radial Tiling, Separate XYZ, Set Normal, Vector Curves, Vector Math, Vector Rotate, Vector Transform |
| Cycles > Script | Script (OSL), see [osl.md](osl.md) |

Blender's Mix and Map Range nodes switch their data type with a menu. In Cycles each data type is
a separate node, so the Tab menu has several nodes labelled **Cycles Mix** (one for colors under
Color, one for floats under Utilities > Math, two for vectors under Utilities > Vector: with a
single factor and with a per-component factor) and two labelled **Cycles Map Range** (float and
vector). The node type names tell them apart: `cycles_mix_color`, `cycles_mix_float`,
`cycles_mix_vector`, `cycles_mix_vector_non_uniform`, `cycles_map_range`, `cycles_vector_map_range`.
**Mix (Legacy)** (`cycles_mix`) is the old color mix node.

The **Background** shader is meant for world shaders, see [world.md](world.md).

### Defaults

Houdini only writes parameters that differ from the node's default value. The delegate applies the
same defaults as the UI (Blender's defaults, which sometimes differ from the raw Cycles defaults),
so an untouched parameter renders the value you see in the parameter editor.

### Ramps, curves and angles

- **Color Ramp**, **Float Curve**, **RGB Curves** and **Vector Curves** use Houdini ramp
  parameters. When the material is written to USD, the Cycles shader translator bakes each ramp to
  the 256-entry lookup table Cycles uses, with each key's own interpolation. Houdini's monotone
  cubic keys approximate Blender's curve handles, so curves can differ slightly from the same curve
  drawn in Blender. RGB Curves apply the combined curve (C) first and then the R, G and B curves,
  like Blender. Vector Curves map -1..1 to -1..1, like Blender. A Color Ramp whose keys are all
  Constant becomes a constant interpolation ramp.
- **Angles are in degrees**, like in Blender: Mapping and texture mapping rotation, Vector Rotate,
  Sky Texture, Gabor Texture, hair offsets and others. The translator writes radians to USD, so USD
  files keep Cycles units. Hip files saved before angles were shown in degrees are converted once
  when they are loaded; animated angles in such files stay in radians and need to be fixed by hand.
- Ramps and degrees rely on the shader translator in the plugin's `husdplugins` folder. If it is
  missing (for example when an incomplete install is on the Houdini path), ramps fall back to
  Houdini's own ramp export, which loses per-key interpolation.

### Textures

The **Image Texture** and **Environment Texture** nodes read any image format OpenImageIO reads,
and Houdini's `.rat` files (also UDIM tiles, MaterialX/Karma images and dome/rect light textures):
those are converted once with Houdini's image library to TIFF files with the same data type in
`%TEMP%\hdCycles_houdini_images` (again when the `.rat` file changes), which Cycles reads like any
other file. Delete that folder to free the space.

- **Color space:** leave it on automatic to let Cycles detect it: 8-bit images are read as sRGB,
  float images as scene linear. Use a raw or non-color space for data textures (roughness, normal
  maps, masks). This also works with ACES OpenColorIO configurations.
- **UDIMs:** file names with `<UDIM>` (`1001`, `1002`, ...) or `<UVTILE>` (`u1_v1`, `u2_v1`, ...)
  read all tiles found next to the file.
- **Missing files** render with the node's missing color.
- **Copernicus and COP images** (`op:` paths, in the Houdini session): converted with Houdini's
  image library into the same cache folder as `.rat` files each time the material (or light) is
  synced. A changed COP network shows after a material edit or **Reload Textures**.
- **Texture cache and size limit:** see [rendering.md](rendering.md#texture-cache).
- **Changed files:** a texture file that changes on disk is reloaded the next time the material is
  edited. To reload without editing anything, use **Reload Textures** in the viewport's renderer
  menu.

### Raycast node

The **Raycast** node casts a ray and reads the hit point. Its **Attributes** panel has four slots:
type an attribute name into a slot, and the slot's color, factor and alpha outputs return that
attribute at the hit point (like the attribute outputs Blender adds to its Raycast node). Objects
can be hidden from Raycast rays with the **Raycast** toggle of their ray visibility (see
[objects.md](objects.md)).

### Parallax Occlusion node

The **Parallax Occlusion** node (Cycles > Utilities > Vector, not in Blender) returns the texture
coordinates where the view ray hits a height map below the surface, for bricks, tiles or cobbles
that look deep without displacement. Connect the height map (1 = the surface, 0 = the deepest
point) into **Height** and the node's **Vector** output into the Vector input of the color, normal
and roughness textures. **Depth** is the depth of the height field in UV units, **Steps** the
number of layers searched (more is slower but avoids stepping at grazing angles), **UV Map** the
UV map of the tangent frame (empty: the default UV map, like the Normal Map node).

The delegate expands the node into standard nodes: Steps + 1 copies of the nodes linked to Height,
each reading the texture coordinates of one layer. In those copies, the inputs that read UVs
(unlinked texture Vector inputs, links from a Texture Coordinate or UV Map UV output, or from the
same output as the node's own Vector input) read the shifted coordinates; procedural textures on
Generated or Object coordinates are not shifted. The silhouette of the mesh does not change.

### AOV Output node

The **AOV Output** node writes a value to a shader AOV, which you render with a render var of the
same name (Passes > Shader AOV on the Cycles Render Settings LOP, see [aovs.md](aovs.md)). In
Houdini it is a pass-through node: connect your shader into its **shader** input and its output
into the material output. Houdini only exports nodes connected to the output, and the delegate
skips the pass-through when it builds the Cycles shader. You can chain several AOV Output nodes.

### Reading attributes (primvars)

The **Attribute**, **UV Map**, **Color Attribute**, **Normal Map** and **Tangent** nodes read USD
primvars by name, including Houdini's names (`Cd`, `uv`, `Alpha`, ...) and per-instance primvars of
instancers. See [objects.md](objects.md#primvars-in-shaders).

## UsdPreviewSurface

UsdPreviewSurface materials are translated to a Principled BSDF, UsdUVTexture to an Image Texture
node and UsdPrimvarReader to an Attribute node. The results match Karma within a few percent.

| UsdPreviewSurface input | Cycles translation |
|---|---|
| `diffuseColor` | Base Color (USD's default 0.18) |
| `emissiveColor` | Emission Color, strength 1 |
| `metallic`, `roughness`, `ior` | Metallic, Roughness, IOR (metallic workflow) |
| `useSpecularWorkflow`, `specularColor` | Specular workflow: specularColor is the reflectance at normal incidence (F0) and becomes the Specular Tint; metallic and ior are not used |
| `clearcoat`, `clearcoatRoughness` | Coat Weight, Coat Roughness (USD's default 0.01) |
| `opacity`, `opacityThreshold` | Alpha; an opacityThreshold above 0 makes a cutout |
| `normal` | Tangent space normal, through a Normal Map node |
| `occlusion`, `displacement` | Not translated |

| UsdUVTexture / UsdPrimvarReader | Cycles translation |
|---|---|
| `file`, `st`, `wrapS`/`wrapT` | Image file, texture coordinate, extension (`repeat` = periodic) |
| `sourceColorSpace` | `raw` reads data, `sRGB` and `auto` use automatic detection |
| `scale`, `bias` | Applied to the outputs (for example to unpack normal maps), only when authored as values; connected scale or bias inputs are ignored |
| `fallback` | Color for missing files and UDIM tiles (black by default, like Karma), and for geometry without the primvar of a primvar reader |
| r, g, b outputs | Luminance of the texture, not the single channels |
| `UsdTransform2d` | Not translated: the node is skipped with a warning, so the texture uses its default coordinates |

## MaterialX and Karma materials

Materials made with the Karma Material Builder or MaterialX nodes are translated when the scene is
rendered. Houdini's USD libraries build a MaterialX document per material, and the delegate turns
it into a Cycles shader graph.

**Supported:**

- Surface shaders: `standard_surface`, `open_pbr_surface`, `UsdPreviewSurface` and `gltf_pbr`, all
  mapped to the Principled BSDF.
- About 100 nodes of the MaterialX standard and PBR libraries: images (`image`, with UDIM file
  names), math (add, multiply, mix, clamp, power, dot, cross product, normalize, transforms,
  comparisons, ...), noise (`noise2d/3d`, `fractal2d/3d`, `cellnoise2d/3d`, `worleynoise2d/3d`),
  color (luminance, HSV conversion, premultiply, blackbody), compositing (blend modes, `over`,
  `in`, `out`, `mask`, `matte`, `disjointover`), geometry (`texcoord`,
  `geompropvalue`, `geomcolor`, position, normal, tangent, bitangent, `time`), normal maps and
  height to normal, and closures (`dielectric_bsdf`, `conductor_bsdf`, `generalized_schlick_bsdf`,
  `subsurface_bsdf`, `sheen_bsdf`, `burley_diffuse_bsdf`, `translucent_bsdf`, `uniform_edf`,
  `surface`, `surface_unlit`, `displacement`), hair (`chiang_hair_bsdf` as the Principled Hair BSDF
  with the Chiang model, `chiang_hair_roughness`, `chiang_hair_absorption_from_color`,
  `deon_hair_absorption_from_melanin`), and volumes (`volumematerial`, `volume`,
  `anisotropic_vdf` and `absorption_vdf` as Cycles' Volume Coefficients, with `uniform_edf` as
  volume emission).
- Nodes that MaterialX defines as node graphs of other nodes (`tiledimage`, `place2d`,
  `colorcorrect`, `hsvadjust`, `checkerboard`, Houdini's `hmtlx` nodes and others) are expanded and translated node
  by node.
- Geometry nodes read primvars with the same rules as the Cycles Attribute node.

**Limits:**

- Noise, fractal and Worley nodes use Cycles' noise: the same character, but a different pattern
  than Karma's.
- The OpenPBR coat is approximated.
- BSDF layering (`layer`) is approximated by adding the layers.
- `chiang_hair_bsdf`: the TT and TRT roughness follow the R roughness with the default scales (0.5
  and 2), and the average of the three lobe tints weights the whole BSDF. Karma XPU does not render
  this BSDF; Karma CPU does.
- Matrix nodes and hex tiling (`hextiledimage`, rendered as a plain image) are not supported. Unsupported nodes output zero and print a warning.
- Editing any parameter of a MaterialX material rebuilds the whole Cycles shader.

To see what the delegate receives, set the environment variable `HD_CYCLES_MTLX_DEBUG=1`; each
MaterialX document is then printed to the console.

## Materials and Cryptomatte

Cycles shaders are named after the material prim path, which is the name the CryptoMaterial layer
uses. See [aovs.md](aovs.md#cryptomatte).
