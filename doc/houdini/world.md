# World and environment shaders

In Blender, the *world* is a shader for everything a ray hits when it leaves the scene: the sky,
an HDRI, a color gradient. In USD and Houdini, the environment is a dome light. hdCycles joins
the two: a dome light can use any material as its **World Material**, and that material is then
the Cycles world. The material's surface is the background (lighting and what the camera sees),
its volume fills the whole scene.

This gives you everything a Blender world can do: Cycles' **Sky Texture** (physical sky with sun
disc), procedural backgrounds, mixed HDRIs, gradients by direction, light-path tricks (a different
background for the camera than for lighting), and world volumes.

For a ready-made sun and sky, use the **Cycles Sun & Sky** LOP (below).

## Using a material as the world

1. Build the material in a Material Library LOP with a **USD Cycles Material Builder**. Inside it,
   connect a **Background** shader (Tab menu: Cycles > Shader > Background) to the surface output.
   Feed its Color with a Sky Texture, an Environment Texture, or any other nodes. For a world
   volume, connect a volume shader (Volume Scatter, Volume Absorption, Principled Volume) to the
   volume output.
2. Create a Dome Light LOP. On its **Cycles** tab, open **World**, turn on **World Material** and
   pick the material's prim (the arrow button opens the prim picker), for example
   `/materials/world`.
3. Render. The dome light's own Color, Intensity, Exposure and Texture are not used while the world
   material is found; set the strength on the Background shader instead. If the path is empty or
   no material exists at that path, the dome light renders as usual.

The dome light still decides everything that is about the light: ray visibility (Camera,
Diffuse, Glossy, ... on its Cycles tab), Multiple Importance, Max Bounces, its light group (which
is also the world's light group), light linking, and whether it is visible at all.

Changes update the viewport render live: editing the world material's nodes, rotating the dome,
setting or clearing the World Material.

A material bound with an Assign Material LOP is not used as the world; only the World Material
setting is.

## Orientation

Houdini is Y-up. Sky Texture and Environment Texture nodes in a world material are oriented like
dome light textures:

- the zenith (top of the sky, top of a latlong HDRI) is the dome light's **+Y**,
- the center of a latlong HDRI is the dome light's **+Z**, like the dome light's own texture,
- rotating the dome light rotates them.

An Environment Texture in a world material therefore looks exactly like the same file on the dome
light's Texture parameter.

The delegate does this through the nodes' Texture Mapping, so Cycles can still sample the Sky
Texture's sun directly (low noise). Texture Mapping parameters you set yourself on these two nodes
in a world material are replaced. Other nodes see directions in Houdini's world space: a
Gradient Texture or a Separate XYZ on the ray direction has **Y** as up.

### Sky Texture sun direction

The Sky Texture's **Sun Rotation** turns the sun around +Y: 0 degrees puts it toward **+X**,
90 degrees toward **+Z** (seen from above, clockwise from +X). **Sun Elevation** is the angle
above the horizon. The Cycles Sun & Sky LOP converts compass directions for you.

## Cycles Sun & Sky LOP

Tab menu: **Cycles > Lighting > Cycles Sun & Sky**. It creates a dome light and a world material
with a Sky Texture through a Background shader, like Blender's default sky setup.

| Parameter | Default | Description |
|---|---|---|
| Light Path | `/lights/$OS` | The dome light. Its Cycles World Material points at the material below |
| Material Path | `/materials/$OS` | The world material (Sky Texture + Background) |
| **Sky** | | |
| Sky Type | Multiple Scattering | Multiple Scattering and Single Scattering are physically based (Nishita) skies with a sun disc; Hosek / Wilkie and Preetham are analytic models without one |
| Strength | 1 | Background strength. Physical skies are bright: lower the strength or the exposure (Render Settings > Film > Exposure) for a darker image |
| Altitude | 100 | Camera height above sea level in meters (physical skies) |
| Air, Aerosol, Ozone | 1, 1, 1 | Density of air molecules (blue sky), dust and water (haze), ozone (sunset color) |
| Turbidity | 2.2 | Haze of the analytic models |
| Ground Albedo | 0.3 | Ground color reflected into the sky (Hosek / Wilkie) |
| **Sun** | | |
| Sun Disc | On | Include the sun. It lights the scene; Cycles samples it directly |
| Sun Size | 0.545 | Angular diameter in degrees (the real sun is about 0.53) |
| Sun Intensity | 1 | Strength of the sun disc |
| Sun Position | Elevation and Azimuth | Place the sun by angles, or by location, date and time |
| Elevation, Azimuth | 15, 135 | Degrees above the horizon; compass direction clockwise from north |
| Latitude, Longitude | 48.8566, 2.3522 | Location (positive north and east) |
| Date (Y M D), Local Time | 2026 6 21, 12 | Local clock time in hours (13.5 = 1:30 pm) |
| UTC Offset | 2 | Time zone of the local time, daylight saving included |
| North Offset | 0 | Rotates the compass around +Y |
| **Fog** | | |
| Fog | Off | Fills a box around the scene with a scattering volume, lit by the sky and sun through its sides |
| Fog Path | `/fog/$OS` | The fog box (its material is `<path>_material`) |
| Size, Center | 1000, (0, 0, 0) | The fog box, in scene units. Keep the camera and the scene inside it |
| Color, Density, Anisotropy | 0.8, 0.002, 0 | The Volume Scatter shader of the fog; density per scene unit |

**Compass:** north is **-Z**, east **+X**, south **+Z**, west **-X** (with North Offset 0). In
Location and Time mode the sun position comes from NOAA's solar position equations, the same as
Blender's Sun Position add-on.

**Brightness:** a physical sky with the sun is bright. The sun disc has very high values: where
the camera sees it, a 16-bit (half float) image overflows to infinity. Lower Strength or the film exposure, or turn
off the Camera visibility of the dome light (its Cycles tab) and use a separate backdrop.

## World volumes

A volume shader on the world material's volume output fills the whole scene, like Blender's world
Volume output. Cycles treats the sky and sun as infinitely far away: **a world volume absorbs the
world background and distant lights completely**, any density above zero makes them black. Only
lights inside the scene (sphere, rect, disk lights, emissive objects) light a world volume. This is
how Cycles 5.x works in Blender too.

For fog lit by a sky or a distant light, use a fog box instead: a closed mesh (for example a Cube
around the scene) with the volume material bound to it. The Sun & Sky LOP's Fog option creates one.

## Limits

- Only Cycles shader networks work as world materials (USD Cycles Material Builder, or
  hand-authored `cycles_*` shaders with `outputs:cycles:surface`). MaterialX and Karma materials
  have no background closure.
- One world: with several dome lights, the first one found is the world (as without world
  materials).
- Karma does not read the World Material setting; for Karma the dome light renders with its own
  texture and color.
