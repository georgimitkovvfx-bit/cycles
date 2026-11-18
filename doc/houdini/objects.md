# Objects and geometry

This page covers how USD geometry reaches Cycles: per-object Cycles settings, primvars that
shaders can read, instancing, implicit shapes, curves, points and subdivision.

## Supported geometry

| USD prim | Cycles geometry |
|---|---|
| Mesh | Triangle mesh, with optional subdivision; face sets (geometry subsets) with their own materials |
| BasisCurves | Curves (hair), rendered as ribbons or 3D curves |
| Points | Spheres, one per point, sized by `widths` |
| ParticleField3DGaussianSplat | Gaussian splats (see [Gaussian splats](#gaussian-splats)) |
| Volume | Volume (see [volumes.md](volumes.md)) |
| Sphere, Cube, Cone, Cylinder, Capsule, Plane | Implicit shapes, converted to meshes |
| PointInstancer, native instances | Cycles instances |

Cycles renders the default and render purposes by default (proxy and guide prims are left out);
the viewport's purpose toggles change which purposes are drawn, also while rendering. USD
visibility (`invisible`) hides a prim.

## Per-object settings

The **Render Geometry Settings LOP** gets a **Cycles** tab with Blender's object visibility and
shading options. The settings are authored as constant primvars named
`primvars:cycles:object:<setting>`. Like any constant primvar they are inherited, so you can set
them on a parent prim to affect everything below it. Settings that are not authored use their
defaults (shown below), and removing a setting returns the object to its default.

The tables are generated from the same layout as the UI.

### Relations

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Pass Index | `primvars:cycles:object:pass_id` | 0 | Index number for the Object Index pass (`IndexOB`) and the Object Index output of the Object Info node, like Blender's object Pass Index |

### Motion Blur

The **Motion Blur** toggle in the panel header turns the panel on; Steps only applies when it is on.

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Motion Blur | `primvars:cycles:object:use_motion_blur` | On | Use motion blur for this object, when the Motion Blur render setting is on. Off, the object renders sharp at the shutter center and has no motion vectors |
| Steps | `primvars:cycles:object:motion_steps` | 0 | Motion steps of this object: 2^steps + 1 motion samples over the shutter, for curved motion. 0 uses the Motion Steps render setting |

### Visibility > Ray Visibility

Turning off a ray type hides the object from that kind of ray, like Blender's object Ray
Visibility panel. For example, turn off **Camera** to hide an object from the camera while it still
casts shadows and shows in reflections.

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Camera | `primvars:cycles:object:visible_camera` | On | Object visibility for camera rays |
| Diffuse | `primvars:cycles:object:visible_diffuse` | On | Object visibility for diffuse reflection rays |
| Glossy | `primvars:cycles:object:visible_glossy` | On | Object visibility for glossy reflection rays |
| Transmission | `primvars:cycles:object:visible_transmission` | On | Object visibility for transmission rays |
| Volume Scatter | `primvars:cycles:object:visible_volume_scatter` | On | Object visibility for volume scatter rays |
| Shadow | `primvars:cycles:object:visible_shadow` | On | Object visibility for shadow rays (off: the object casts no shadows) |
| Raycast | `primvars:cycles:object:visible_raycast` | On | Object visibility for the rays of Raycast shader nodes |

Karma's **Render Visibility** (`primvars:karma:object:rendervisibility`, Karma tab of the Render
Geometry Settings LOP) also works, for the ray types without a Cycles setting: `primary` is
Camera, `shadow` Shadow, `diffuse` Diffuse, `reflect` Glossy, `refract` Transmission and `volume`
Volume Scatter, with Karma's patterns (`*`, `-shadow`, `primary|shadow`,
`-diffuse&-reflect&-refract&-volume&-sss`). Houdini's SOP Import sets `-shadow` on Gaussian splats.

### Visibility > Mask

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Shadow Catcher | `primvars:cycles:object:is_shadow_catcher` | Off | Only render shadows and reflections on this object, for compositing renders into real footage. Render the Shadow Catcher pass to get the shadows as a separate image (see [aovs.md](aovs.md)) |
| Holdout | `primvars:cycles:object:use_holdout` | Off | Render a hole in the image with transparent alpha instead of the object |

Karma's **Holdout Mode** (`primvars:karma:object:holdoutmode`) also works when these are not set:
Matte makes the object a Holdout, Background a Shadow Catcher.

