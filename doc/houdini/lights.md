# Lights

Cycles renders the standard USD lights that Houdini's Light LOP, Dome Light LOP and Distant Light
LOP create. Their brightness matches Karma within 1%, so a scene lit for Karma looks the same in
Cycles. Emissive geometry (any material with an Emission shader or Principled BSDF emission) is a
light source too.

## Light types

| USD light | Cycles light | Notes |
|---|---|---|
| Sphere light | Point light, or spot light when shaping is used | `radius` in stage units; `treatAsPoint` (or radius 0) gives a point light. A shaping cone (`shaping:cone:angle`, `shaping:cone:softness`) makes it a spot light |
| Rect light | Rectangular area light | `width`, `height`; `texture:file` is mapped over the light |
| Disk light | Round area light | `radius` |
| Distant light | Sun light | `angle` (angular diameter in degrees) |
| Dome light | World background light | `texture:file` as a latitude-longitude environment map |
| Cylinder light | Mesh light | Cycles has no cylinder light: the side of the cylinder (along X, `radius`, `length`) is an emissive tube with the light's radiance, like USD's cylinder light (no light from the caps). Supports color, intensity, exposure, normalize, light linking, the Cycles tab's ray visibility, light group and Multiple Importance; no shadow, spread, IES or texture settings |

USD portal lights (`PortalLight`) are not supported; use a rect light with the Cycles **Portal**
setting (below) as a portal. Geometry lights (mesh lights) are described
[below](#geometry-lights).

## Instanced lights

Lights that are prototypes of a point instancer (an Instancer LOP whose prototype is a light, or
an Xform holding one) or that are inside native instances render as one light per instance, at
the light's transform times the instance's, like in Karma. The instances share the light's
settings: ray visibility, light group and light linking. Scaled instances of sphere lights and of
normalized lights get a light of their own, so their size and brightness follow the scale.

Limits: instanced lights are not motion blurred, and normalized cylinder lights do not follow a
per-instance scale.

## Geometry lights

Houdini's **Geometry Light** LOP (UsdLux `MeshLightAPI` or `LightAPI` on a mesh, curves or points
prim) makes the geometry itself a light, like an emissive mesh in Blender. Cycles renders it as a
mesh light (*untested*):

- **Material Sync Mode** No Material Response (the default of the Geometry Light LOP), or no bound
  material: the light's Color x Intensity x 2^Exposure (and color temperature) is the emission of
  the surface, replacing its material. **Normalize** divides it by the surface area.
- Material Glow Tints Light (the `MeshLightAPI` default) or Independent: the bound material is
  kept, and its emission lights the scene. The light's Intensity and Color are not applied then
  (exact for the defaults 1 and white).
- Light and shadow linking of the geometry light work like for other lights, and its Cycles tab's
  **Multiple Importance** and **Light Group** apply. A scene with only geometry lights gets no grey
  default world.
- Visibility to camera and other rays is the geometry's own (Render Geometry Settings LOP).

Limits: emission is two-sided; curves and points emit but are not normalized, and Cycles only
finds their light when rays hit them (no light sampling); volumes with `VolumeLightAPI` keep their
own shader. Diffuse and specular multipliers, Karma's light contributions and shadow color do not
apply to geometry lights. Normalized instances all use the area of the first instance.

## Intensity

The delegate converts the USD light attributes so that lights match Karma, both with and without
`normalize`, on meter and centimeter stages:

- The emitted color is `color` x `intensity` x 2^`exposure`.
- **Normalize** (`normalize`) makes the light's power independent of its size, as in USD.
- **Color temperature** (`enableColorTemperature`, `colorTemperature`) multiplies the color with a
  blackbody color.
- **IES profiles** (`shaping:ies:file`) shape the light's emission.
- **Shadows** can be turned off with `shadow:enable`.
- **Visibility:** an invisible light (USD visibility `invisible`) is switched off.

- **Diffuse and specular multipliers:** Cycles lights have no multipliers. A `diffuse` or
  `specular` of 0 turns off the light's diffuse or glossy contribution (like the ray visibility
  settings below); other values are ignored.

