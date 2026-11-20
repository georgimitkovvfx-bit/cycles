# Render settings

Cycles render settings are USD attributes named `cycles:...` on a render settings prim. Houdini
gives you three places to edit them. All three use the same layout, which follows Blender's Cycles
render properties (Sampling, Light Paths, Volumes, Curves, Simplify, Motion Blur, Film, Passes,
Performance), and the same defaults, which are the values the render delegate uses when a setting
is not authored.

## Where the settings live

### Cycles Render Settings LOP

The **Cycles Render Settings** LOP (in the LOP Tab menu under **Cycles** and under **Render**) is the recommended way to set up
a Cycles render. Like the Karma Render Settings LOP, one node creates:

- a render settings prim at **Render Settings Path** (default `/Render/$OS`),
- a render product `Products/renderproduct` that writes the **Output Picture**,
- a render var `Vars/beauty` for the beauty image (RGBA), and one render var per pass you turn on
  in the Passes folder (see [aovs.md](aovs.md)).

Every Cycles setting on this node is authored on the render settings prim, so the node fully
defines the render. When **Set as Default Render Settings Prim** is on (the default), the node also
makes its prim the stage's default render settings, which `husk`, the USD Render ROP, TOPs and the
viewport use when no render settings are chosen explicitly. This matters when a Karma render
settings LOP is also in the chain: without it, the other render settings could win.

The top of the node has the parameters of the stock Render Settings LOP that a render needs:

| Parameter | Purpose |
|---|---|
| Render Settings Primitives | Paths of the render settings, products and vars prims, and **Set as Default Render Settings Prim** |
| Output Picture | Image file to render to (default `$HIP/render/$HIPNAME.$OS.$F4.exr`) |
| Image Filters | Image filter list prims (made by the Image Filter LOP) that `husk` applies to the rendered AOVs, like a slap comp |
| Camera, Resolution Mode, Resolution, Pixel Aspect Ratio | The same parameters as on the stock Render Settings LOP |
| Blender Defaults | Sets Blender's final-render defaults, see below |

**Blender Defaults** sets 4096 samples, denoising on, the Blue-Noise (pure) sampling pattern,
Light Threshold 0.01, bounces of 12 total, 4 diffuse, 4 glossy, 12 transmission, 0 volume and
8 transparent, a Blackman-Harris pixel filter of width 1.5, a transparent glass Roughness Threshold
of 0.1, 3D Curves with 2 Curve Subdivisions, and Use Compact BVH off. Denoising stays on the GPU, while Blender denoises final renders on the CPU.
The button changes values, not defaults: **Revert to Defaults** on a parameter returns it to the
Cycles default.