### Visibility > Caustics

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Cast Shadow Caustics | `primvars:cycles:object:is_caustics_caster` | Off | With Manifold Next Event Estimation, cast shadow caustics from refractive surfaces onto caustics receivers |
| Receive Shadow Caustics | `primvars:cycles:object:is_caustics_receiver` | Off | With Manifold Next Event Estimation, receive shadow caustics from caustics casters |

Shadow caustics also need **Shadow Caustics** on the light (see [lights.md](lights.md)).

### Visibility > Fast GI Approximation

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| AO Distance | `primvars:cycles:object:ao_distance` | 0 | AO distance used for the Fast GI approximation (0 uses the setting of the render settings) |

### Shading > Shadow Terminator

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Geometry Offset | `primvars:cycles:object:shadow_terminator_geometry_offset` | 0.1 | Offset rays from the surface to reduce shadow terminator artifacts on low poly geometry. Only affects triangles at grazing angles to the light |
| Shading Offset | `primvars:cycles:object:shadow_terminator_shading_offset` | 0 | Push the shadow terminator towards the light to hide artifacts on low poly geometry |

### Shading > Light Group

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Light Group | `primvars:cycles:object:lightgroup` | (empty) | Light group the object's emission belongs to. Render it with a light group pass on the Cycles Render Settings LOP, see [aovs.md](aovs.md#light-groups) |

### Subdivision

