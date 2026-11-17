# Motion blur

Cycles renders motion blur from the motion that USD describes: animated transforms, deforming
points, velocities, instancers, volume velocity fields and the camera. It is off by default.

## Turning it on

1. Turn on **Motion Blur** in the render settings (Motion Blur panel, `cycles:integrator:motion_blur`).
   It is also in the viewport's display options.
2. Set the shutter on the camera: **Shutter Open** and **Shutter Close** on the Camera LOP, in
   frames relative to the current frame (for example -0.25 and 0.25 for a 180 degree shutter
   centered on the frame).

Turning motion blur on or off, changing the shutter and changing Motion Steps update the render
without restarting it. The USD render settings attribute `disableMotionBlur` turns motion blur off
too.

## What blurs

| Motion | Source |
|---|---|
| Object transforms | Time samples of the transforms over the shutter |
| Deforming meshes, curves and points | Time samples of the points, or `velocities` (and `accelerations`) |
| Point instancers, also nested | Time samples, or `velocities` and `angularVelocities` of the instances |
| Volumes | A velocity field (`vel`, `v`, `velocity`, or native `vel.x/y/z`), see [volumes.md](volumes.md#motion-blur) |
| Camera | Time samples of the camera transform |

Velocities are per second, as Houdini writes them; the delegate uses the scene's frame rate from
Houdini to turn them into motion over the shutter. Houdini's attribute names (`v`, `accel`) work as
well as the USD names.

### Motion needs time samples

Cycles only sees what is in the USD stage at the shutter times. A transform animated with an
expression on a LOP parameter only exists at the current frame, so it does not blur. As for Karma,
cache the motion first with a **Motion Blur LOP** (or a Cache LOP), or bring in animated geometry
from SOPs with time samples or velocities.

## Motion Steps

**Motion Steps** (`cycles:motion_steps`, default 1, range 1 to 7) sets how many motion samples
Cycles uses for transforms, points and instances over the shutter: 2^steps + 1 samples, like
Blender's object Motion Steps. The default gives 3 samples, enough for straight motion. Raise it
for fast rotation or curved paths (a spinning wheel, a swinging arm); each step uses more memory.
The samples are evenly spaced over the shutter, with the middle one at the shutter center.

## Limits

- Curve and point **widths** are not blurred, only their positions.
- Instancers whose number of instances changes within the shutter get no motion blur.
- A volume field named `vel` or `v` is always used as velocity.
- Animated focal length (zoom) does not blur and does not show in the Vector pass.

## The Vector pass

The **Vector** pass (Passes > Data > Vector on the Cycles Render Settings LOP, render var `Vector`)
gives Blender-style motion vectors for motion blur in compositing:

- It is only written when **Motion Blur is off**. With motion blur on, the pass is empty, like in
  Blender (a warning is printed).
- It holds the screen-space motion in pixels to the previous frame (first two channels) and to the
  next frame (last two channels), so it needs a 4-channel format; the LOP authors it as `float4`.
- It needs the scene at the previous and next frame. Animated geometry needs time samples there:
  cache a frame range around the rendered frame (for example with a Cache LOP).

Adding or removing the Vector pass while rendering updates the render without a restart.
