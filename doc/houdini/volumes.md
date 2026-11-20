# Volumes

Cycles renders USD volume prims: smoke, fire, clouds and other fog volumes, from files or straight
from SOPs. Volumes render on the CPU and the GPU, with motion blur from velocity fields.

## Volume sources

| Source | How it reaches Cycles |
|---|---|
| OpenVDB files (`.vdb`) | Volume prims with OpenVDB field assets, for example from a Volume LOP or a USD file |
| Houdini `.bgeo` volumes | Native Houdini volume fields (`houdiniFieldAsset`), converted to OpenVDB grids when loaded |
| VDB volumes made in SOPs | SOP Import or SOP Create LOPs: the grids are read directly from Houdini's memory and shared, not copied |
| Native Houdini volumes made in SOPs | Converted to OpenVDB grids when loaded |

Volumes made in SOPs reach the renderer as `op:` paths into the SOP network; the delegate gets
their grids from Houdini, so no files need to be written first.

## Shading volumes

A volume without a material renders with Cycles' default volume shader, which shows the `density`
field as grey smoke, like Karma does. A volume shader that reads no field (a constant absorption
or emission) fills the bounds of the density field (or of the first field), like Karma fills the
field's extent.

To shade a volume, assign a material with a volume shader:

- **Principled Volume** for smoke and fire: density, color, absorption, emission, blackbody
  (temperature) and the attribute names it reads (`density`, `color`, `temperature`, ...).
- **Volume Scatter**, **Volume Absorption** and **Volume Coefficients** for building volumes by hand.
- **Volume Info** reads the standard fields (color, density, flame, temperature), and the
  **Attribute** node reads any field by name.

In the USD Cycles Material Builder you can connect the volume shader to the surface input of the
output node: Houdini's builder authors every Cycles shader as the surface terminal, and the
delegate routes volume shaders to the volume input of the Cycles material.

Volume shaders also work on closed meshes (for example a Principled Volume on a box), which renders
the inside of the mesh as a volume.

### Field names

Fields are found by their names in the volume prim. Cycles' standard names are `density`, `color`,
`flame`, `heat`, `temperature` and `velocity`. Houdini's velocity names are understood:

| Field in Houdini | Used as |
|---|---|
| `vel` (vector grid) | `velocity` |
| `vel.x`, `vel.y`, `vel.z` (native volumes, bound as `vel_x`, `vel_y`, `vel_z`) | `velocity`, merged into one vector |
| `v` (for example from rasterized points) | `velocity` |

A shader can still read a velocity field by its own name. Note that a field named `vel` or `v` is
always treated as velocity.

Constant primvars on the volume prim are also readable in shaders as attributes; a field with the
same name wins.

## Volume settings

Volume rendering is controlled in two places.

**Render settings, Volumes panel** (see [render-settings.md](render-settings.md#volumes)):

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Biased | `cycles:integrator:volume_ray_marching` | Off | Use biased ray marching instead of Cycles' default unbiased null scattering |
| Step Rate | `cycles:integrator:volume_step_rate` | 1 | With Biased on: global scale of the step size; higher is faster, lower is more detailed |
| Max Steps | `cycles:integrator:volume_max_steps` | 1024 | With Biased on: maximum number of steps through a volume |

The Light Paths panel's **Max Bounces > Volume** sets how many times light can scatter inside
volumes (default 7). More bounces brighten dense, bright volumes like clouds.

**Material settings, Volume panel** of the USD Cycles Material Builder's Settings tab (see
[materials.md](materials.md#the-settings-tab)):

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Sampling | `config:cycles:volume_sampling_method` | Multiple Importance | Distance, Equiangular or Multiple Importance |
| Interpolation | `config:cycles:volume_interpolation_method` | Linear | Linear or Cubic interpolation of the fields |
| Step Rate | `config:cycles:volume_step_rate` | 1 | Scale of the distance between volume shader samples for this material |

**Object settings, Volume panel** of the Render Geometry Settings LOP's Cycles tab (see
[objects.md](objects.md#volume)):

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Step Size | `primvars:cycles:object:volume_step_size` | 0 | With Biased on: distance between volume samples in object space; 0 computes it from the voxel size, like Blender's volume Step Size |

Lights contribute to volumes unless their **Volume Scatter** ray visibility is off, and objects can
be hidden from volume scattering with their own **Volume Scatter** toggle.

## Motion blur

With Motion Blur on, volumes with a velocity field blur along the velocity over the camera shutter.
Velocities are in units per second, as Houdini writes them, and the frame rate comes from Houdini.
The velocity field is only loaded while motion blur is on. Velocity blur is a deformation: it
follows the object's Deformation Samples, and Deformation Samples 1 turns it off (*untested*). See
[motion-blur.md](motion-blur.md).

## Limits

- MaterialX volume materials (`volumematerial` with `volume`, `anisotropic_vdf`, `absorption_vdf`
  and `uniform_edf`) are translated to Cycles' Volume Coefficients; see
  [materials.md](materials.md).
- Volume motion blur needs a velocity field; animated transforms of the volume prim blur like any
  other object.