Because this node authors every setting, the **Transparent** toggle (Film > Transparent) is
authored too: the background is opaque unless you turn it on. With the stock Render Settings LOP,
leaving Transparent unset gives the automatic behavior described in the
[notes on the reference](#notes-on-the-reference).

Settings inside a panel whose header toggle is off (for example the Denoise options when Denoise is
off, or Motion Steps when Motion Blur is off) are disabled in the UI and are not authored.

### Cycles tab on the Render Settings LOP

The stock **Render Settings LOP** gets a **Cycles** tab with the same settings. Like Houdini's other
renderer tabs, each setting has the usual Edit Properties control next to it (set, do nothing,
block), and only settings you set are authored. Use this when you already have a render settings
LOP in your pipeline, or want to author only a few Cycles settings over existing render settings.

### Viewport display options

The viewport's **Display Options** have a Cycles section with the settings that matter for
interactive rendering: Device, Open Shading Language, the main sampling and denoise settings,
the bounce limits, Curves Shape, Texture Size Limit, Motion Blur and Start at Low Resolution. The
denoise toggle is also on the viewport toolbar. The viewport also reads the render settings prim of
the stage (the Cycles Render Settings LOP sets itself as the default).

### Other places that author Cycles attributes

- Per-object settings: the **Cycles** tab of the Render Geometry Settings LOP, see
  [objects.md](objects.md).
- Per-light settings: the **Cycles** tab of the Light LOP, see [lights.md](lights.md).
- Material settings: the **Settings** tab of the USD Cycles Material Builder, see
  [materials.md](materials.md).

## Settings that restart the render

Most settings change the render while it runs. These settings are fixed when Cycles creates its
render session, and Houdini restarts the viewport render when they change (they are listed as
`restartrendersettings` in the plugin's `UsdRenderers.json`):

| Setting | USD attribute |
|---|---|
| Device | `cycles:device` |
| Threads | `cycles:threads` |
| Open Shading Language | `cycles:shading_system` |
| Start at Low Resolution | `cycles:use_resolution_divider` |
| Curves Shape | `cycles:curve_shape` |
| Texture Cache, Auto Generate, Cache Directory | `cycles:use_texture_cache`, `cycles:auto_texture_cache`, `cycles:texture_cache_path` |
| Texture Size Limit | `cycles:texture_limit` |
| Curve Subdivisions | `cycles:curve_subdivisions` |
| Pixel Size, Tile Size | `cycles:pixel_size`, `cycles:tile_size` |
| Use Spatial Splits, Use Compact BVH | `cycles:use_spatial_splits`, `cycles:use_compact_bvh` |

Motion blur, the camera shutter and Motion Steps change without a restart.

**Threads** only applies to `husk`. In the Houdini viewport, Cycles shares the process with Houdini
and always uses all cores, because a thread limit would apply to the whole Houdini process.

## Reference

The tables below list every setting, panel by panel, in the order of the UI. The **USD attribute**
is the attribute authored on the render settings prim; use it when you author settings with other
tools (for example a Python LOP or a USD file written by hand). Menu values are authored as tokens.

Settings marked *Also in the viewport display options* appear in the viewport's Display Options as
well.

### Device

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Device | `cycles:device` | Automatic (Best GPU) | Device to render with. Automatic uses the best available GPU (OptiX, then CUDA, HIP, oneAPI, Metal) and falls back to the CPU. Choices: Automatic (Best GPU) (`AUTO`), GPU - OptiX (`OPTIX`), GPU - CUDA (`CUDA`), GPU - HIP (`HIP`), GPU - oneAPI (`ONEAPI`), CPU (`CPU`). Also in the viewport display options. **Restarts the render.** |
| Open Shading Language | `cycles:shading_system` | Off | Use Open Shading Language. On the GPU only OptiX supports it. Also in the viewport display options. **Restarts the render.** |

### Sampling > Render

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Noise Threshold | `cycles:integrator:use_adaptive_sampling` | On | Automatically reduce the number of samples per pixel based on estimated noise level. Also in the viewport display options. |
| Threshold | `cycles:integrator:adaptive_threshold` | 0.01 | Noise level step to stop sampling at, lower values reduce noise at the cost of render time. 0 for automatic setting based on the number of samples. Also in the viewport display options. |
| Max Samples | `cycles:samples` | 1024 | Number of samples to render for each pixel. Also in the viewport display options. |
| Min Samples | `cycles:integrator:adaptive_min_samples` | 0 | Minimum number of samples before adaptive sampling stops a pixel. 0 for automatic setting based on the noise threshold. Also in the viewport display options. |
| Time Limit | `cycles:time_limit` | 0 | Limit the render time in seconds, excluding synchronization time. 0 disables the limit. |

### Sampling > Render > Denoise

The **Denoise** toggle in the panel header turns the panel on; the other settings in it only apply when it is on.

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Denoise | `cycles:integrator:use_denoise` | Off | Denoise the rendered image. Also in the viewport display options. |
| Denoiser | `cycles:integrator:denoiser_type` | OpenImageDenoise | Denoiser to use. OptiX requires an NVIDIA GPU. DLSS Ray Reconstruction denoises the viewport, it requires an NVIDIA RTX GPU and driver 620 or newer; final renders and other GPUs use OpenImageDenoise or the fastest available denoiser instead. Choices: OpenImageDenoise (`openimagedenoise`), OptiX (`optix`), DLSS (`dlss`). Also in the viewport display options. |
| Prefilter | `cycles:integrator:denoiser_prefilter` | Accurate | Prefilter of the guiding passes (albedo and normal). Accurate gives the best detail, None should only be used when the guiding passes are noise free. Choices: None (`none`), Fast (`fast`), Accurate (`accurate`). Also in the viewport display options. |
| Quality | `cycles:integrator:denoiser_quality` | High | Quality of the OpenImageDenoise denoiser, balanced and fast trade quality for speed. Choices: High (`high`), Balanced (`balanced`), Fast (`fast`). Also in the viewport display options. |
| Start Sample | `cycles:integrator:denoise_start_sample` | 0 | Sample to start denoising the interactive render at. Also in the viewport display options. |
| Use GPU | `cycles:integrator:denoise_use_gpu` | On | Run the denoiser on the GPU when available. Also in the viewport display options. |

### Sampling > Path Guiding

The **Path Guiding** toggle in the panel header turns the panel on; the other settings in it only apply when it is on.

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Path Guiding | `cycles:integrator:use_guiding` | Off | Use path guiding for sampling paths. Path guiding incrementally learns the light distribution of the scene and guides paths into directions with high direct and indirect light contributions. Only supported on the CPU. |
| Training Samples | `cycles:integrator:guiding_training_samples` | 128 | The maximum number of samples used for training path guiding. 0 trains for all samples. |
| Surface | `cycles:integrator:use_surface_guiding` | On | Use guiding when sampling directions on a surface. |
| Volume | `cycles:integrator:use_volume_guiding` | On | Use guiding when sampling directions inside a volume. |

### Sampling > Path Guiding > Debug

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Distribution Type | `cycles:integrator:guiding_distribution_type` | Parallax-Aware VMM | Type of representation for the guiding distribution. Choices: Parallax-Aware VMM (`PARALLAX_AWARE_VMM`), Directional Quad Tree (`DIRECTIONAL_QUAD_TREE`), VMM (`VMM`). |
| Roughness Threshold | `cycles:integrator:guiding_roughness_threshold` | 0.05 | The minimal roughness value of a material to apply guiding. |
| Directional Sampling Type | `cycles:integrator:guiding_directional_sampling_type` | Re-sampled Importance Sampling | Type of the directional sampling used for guiding. Choices: Diffuse Product MIS (`MIS`), Re-sampled Importance Sampling (`RIS`), Roughness-based (`ROUGHNESS`). |
| Surface Guiding Probability | `cycles:integrator:surface_guiding_probability` | 0.5 | The probability of guiding a direction on a surface. |
| Volume Guiding Probability | `cycles:integrator:volume_guiding_probability` | 0.5 | The probability of guiding a direction inside a volume. |
| Deterministic | `cycles:integrator:deterministic_guiding` | On | Make path guiding deterministic, which means multiple renders with the same settings and scene give the same result. Rendering may be slower. |
| Guide Direct Light | `cycles:integrator:use_guiding_direct_light` | On | Consider the contribution of directly visible light sources during guiding. |
| Use MIS Weights | `cycles:integrator:use_guiding_mis_weights` | On | Use the MIS weight to weight the contribution of directly visible light sources during guiding. |

### Sampling > Advanced

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Pattern | `cycles:integrator:sampling_pattern` | Tabulated Sobol | Random sampling pattern used by the integrator. Choices: Tabulated Sobol (`tabulated_sobol`), Sobol-Burley (`sobol_burley`), Blue-Noise (pure) (`blue_noise_pure`), Blue-Noise (round) (`blue_noise_round`), Blue-Noise (first) (`blue_noise_first`). |
| Seed | `cycles:integrator:seed` | 0 | Seed value for the random number generator, to get different noise patterns. |
| Scrambling Distance | `cycles:integrator:scrambling_distance` | 1 | Reduce the randomization between pixels to improve GPU rendering performance, at the cost of possible rendering artifacts if set too low. |
| Min Light Bounces | `cycles:integrator:min_bounce` | 0 | Minimum number of light bounces. Setting this higher reduces noise in the first bounces, but can also be less efficient for more complex geometry like hair. |
| Min Transparent Bounces | `cycles:integrator:transparent_min_bounce` | 0 | Minimum number of transparent bounces. Setting this higher reduces noise in the first bounces, but can also be less efficient for more complex geometry like hair. |

### Sampling > Advanced > Sample Subset

The **Sample Subset** toggle in the panel header turns the panel on; the other settings in it only apply when it is on.

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Sample Subset | `cycles:integrator:use_sample_subset` | Off | Render a subset of the specified max samples. Typically used for distributed rendering across multiple devices. |
| Offset | `cycles:integrator:sample_subset_offset` | 0 | 0-based index of sample to start rendering from. |
| Length | `cycles:integrator:sample_subset_length` | 16777216 | The number of samples to render in this subset. |

### Sampling > Lights

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Light Tree | `cycles:integrator:use_light_tree` | On | Sample multiple lights more efficiently based on estimated contribution at every shading point. |
| Light Threshold | `cycles:integrator:light_sampling_threshold` | 0 | Probabilistically terminate light samples when the light contribution is below this threshold (more noise but faster rendering). 0 disables the test. |

### Light Paths > Max Bounces

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Total | `cycles:integrator:max_bounce` | 7 | Total maximum number of bounces. Also in the viewport display options. |
| Diffuse | `cycles:integrator:max_diffuse_bounce` | 7 | Maximum number of diffuse reflection bounces, bounded by total maximum. Also in the viewport display options. |
| Glossy | `cycles:integrator:max_glossy_bounce` | 7 | Maximum number of glossy reflection bounces, bounded by total maximum. Also in the viewport display options. |
| Transmission | `cycles:integrator:max_transmission_bounce` | 7 | Maximum number of transmission bounces, bounded by total maximum. Also in the viewport display options. |
| Volume | `cycles:integrator:max_volume_bounce` | 7 | Maximum number of volumetric scattering events. Also in the viewport display options. |
| Transparent | `cycles:integrator:transparent_max_bounce` | 7 | Maximum number of transparent bounces. This is independent of maximum number of other bounces. Also in the viewport display options. |

### Light Paths > Clamping

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Direct Light | `cycles:integrator:sample_clamp_direct` | 0 | If non-zero, the maximum value for a direct sample, higher values will be scaled down to avoid too much noise and slow convergence at the cost of accuracy. |
| Indirect Light | `cycles:integrator:sample_clamp_indirect` | 10 | If non-zero, the maximum value for an indirect sample, higher values will be scaled down to avoid too much noise and slow convergence at the cost of accuracy. |

### Light Paths > Caustics

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Filter Glossy | `cycles:integrator:filter_glossy` | 1 | Adaptively blur glossy shaders after blurry bounces, to reduce noise at the cost of accuracy. |
| Reflective | `cycles:integrator:caustics_reflective` | On | Use reflective caustics, resulting in a brighter image (more noise but added realism). |
| Refractive | `cycles:integrator:caustics_refractive` | On | Use refractive caustics, resulting in a brighter image (more noise but added realism). |

### Light Paths > Fast GI Approximation

The **Fast GI Approximation** toggle in the panel header turns the panel on; the other settings in it only apply when it is on.

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Fast GI Approximation | `cycles:fast_gi` | Off | Approximate diffuse indirect light with background tinted ambient occlusion. This provides a fast alternative to full global illumination, for interactive viewport rendering or final renders with reduced quality. |
| Method | `cycles:fast_gi_method` | Replace | Fast GI approximation method. Replace: replace global illumination with ambient occlusion after a specified number of bounces. Add: add ambient occlusion to diffuse surfaces. Choices: Replace (`replace`), Add (`add`). |
| Bounces | `cycles:ao_bounces` | 1 | After this number of light bounces, use approximate global illumination. 0 disables this feature. |
| AO Factor | `cycles:ao_factor` | 1 | Factor for ambient occlusion blending. |
| AO Distance | `cycles:integrator:ao_distance` | Unlimited | Distance from shading point to trace rays for the approximation. |

### Volumes

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Biased | `cycles:integrator:volume_ray_marching` | Off | Use the biased ray marching volume rendering instead of the unbiased null scattering method. |
| Step Rate | `cycles:integrator:volume_step_rate` | 1 | Globally adjust detail for volume rendering, on top of automatically estimated step size. Higher values reduce render time, lower values render with more detail. |
| Max Steps | `cycles:integrator:volume_max_steps` | 1024 | Maximum number of steps through the volume before giving up, to avoid extremely long render times with big objects or small step sizes. |

### Curves

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Shape | `cycles:curve_shape` | Rounded Ribbons | Form of curves. Rounded Ribbons render curves as flat ribbons with rounded normals, for fast rendering. 3D Curves render curves as circular 3D geometry, for accurate results when viewing closely. Choices: Rounded Ribbons (`ribbon`), 3D Curves (`thick`). Also in the viewport display options. **Restarts the render.** |
| Curve Subdivisions | `cycles:curve_subdivisions` | 3 | Number of subdivisions used in curve intersection, as a power of 2 (Blender uses 2). **Restarts the render.** |

### Subdivision

These settings apply to objects with **Adaptive Subdivision** (Render Geometry Settings LOP, see
[objects.md](objects.md#subdivision-1)). Changing them dices the meshes again without restarting the
render.

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Dicing Rate Render | `cycles:dicing_rate` | 1 | Size of a micropolygon in pixels for final renders, for objects with Adaptive Subdivision. |
| Viewport | `cycles:preview_dicing_rate` | 8 | Size of a micropolygon in pixels in the viewport, for objects with Adaptive Subdivision. Also in the viewport display options. |
| Offscreen Scale | `cycles:offscreen_dicing_scale` | 4 | Multiplier for the dicing rate of geometry outside of the camera view. The dicing rate of objects is gradually increased the further they are outside the camera view. Lower values provide higher quality reflections and shadows for off screen objects, while higher values use less memory. |
| Max Subdivisions | `cycles:max_subdivisions` | 12 | Stop subdividing when this level is reached even if the dice rate would produce finer micropolygons. |
| Dicing Camera | `cycles:dicing_camera` | (empty) | Camera prim to use as reference point when subdividing geometry, useful to avoid crawling artifacts in animations when the scene camera is moving. Empty uses the render camera. Meshes are diced at the render resolution. |

### Simplify

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Texture Size Limit | `cycles:texture_limit` | No Limit | Limit the size of image textures, for less memory use. Choices: No Limit (`off`), 128 (`128`), 256 (`256`), 512 (`512`), 1024 (`1024`), 2048 (`2048`), 4096 (`4096`), 8192 (`8192`). Also in the viewport display options. **Restarts the render.** |

### Motion Blur

The **Motion Blur** toggle in the panel header turns the panel on; the other settings in it only apply when it is on.

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Motion Blur | `cycles:integrator:motion_blur` | Off | Render motion blur over the shutter of the camera (Shutter Open and Close on the Camera LOP, in frames), from animated transforms, points and instances, velocities and accelerations of points and instances, and velocity fields of volumes (vel, v or velocity). The Vector pass is empty with motion blur, like in Blender. Also in the viewport display options. |
| Motion Steps | `cycles:motion_steps` | 1 | Accuracy of motion blur of transforms, points and instances: more steps use more memory (2^steps + 1 motion samples over the shutter, like Blender's object motion steps). |

### Film

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Exposure | `cycles:film:exposure` | 1 | Image brightness scale. |

### Film > Pixel Filter

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Type | `cycles:film:filter_type` | Box | Pixel filter type. Choices: Box (`box`), Gaussian (`gaussian`), Blackman-Harris (`blackman_harris`). |
| Width | `cycles:film:filter_width` | 1 | Pixel filter width. |

### Film > Transparent

The **Transparent** toggle in the panel header turns the panel on; the other settings in it only apply when it is on.

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Transparent | `cycles:background:transparent` | Off | World background is transparent, for compositing the render over another background. When not set, the background is transparent unless the scene has a dome light. |
| Transparent Glass | `cycles:background:transparent_glass` | Off | Render transmissive surfaces as transparent, for compositing glass over another background. |
| Roughness Threshold | `cycles:background:transparent_roughness_threshold` | 0 | For transparent transmission, keep surfaces with roughness above the threshold opaque. |

### Passes

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Alpha Threshold | `cycles:film:pass_alpha_threshold` | 0.5 | Z, Index, normal, UV and vector passes are only affected by surfaces with alpha transparency equal to or higher than this threshold. |

### Passes > Mist Pass

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Start | `cycles:film:mist_start` | 0 | Starting distance of the mist, measured from the camera. |
| Depth | `cycles:film:mist_depth` | 100 | Distance over which the mist effect fades in. |
| Falloff | `cycles:film:mist_falloff` | 1 | Type of transition used to fade mist (Blender: 1 linear, 2 quadratic, 0.5 inverse quadratic). |

### Performance > Threads

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Threads | `cycles:threads` | 0 | Number of CPU threads to use. 0 uses all cores. **Restarts the render.** |

### Performance > Viewport

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Start at Low Resolution | `cycles:use_resolution_divider` | On | Render the first samples after a change at a lower resolution, for faster feedback while navigating. Also in the viewport display options. **Restarts the render.** |
| Pixel Size | `cycles:pixel_size` | 1x | Pixel size for viewport rendering, larger pixels render faster. Final renders always use full resolution. Choices: `1`, `2`, `4`, `8`. Also in the viewport display options. **Restarts the render.** |

### Performance > Memory

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Tile Size | `cycles:tile_size` | 2048 | Render high resolution images in tiles of this size, to reduce memory usage. Tiles are cached to disk while rendering. Final renders only (the viewport never tiles). **Restarts the render.** |

### Performance > Acceleration Structure

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Use Spatial Splits | `cycles:use_spatial_splits` | Off | Use BVH spatial splits: longer build time, faster render. CPU only (Embree). **Restarts the render.** |
| Use Compact BVH | `cycles:use_compact_bvh` | On | Use a compact BVH structure: uses less memory but renders slower. CPU only (Embree). Blender turns it off (Blender Defaults button). **Restarts the render.** |

### Performance > Texture Cache

The **Texture Cache** toggle in the panel header turns the panel on; the other settings in it only apply when it is on.

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Texture Cache | `cycles:use_texture_cache` | On | Use tiled and mipmapped .tx files, loading only the tiles and mip levels the render needs: .tx, tiled and mipmapped textures are used directly, other images use the cache files of the Cache Directory or of a blender_tx folder next to them when those exist. **Restarts the render.** |
| Auto Generate | `cycles:auto_texture_cache` | Off | Generate missing cache files when rendering. **Restarts the render.** |
| Cache Directory | `cycles:texture_cache_path` | (empty) | Directory of the cache files, absolute or relative to each image. Empty uses a blender_tx folder next to each image. **Restarts the render.** |

### Notes on the reference

- **Path guiding:** the Path Guiding panel is present, but the shareable build is made without path
  guiding, so these settings currently have no effect. Path guiding is CPU-only in Cycles.
- **Transparent background:** when `cycles:background:transparent` is not authored, the background
  is transparent unless the scene has a dome light, like other Hydra renderers. Authoring it, on or
  off, overrides that automatic choice.
- **Fast GI:** the Fast GI settings (`cycles:fast_gi`, `cycles:fast_gi_method`, `cycles:ao_bounces`,
  `cycles:ao_factor`) are mapped onto Cycles' integrator AO settings like Blender does. Scenes saved
  with older versions of the Cycles Render Settings LOP authored the integrator attributes
  `cycles:integrator:ao_bounces`, `cycles:integrator:ao_factor` and
  `cycles:integrator:ao_additive_factor` directly; they are converted when the scene is loaded, and
  the delegate ignores them when Fast GI settings are authored.
- **Passes:** on the Cycles Render Settings LOP, the Passes folder also holds the pass toggles,
  Cryptomatte, shader AOVs, primvar AOVs, light groups and Split Render Product by Render Var.
  They create render vars rather than render settings; see [aovs.md](aovs.md).
- **Volume Step Rate and Max Steps** are only shown when **Biased** is on.
- **Denoiser options:** Prefilter, Start Sample and Use GPU are hidden for DLSS, and Quality is only
  shown for OpenImageDenoise. See [rendering.md](rendering.md#denoising) for how the denoisers
  behave.
- **Device menu:** HIP, oneAPI (and Metal on macOS) are listed, but only CUDA, OptiX and the CPU are
  enabled in this build; a device that is not available falls back to the CPU.
- **Distances** such as Mist Start and Depth and the AO Distance are in stage units (the scene is not
  scaled by `metersPerUnit`).

### Bake

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Bake Object | `cycles:bake_object` | (empty) | Mesh prim to bake. The render is then the mesh's UV space instead of the camera view, like Blender's Bake: each pixel shades the point of the surface that covers it in the UV map, at the render resolution. Empty renders the camera view. |
| Instance | `cycles:bake_instance` | 0 | Instance of an instanced Bake Object to bake: its instance index in the point instancer (the `instanceId` AOV). |
| UV Map | `cycles:bake_uv_map` | (empty) | UV primvar the image maps, like Blender's active UV map (e.g. `uv2`); the mesh exports it for the bake even when no shader reads it. Empty uses the default UV map (`st`, `uv`, then the first texture coordinate primvar). Tangent space normals use per triangle tangents for maps other than the default one. |
| Selected to Active | `cycles:bake_use_selected_to_active` | off | Bakes the shading of the Source prims onto the Bake Object, like Blender's Selected to Active: a ray from each pixel's point of the Bake Object goes inwards along its normal (smooth faces: the interpolated vertex normal) and the Source surface it hits is shaded. |
| Source | `cycles:bake_source` | (empty) | Prims baked from (Blender's selected objects): prim paths with the meshes below them, or USD collections (`/prim.collection:name`), separated by spaces. The Bake Object itself is skipped. |
| Cage Object | `cycles:bake_cage_object` | (empty) | Mesh the rays start from, with the same triangles as the Bake Object; rays go from the cage point to the Bake Object point. Empty uses the Extrusion. |
| Extrusion | `cycles:bake_cage_extrusion` | 0 | Inflates the Bake Object along its normals by this distance (world units) for the ray starts, so Source surfaces outside it are hit. |
| Max Ray Distance | `cycles:bake_max_ray_distance` | 0 | Longest distance from the ray start to the Source surface, 0 is no limit. |
| Normal Space | `cycles:bake_normal_space` | Tangent | Space of baked Normal render vars: Tangent (a normal map: tangent along the UV map's U direction, bitangent along V, normal, mapped from -1..1 to 0..1 like Blender's), Object, or World (Cycles' normal pass). Tangents are MikkTSpace tangents, like Blender's. |
| Margin | `cycles:bake_margin` | 16 | Extends the baked result around the UV islands, in pixels, so seams do not show (Blender's Extend margin). Applied to the final image (and husk's updates), not to progressive viewport updates. ID AOVs get no margin. |

**Cycles Bake (Setup)** in the LOP Tab menu (Cycles and Rendering) adds a Cycles Render Settings LOP
that bakes the prim selected in the viewer at 2048 x 2048 with Diffuse Color and Normal render vars,
and a USD Render ROP for it.

Every render var is baked: the beauty gives a full bake (Blender's Combined), Diffuse Color or
Glossy Color give albedo maps, Normal gives normals (see Normal Space), Emit, AO, Position, shader AOVs
and light groups work too. Pixels outside the UV islands have zero alpha. Render it with husk or
the USD Render ROP as usual (the camera is still needed by the render settings but not used).

Selected to Active casts the rays on the CPU (Embree) when the render starts, also for GPU renders. Its pixels are not anti-aliased (no ray differentials, like Blender's). The Bake Object stays in the scene: hide it from shadow and indirect rays (Cycles object visibility) when it encloses the Source. Instanced prims under a Source path are matched by their Hydra prim path.

## Settings without a UI

The delegate reads a few more settings that have no parameter in the generated UI:

| USD attribute | Type | Purpose |
|---|---|---|
| `cycles:sample_offset` | int | Sample to start from; a value above 0 renders a subset of the samples, from that sample on |
| `cycles:integrator:<socket>`, `cycles:film:<socket>`, `cycles:background:<socket>` | any | Every socket of Cycles' Integrator, Film and Background nodes can be set by its Cycles name, also those that are not in the UI |
| `disableMotionBlur` | bool | USD's render settings attribute: turns motion blur off for the render |
| `shutter:open`, `shutter:close` | double | Shutter interval in frames, relative to the current frame. The Houdini viewport and `husk` pass the camera's shutter this way; without these settings the first camera with an open shutter is used |

Houdini also passes `houdini:viewport`, `houdini:fps` and the stage's `metersPerUnit` to the
delegate itself; there is nothing to set for these.