Not read: shadow color, IES angle scale and normalization, and most Karma-specific light
attributes (see [Karma light settings](#karma-light-settings)). Use the Cycles ray visibility
settings below for per-ray-type control.

## Cycles light settings

The Light LOP (and the other LOPs that create lights) gets a **Cycles** tab with Blender's light
settings. They are authored as `inputs:cycles:light:<setting>` attributes on the light. Unset
settings use Blender's defaults, except that lights are invisible to camera rays unless you turn
**Camera** on, like in Karma (in Blender lights are visible to the camera).

### Light

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Max Bounces | `inputs:cycles:light:max_bounces` | 1024 | Maximum number of bounces the light contributes to the render |
| Multiple Importance | `inputs:cycles:light:use_mis` | On | Use multiple importance sampling for the light; reduces noise for area lights and sharp glossy materials |
| Shadow Caustics | `inputs:cycles:light:use_caustics` | Off | Generate approximate caustics in shadows of refractive surfaces. The light, the caster and the receiver objects must all have their shadow caustics options on (see [objects.md](objects.md)) |
| Soft Falloff | `inputs:cycles:light:use_soft_falloff` | Off | Sphere lights only: apply falloff to avoid sharp edges where the light's sphere intersects other objects. Off by default so the light is a true sphere like in USD and Karma (in Blender it is on) |
| Spread | `inputs:cycles:light:spread` | 180 | Rect and disk lights only: how widely the light fans out, in degrees (like a gridded softbox). 180 emits over the whole front hemisphere |
| Portal | `inputs:cycles:light:is_portal` | Off | Rect and disk lights only: the light emits nothing itself but guides world (dome light) sampling through windows and openings, to reduce noise in interiors |

Blender's other light settings are the standard USD ones on the Light LOP's other tabs: power is
Intensity/Exposure, Radius/Size are Radius/Width/Height, Spot Size and Blend are Shaping > Cone
Angle and Cone Softness, Cast Shadow is Shadow > Enable.

### Visibility > Ray Visibility

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Camera | `inputs:cycles:light:visible_camera` | Off | Light visibility for camera rays (the light's shape seen directly by the camera). Unset, Karma's **Render Light Geometry** (`inputs:karma:light:renderlightgeo`) decides |
| Diffuse | `inputs:cycles:light:visible_diffuse` | On | Light contribution to diffuse reflection |
| Glossy | `inputs:cycles:light:visible_glossy` | On | Light contribution to glossy reflection |
| Transmission | `inputs:cycles:light:visible_transmission` | On | Light contribution to transmission |
| Volume Scatter | `inputs:cycles:light:visible_volume_scatter` | On | Light contribution to volume scattering |

### Karma light settings

These Karma light properties (Karma tab of the Light LOP) are used while the Cycles setting is not
authored, like Karma's Render Light Geometry for Camera:

| Karma setting | USD attribute | Cycles setting |
|---|---|---|
| Contributions | `inputs:karma:light:contribs` | Diffuse, Glossy (`reflect` and `coat`), Transmission (`refract`), Volume Scatter ray visibility; `sss` has no Cycles ray type. *Untested* |
| Spread | `inputs:karma:light:spread` | Spread: Karma's 0 (parallel) to 1 (diffuse) as 0 to 180 degrees, linearly (approximate between the ends). *Untested* |

### Light Group

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Light Group | `inputs:cycles:light:lightgroup` | (empty) | Light group the light belongs to. Render it with a light group pass on the Cycles Render Settings LOP, see [aovs.md](aovs.md#light-groups); **Light Group per Light** there gives every light without a group its own |

A dome light's light group also applies to the world background, so the environment lighting ends
up in that group's pass.

### World

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| World Material | `inputs:cycles:light:world_material` | (empty) | Dome lights only: a material used as the Cycles world, like a Blender world. Its surface replaces the dome's color and texture, its volume fills the scene. See [world.md](world.md) |
| Map Resolution | `inputs:cycles:light:map_resolution` | 0 | Dome lights only: resolution of the importance map used to sample the world. 0 is Auto, the resolution of the environment texture (Blender's world Sampling: Auto) |
| Exposure | `inputs:cycles:light:world_exposure` | 0 | Dome lights only: brightens or darkens the world in stops, the dome's own color and texture or its World Material (the strength of its Background shaders), on top of the light's Intensity and Exposure |
| Saturation | `inputs:cycles:light:world_saturation` | 1 | Dome lights only: saturation of the world (0 gray, 2 doubled), the dome's color or the color of the Background shaders of its World Material |
| Rotation | `inputs:cycles:light:world_rotation` | 0 | Dome lights only: turns the world around the light's +Y (its pole) in degrees, counterclockwise seen from above, on top of the light's transform (its texture, or the sky and environment textures of its World Material) |

Exposure, Saturation and Rotation are *untested*.

## Light and shadow linking

Cycles supports USD light linking and shadow linking, set up with Houdini's **Light Linker LOP**
(or by editing the light's `lightLink` and `shadowLink` collections directly), like for Karma:

- **Light linking** limits which objects a light illuminates.
- **Shadow linking** limits which objects cast shadows from a light.

The delegate turns the collections into Cycles light sets and shadow sets, like Blender's light
linking. The results match Karma.

Limits:

- Cycles supports at most 64 light linking sets and 64 shadow linking sets. A set is needed for
  each different combination of links that objects receive, not for each light.
- Point instances use the links of their prototype, not links set per instance.

## Dome lights

The dome light's `texture:file` is used as a latitude-longitude environment map, oriented like in
Karma: the top of the image is along the light's +Y axis and the center of the image along its +Z
axis. Rotate the dome light to rotate the environment. The `poleAxis` attribute of newer dome
lights (DomeLight_1 with a Z pole) is not read.

Without a texture, the dome light is a uniform color (`color`, `intensity`, `exposure`, color
temperature). A dome light makes the background opaque by default; see Film > Transparent in
[render-settings.md](render-settings.md#notes-on-the-reference).

A scene without any light renders with a uniform grey world (0.5), so that objects are visible;
as soon as the scene has a light, the default world is black.

**Viewport note:** with a dome light in the scene, Houdini's *Display Environment Maps* option
(on by default) draws the dome's texture behind the render. Where the render is transparent, you
see that texture instead of the render's alpha. Turn off Display Environment Maps in the viewport's
display options to check the alpha. With a dome light the viewport image also comes through a
slower path, so the low resolution start of each change is not shown.

## World / environment shaders

See [world.md](world.md).

## Moving and editing lights

Moving, rotating and editing lights updates the viewport render without a restart. Lights are
sampled with Cycles' light tree (Sampling > Lights > Light Tree, on by default), which keeps scenes
with many lights efficient.
