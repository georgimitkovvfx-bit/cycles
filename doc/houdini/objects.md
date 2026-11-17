# Objects and geometry

This page covers how USD geometry reaches Cycles: per-object Cycles settings, primvars that
shaders can read, instancing, implicit shapes, curves, points and subdivision.

## Supported geometry

| USD prim | Cycles geometry |
|---|---|
| Mesh | Triangle mesh, with optional subdivision; face sets (geometry subsets) with their own materials |
| BasisCurves | Curves (hair), rendered as ribbons or 3D curves |
| Points | Spheres, one per point, sized by `widths` |
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

### Visibility > Mask

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Shadow Catcher | `primvars:cycles:object:is_shadow_catcher` | Off | Only render shadows and reflections on this object, for compositing renders into real footage. Render the Shadow Catcher pass to get the shadows as a separate image (see [aovs.md](aovs.md)) |
| Holdout | `primvars:cycles:object:use_holdout` | Off | Render a hole in the image with transparent alpha instead of the object |

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

Curve width comes from `widths`. Cycles interpolates curve points with Catmull-Rom splines; B-spline
and Bezier bases are not converted and render as Catmull-Rom curves through the same control
points. Use the **Curves Info** node (`hair_info`) and the hair BSDFs (**Principled Hair BSDF**,
**Hair BSDF**) for hair shading.

## Points

Points render as spheres with the radius `widths / 2`. All primvar interpolations work on points.

## Subdivision

Meshes with the `subdivisionScheme` `catmullClark` (or `bilinear`) are subdivided by Cycles when
Hydra passes a refine level above 0 to the delegate; otherwise they render as their control mesh.
Creases, corners and boundary interpolation rules are passed to Cycles. There are no Cycles
dicing settings yet: subdivision uses a fixed dicing rate of 1.

For true displacement (material **Settings > Displacement**), the displaced detail is limited by
the mesh's resolution after subdivision.

## Units

Cycles works in meters. The delegate scales the scene by the stage's `metersPerUnit`, so stages in
meters and in centimeters render the same. Depth and Position passes are written in stage units,
like Karma's. Settings that are distances (for example AO Distance, Mist Start and Depth) are in
meters.