The **Adaptive Subdivision** toggle in the panel header turns the panel on. See
[Subdivision](#subdivision-1) below.

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Adaptive Subdivision | `primvars:cycles:object:use_adaptive_subdivision` | Off | Subdivide subdivision surfaces to the dicing rate of the render settings in pixels of the render camera, up to Max Subdivisions, also without a refinement level. Polygon meshes (`subdivisionScheme` `none`) are diced flat (bilinear). Off, they are subdivided up to their refinement level |
| Dicing Scale | `primvars:cycles:object:dicing_rate` | 1 | Multiplier for the dicing rate of the render settings |

### Volume

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Step Size | `primvars:cycles:object:volume_step_size` | 0 | Distance between volume samples in object space, with Biased volume rendering. 0 computes it from the voxel size. See [volumes.md](volumes.md#volume-settings) |

**Limit:** per-object settings are read per prim, not per instance. All instances of a point
instancer prototype share the prototype's settings.

## Primvars in shaders

Every USD primvar can be read in a shader by name. This works with the Cycles **Attribute**,
**UV Map**, **Color Attribute**, **Normal Map** and **Tangent** nodes, the USD Primvar Reader,
MaterialX geometry nodes (`geompropvalue`, `texcoord`, `geomcolor`) and OSL `getattribute()`.

- **All interpolations** work on meshes (also subdivided), curves and points: constant, uniform,
  varying, vertex and face-varying. Volumes support constant primvars.
- **Types:** float, double, half, int and bool scalars, 2, 3 and 4 component vectors and colors,
  and arrays (with `elementSize`). Matrix, string and int64 primvars are not exported.
- **Houdini names work:** `Cd`, `Alpha`, `uv`, `v` and `accel` find `displayColor`,
  `displayOpacity`, `st`, `velocities` and `accelerations`, and the other way round. A shader can
  also ask for `primvars:<name>`.
- **UV maps:** `st` (or `uv`, or the first primvar with the texture coordinate role) is the default
  UV map, which the UV Map node, image textures and normal mapping use. Other UV sets are read by
  name.
- **Colors:** `displayColor` (or `Cd`) is the color attribute at every interpolation, which the
  Color Attribute node reads. A constant color also sets the Object Info node's Color.
- **Normals:** face-varying normals (Houdini vertex normals `N`) are used as corner normals, so
  hard edges render as authored.
- Only primvars that a shader reads are sent to Cycles, so unused primvars cost no memory. When an
  edited material starts reading a new primvar, the geometry is updated without a restart.

The **Primvar AOV** feature writes primvars to images without changing the materials, see
[aovs.md](aovs.md#primvar-aov).

## Instancing

Point instancers (from the Instancer LOP, or SOP Import of packed primitives) and native USD
instances render as Cycles instances, which share their geometry in memory. Nested point
instancers work, and the instances' orientation (`orientations`), scale (`scales`) and position
are applied in USD's order.

**Instance primvars:** per-point primvars of a point instancer, and constant primvars on native
instances, reach the shaders of the prototypes as attributes. When a primvar exists at several
levels, the prototype's own primvar wins, then the innermost instancer, like in Karma. A
per-instance `displayColor` (or `Cd`) also gives each instance its Object Info color.

Instance primvars are stored only for the names that shaders read.

## Implicit shapes

USD's implicit shapes (Sphere, Cube, Cone, Cylinder, Capsule and Plane) are converted to meshes:

- Spheres, cylinders, cones and capsules are smooth meshes with 64 segments and normals; a sphere
  of radius 1 matches the analytic size within 0.2%.
- The Cube has flat shaded faces.
- All shapes get texture coordinates (`st`) laid out like Blender's primitives: the sphere like a
  UV sphere, revolved shapes around their axis with planar caps, the cube in a cross layout. An
  authored UV primvar wins.

## Curves

Curves render in one of two shapes, chosen for the whole scene with Render Settings > Curves >
**Shape** (`cycles:curve_shape`):

| Shape | Use |
|---|---|
| Rounded Ribbons (default) | Flat ribbons facing the camera with rounded normals. Fast, good for hair seen from a distance |
| 3D Curves | Round tubes. Accurate for close-ups and when hair is seen from different angles |

Changing the shape restarts the render. The Blender Defaults button of the Cycles Render Settings
LOP sets 3D Curves.

Curve width comes from `widths` (constant, uniform, varying or vertex). Every USD curve type
renders with its own shape: linear curves as Cycles' linear 3D curves (with the 3D Curves shape),
cubic Bezier, B-spline and Catmull-Rom curves converted to Cycles' Catmull-Rom keys (Bezier curves
sampled 3 times per segment, B-splines twice), periodic curves closed and pinned curves through
their end points. Primvars follow the conversion. Use the **Curves Info** node (`hair_info`) and the hair BSDFs (**Principled Hair BSDF**,
**Hair BSDF**) for hair shading.

## Points

Points render as spheres with the radius `widths / 2`. All primvar interpolations work on points.

## Gaussian splats

`ParticleField3DGaussianSplat` prims (Houdini's SOP Import writes them for splats from the Bake
Gaussian Splats SOP or a PLY file) render as Cycles Gaussian splats: `positions`, `orientations`,
`scales` and `opacities` (float or half) and the spherical harmonics coefficients
(`radiance:sphericalHarmonicsCoefficients`, degree 0 to 3; higher degrees are cut at 3). Without
coefficients a splat is mid grey (USD's DC signal of 0.5). Cycles stores the degree 1 to 3
coefficients in 8 bits, clamped to [-1, 1]. Splats without a material use Cycles' default splat
shader (the splat's view-dependent radiance as emission); a bound material replaces it. Animated splat
positions (time samples) are motion blurred.

## Subdivision

Meshes with the `subdivisionScheme` `catmullClark` (or `bilinear`) are subdivided by Cycles. Creases,
corners and boundary interpolation rules are passed to Cycles. Cycles dices the surface into
micropolygons of a size in pixels of the camera you render through, or of the render settings'
**Dicing Camera** when it is set.

- **Without Adaptive Subdivision** (default), a mesh is subdivided when Hydra passes a refine level
  above 0 (Houdini's level of detail; `husk` passes one), up to that many levels, with micropolygons
  of 1 pixel. Otherwise it renders as its control mesh.
- **With Adaptive Subdivision** (Render Geometry Settings LOP, Cycles tab, Subdivision panel), the
  mesh is always subdivided, up to **Max Subdivisions** levels, with micropolygons of the
  **Dicing Rate** of the render settings (Render or Viewport) times the object's **Dicing Scale**.
  Geometry outside the camera view is diced more coarsely by **Offscreen Scale**. This is Blender's
  adaptive subdivision: detail where the camera sees it, for displacement-heavy assets. Polygon
  meshes (`subdivisionScheme` `none`, the SOP Import default unless **Import Polygons as
  Subdivision** is on) are diced too, with flat (bilinear) subdivision that keeps their shape,
  like Blender's Simple subdivision.

The dicing settings are in the render settings' **Subdivision** panel (see
[render-settings.md](render-settings.md#subdivision)). Changing them dices the meshes again without
restarting the render.

For true displacement (material **Settings > Displacement**), the displaced detail is limited by
the mesh's resolution after subdivision, so use Adaptive Subdivision with a small dicing rate.

## Units

The scene renders in stage units (since Cycles 5.4 the delegate no longer scales it by the stage's
`metersPerUnit`), so Depth and Position passes are in stage units, like Karma's, and settings that
are distances (for example AO Distance, Mist Start and Depth, volume step sizes) are in stage units
too. Camera apertures and focal lengths are converted with `metersPerUnit`.
