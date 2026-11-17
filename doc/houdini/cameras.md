# Cameras

Cycles renders USD cameras: perspective and orthographic projection, focal length, apertures and
their offsets, clipping range, depth of field (F-Stop and Focus Distance), the shutter for motion
blur and exposure. Blender's extra camera settings are on the **Cycles** tab of the Camera LOP.

## USD camera settings

| Camera LOP parameter | USD attribute | Cycles |
|---|---|---|
| Projection | `projection` | Perspective or orthographic camera (replaced by a Panorama Type, below) |
| Focal Length, Horizontal/Vertical Aperture and Offset | `focalLength`, `horizontalAperture`, ... | Field of view and view window, conformed to the image like Karma |
| Clipping Range | `clippingRange` | Near and far clipping |
| F-Stop, Focus Distance | `fStop`, `focusDistance` | Depth of field: aperture size = focal length / (2 F-Stop). F-Stop 0 turns it off |
| Shutter Open, Shutter Close | `shutter:open`, `shutter:close` | Motion blur shutter, see [motion-blur.md](motion-blur.md) |
| Exposure (and Exposure Time, ISO, F-Stop, Responsivity) | `exposure`, `exposure:time`, `exposure:iso`, `exposure:fStop`, `exposure:responsivity` | Image brightness: 2^exposure x time x ISO / 100 / fStop^2 x responsivity, multiplied with the film Exposure render setting, like Karma |

## Cycles settings

The Camera LOP's **Cycles** tab authors `cycles:camera:<setting>` attributes on the camera. Settings
that are not authored use the defaults below (Blender's). Angles are in degrees.

### Lens

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Panorama Type | `cycles:camera:panorama_type` | None | Render a panoramic image with this projection instead of the camera's perspective or orthographic projection: Equirectangular (`equirectangular`), Equiangular Cubemap Face (`equiangular_cubemap_face`), Mirror Ball (`mirrorball`), Fisheye Equidistant (`fisheye_equidistant`), Fisheye Equisolid (`fisheye_equisolid`), Fisheye Lens Polynomial (`fisheye_lens_polynomial`), Central Cylindrical (`panorama_central_cylindrical`) |
| Field of View | `cycles:camera:fisheye_fov` | 180 | Fisheye types: field of view of the lens |
| Fisheye Lens | `cycles:camera:fisheye_lens` | 10.5 | Fisheye Equisolid: focal length of the lens, in the unit of the aperture (millimeters) |
| Latitude Min, Max | `cycles:camera:latitude_min`, `latitude_max` | -90, 90 | Equirectangular: vertical range of the panorama |
| Longitude Min, Max | `cycles:camera:longitude_min`, `longitude_max` | -180, 180 | Equirectangular: horizontal range of the panorama |
| K0 .. K4 | `cycles:camera:fisheye_polynomial_k0` .. `k4` | Blender's | Fisheye Lens Polynomial: coefficients of the lens polynomial (from a lens calibration) |
| Min, Max Longitude | `cycles:camera:central_cylindrical_range_u_min`, `_u_max` | -180, 180 | Central Cylindrical: horizontal range |
| Min, Max Height | `cycles:camera:central_cylindrical_range_v_min`, `_v_max` | -1, 1 | Central Cylindrical: vertical range |

Panoramic cameras look down the camera's -Z axis like any USD camera; an equirectangular camera
sees all around it (render it at a 2:1 resolution). The fisheye types use the camera's aperture as
the sensor size, so a 36 x 24 aperture behaves like Blender's default sensor. Houdini's own camera
overlay (frustum, guides) still draws the perspective projection.

### Depth of Field > Aperture

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Blades | `cycles:camera:blades` | 0 | Number of blades in the aperture for polygonal bokeh (at least 3, 0 for a round bokeh). The aperture size comes from the camera's F-Stop |
| Rotation | `cycles:camera:blades_rotation` | 0 | Rotation of the blades |
| Ratio | `cycles:camera:aperture_ratio` | 1 | Distortion to simulate an anamorphic lens bokeh |

### Motion Blur > Rolling Shutter

| Setting | USD attribute | Default | Description |
|---|---|---|---|
| Rolling Shutter | `cycles:camera:rolling_shutter_type` | None | Type of rolling shutter effect matching CMOS-based cameras, with motion blur: None (`none`) or Top-Bottom (`top`) |
| Rolling Shutter Duration | `cycles:camera:rolling_shutter_duration` | 0.1 | Scanline "exposure" time for the rolling shutter effect: 0 scans the image over the whole shutter, 1 exposes all scanlines over the whole shutter |

## Limits

- Changing a Cycles camera setting while the viewport renders does not update the render yet:
  restart the render (viewport display options, or toggle the renderer) to see it.
- Stereo cameras, Blender's shutter curve and a separate dicing camera are not exposed.
- Lens distortion and split diopter (USD 26 camera schemas) are ignored.
- In the free viewport camera (not looking through a camera prim) the Cycles settings and the
  exposure do not apply.
