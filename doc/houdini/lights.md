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

Mesh lights (the UsdLux mesh light API) and USD portal lights (`PortalLight`) are not supported;
use an emissive material on the geometry instead, and a rect light with the Cycles **Portal**
setting (below) as a portal.

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

Not read: shadow color, IES angle scale and normalization,
and Karma-specific light attributes (`karma:light:*`). Use the Cycles ray visibility settings below
for per-ray-type control.

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

### Light Group

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Light Group | `inputs:cycles:light:lightgroup` | (empty) | Light group the light belongs to. Render it with a light group pass on the Cycles Render Settings LOP, see [aovs.md](aovs.md#light-groups) |

A dome light's light group also applies to the world background, so the environment lighting ends
up in that group's pass.

### World

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| World Material | `inputs:cycles:light:world_material` | (empty) | Dome lights only: a material used as the Cycles world, like a Blender world. Its surface replaces the dome's color and texture, its volume fills the scene. See [world.md](world.md) |
| Map Resolution | `inputs:cycles:light:map_resolution` | 0 | Dome lights only: resolution of the importance map used to sample the world. 0 is Auto, the resolution of the environment texture (Blender's world Sampling: Auto) |

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
