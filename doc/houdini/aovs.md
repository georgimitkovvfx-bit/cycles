# AOVs and render passes

Every image Cycles writes besides the beauty is a USD **render var**: a prim under the render
settings that names its source (a Cycles pass, a light path expression, a primvar, ...). The
**Cycles Render Settings LOP** creates these render vars for you from toggles laid out like
Blender's View Layer Passes. You can also author render vars yourself, or render the AOVs of a
Karma Render Settings LOP with Cycles.

All render vars of a render product go into one multi-layer EXR by default, or into one image per
render var with **Split Render Product by Render Var**.

## Passes on the Cycles Render Settings LOP

The **Passes** folder of the Cycles Render Settings LOP has these sections. Each toggle adds one or
more render vars to the render product; the beauty (`color`, RGBA) is always rendered.

### Data

| Toggle | Render var | Cycles pass | Contents |
|---|---|---|---|
| Include > Z | `Depth` | `depth` | Distance from the camera, in stage units |
| Include > Mist | `Mist` | `mist` | Mist factor 0..1, shaped by Passes > Mist Pass in the render settings |
| Include > Position | `Position` | `position` | World position, in stage units |
| Include > Normal | `Normal` | `normal` | World space shading normal |
| Include > Vector | `Vector` | `motion` | Motion vectors in pixels, to the previous and next frame (see [motion-blur.md](motion-blur.md#the-vector-pass)) |
| Include > UV | `UV` | `uv` | The default UV map |
| Include > Denoising Data | `Denoising Normal`, `Denoising Albedo`, `Denoising Depth` | `denoising_normal`, `denoising_albedo`, `denoising_depth` | The guiding passes used by denoisers, for denoising in compositing |
| Indexes > Object Index | `IndexOB` | `object_id` | Object index |
| Debug > Sample Count | `Debug Sample Count` | `sample_count` | Samples taken per pixel (useful with adaptive sampling) |

The Z, Index, Normal, UV and Vector passes only see surfaces whose alpha is at least the
**Alpha Threshold** (render settings, Passes panel, default 0.5).

### Light

These are Blender's light passes, with Blender's EXR names. As in Blender, the Direct and Indirect
passes hold the light without the surface color; multiply them with the Color pass to rebuild the
contribution.

| Row | Direct | Indirect | Color |
|---|---|---|---|
| Diffuse | `DiffDir` (`diffuse_direct`) | `DiffInd` (`diffuse_indirect`) | `DiffCol` (`diffuse_color`) |
| Glossy | `GlossDir` (`glossy_direct`) | `GlossInd` (`glossy_indirect`) | `GlossCol` (`glossy_color`) |
| Transmission | `TransDir` (`transmission_direct`) | `TransInd` (`transmission_indirect`) | `TransCol` (`transmission_color`) |
| Volume | `VolumeDir` (`volume_direct`) | `VolumeInd` (`volume_indirect`) | |

| Other | Render var | Cycles pass | Contents |
|---|---|---|---|
| Emission | `Emit` | `emission` | Light emitted by surfaces seen directly |
| Environment | `Env` | `background` | The world background seen directly |
| Ambient Occlusion | `AO` | `ao` | Ambient occlusion |
| Shadow Catcher | `Shadow Catcher` | `shadow_catcher` | Shadows and reflections on shadow catcher objects (see [objects.md](objects.md)) |

### Light Path Expressions

These toggles create render vars with the names and light path expressions of Karma's AOVs, so
comps built for Karma find the images they expect. Cycles has no light path expression engine:
each expression is mapped to the Cycles pass that holds the same light paths. Unlike Blender's
light passes, these include the surface color (albedo), like Karma's.

| Row | Toggle | Render var | Expression |
|---|---|---|---|
| Diffuse | Combined / Direct / Indirect | `combineddiffuse`, `directdiffuse`, `indirectdiffuse` | `C<RD>.*L`, `C<RD>L`, `C<RD>.+L` |
| Glossy Reflection | Combined / Direct / Indirect | `combinedglossyreflection`, `directglossyreflection`, `indirectglossyreflection` | `C<RG>.*L`, `C<RG>L`, `C<RG>.+L` |
| Glossy Transmission | Combined | `glossytransmission` | `C<TG>.*L` |
| Volume | Combined / Direct / Indirect | `combinedvolume`, `directvolume`, `indirectvolume` | `CV.*L`, `CVL`, `CV.+L` |
| Other | Direct Emission | `directemission` | `CO` |
| Other | Albedo | `albedo` | `CDA` |

Cycles writes one buffer per pass type, so a Blender light pass and the expression that uses the
same Cycles pass cannot be rendered together, with and without the albedo. The LOP disables
**Direct** and **Indirect** under Diffuse and Glossy Reflection when the matching Blender pass
(DiffDir, DiffInd, GlossDir, GlossInd) is on.

Expressions other than the ones above (including Karma's shadow and unshadowed expressions) render
black, and a warning is printed.

### Cryptomatte

| Parameter | Default | Description |
|---|---|---|
| Include > Object | Off | Cryptomatte of objects, named after their prim paths |
| Include > Material | Off | Cryptomatte of materials, named after the material prim paths |
| Include > Asset | Off | Cryptomatte of assets: the parent prim path of each object |
| Levels | 6 | Number of objects, materials or assets per pixel that can be separated |

Each render var holds two levels, so the default of 6 levels writes three render vars per layer:
`CryptoObject00`, `CryptoObject01`, `CryptoObject02` (and the same for `CryptoMaterial` and
`CryptoAsset`). This is the standard Cryptomatte layout; compositing tools read all of them
together. Raise Levels when many small objects overlap in a pixel (hair, foliage, motion blur).

The Cryptomatte metadata (layer names, hash type, conversion and the manifest of names) is written
into the EXR header by `husk`, so Nuke's and other Cryptomatte tools can pick objects by name.
Write Cryptomatte to EXR; other formats cannot hold the metadata.

Use the Cryptomatte toggles of the Cycles Render Settings LOP. The Cryptomatte render vars of the
Karma Render Settings LOP use Karma-specific sources and do not produce Cryptomatte with Cycles.

The Cryptomatte, Position, Normal, UV, Vector and denoising normal render vars are authored with
float data types (not color types). This keeps `husk` from converting them from the OpenColorIO
`scene_linear` role to the output color space, which would break the IDs with ACEScg
configurations.

### Shader AOV

The **AOVs** list writes the values of **AOV Output** shader nodes. Add an entry per AOV, with the
same **Name** as the AOV Output node and its **Type** (Color or Value). Materials without that AOV
write black (color) or 0 (value). See [materials.md](materials.md#aov-output-node).

### Primvar AOV

The **Primvars** list writes a primvar of the geometry to an image, without changing any material:
every material and the default shader write it. Give the primvar name (for example `Cd` or a custom
attribute) and its type (Color or Value). Primvar names follow the same rules as in shaders,
including Houdini's names (see [objects.md](objects.md#primvars-in-shaders)). Adding, removing or
changing primvar AOVs updates the render without a restart.

A render var with source type `primvar` (or a source name starting with `primvars:`) does the same
when you author render vars yourself.

### Light Groups

The **Light Groups** list renders one image per light group, with Blender's name
`Combined_<name>`: the light of all lights, emissive objects and the dome light whose
**Light Group** is `<name>`. Set the light group on the Cycles tab of the Light LOP (lights and the
dome light, see [lights.md](lights.md#light-group)) or of the Render Geometry Settings LOP
(emissive objects, see [objects.md](objects.md#shading--light-group)), then add the same name
here. Lights and objects without a light group are in none of these images.

Light group images are not denoised.

### Split Render Product by Render Var

With **Split Render Product by Render Var** on, the LOP creates one render product per render var,
named `renderproduct_<AOV>`, and each writes its own image: the Output Picture with `_<AOV>` added
before the extension, for example `shot.0001_DiffDir.exr`. This works like the same option of the
Karma Render Settings LOP.

## Authoring render vars yourself

Render vars created with Houdini's Render Var LOP, or written into a USD file, work as long as their
source is one Cycles knows:

| Source name | Source type | Result |
|---|---|---|
| `color`, `depth`, `normal`, `primId` | `raw` | Hydra's standard AOVs: beauty, depth, normal, object index |
| A Cycles pass name, for example `diffuse_direct`, `mist`, `ao`, `shadow_catcher`, `emission`, `background`, `denoising_albedo` | `raw` | That Cycles pass, with a format that fits the pass |
| One of the light path expressions listed above | `lpe` | The Cycles pass with the same light paths, with albedo |
| `CryptoObject00`, `CryptoMaterial01`, ... | `raw` | Cryptomatte ranks (two per render var) |
| `lightgroup:<name>` | `raw` | The light group `<name>` |
| A primvar name, or `primvars:<name>` | `primvar` | Primvar AOV |
| Any other name | `raw` | Shader AOV written by AOV Output nodes of that name |

Data passes should use float data types (`float`, `float3`, `float4`) rather than color types, for
the color conversion reason given under [Cryptomatte](#cryptomatte). When writing with `husk`, set
`driver:parameters:aov:husk:name` on the render var for the layer name; the Cycles Render
Settings LOP does this for you.

## Karma's AOVs

If a **Karma Render Settings LOP** with AOVs is the stage's render settings, Cycles renders as many
of its render vars as it can:

| Karma AOV source | Cycles pass |
|---|---|
| `ray:hitP` | Position |
| `ray:hitPz` | Depth |
| `ray:hitN` | Normal |
| `ray:primid` | Object index |
| `st` | UV |
| `AO` | Ambient occlusion |
| The light path expressions listed above, and the beauty `C.*[LO]` | The matching Cycles light pass |

Other Karma AOVs (the shadow and unshadowed expressions, geometric normal, facing ratio,
camera-space position, element ID, Karma's motion vectors, coat and subsurface tags) have no Cycles
pass and render black. Karma's Cryptomatte render vars do not work with Cycles.

When both a Karma and a Cycles Render Settings LOP are in the chain, the Cycles LOP's
**Set as Default Render Settings Prim** makes sure `husk`, ROPs and TOPs use the Cycles settings.

## AOVs in the viewport

Every render var of the viewport's render settings appears in the viewport's **Render Outputs**
menu (the AOV menu of the viewer), so you can look at any pass while you work. Cycles renders the
displayed AOV with every update. To keep the viewport responsive, the AOVs that are not displayed
(depth, IDs and the inputs of slap comps) are refreshed about 0.25 seconds after a change and then
once per second; final renders write all of them completely.

In the viewport the depth AOV is converted to the window depth that Houdini uses to draw guides
and handles over the render; files rendered with `husk` keep the camera distance in stage units.

## Slap comps

- **husk:** put image filter list prims (made with the **Image Filter LOP**) into the **Image
  Filters** parameter of the Cycles Render Settings LOP. `husk` applies them to the rendered AOVs,
  like Karma's slap comps.
- **Copernicus:** the **Slap Comp Import** COP can read the render outputs of the Cycles viewport,
  including the color.

## Denoising and passes

With Denoise on, the beauty is denoised. The other passes are written as rendered; use the
Denoising Data passes to denoise them in compositing if needed. Light group passes are always
written noisy.
