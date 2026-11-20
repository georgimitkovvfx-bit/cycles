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
too, and so do its older name `instantaneousShutter` and Karma's `karma:global:enablemblur` off
(both *untested*).

## What blurs

| Motion | Source |
|---|---|
| Object transforms | Time samples of the transforms over the shutter |
| Deforming meshes, curves and points | Time samples of the points, or `velocities` (and `accelerations`); animated curve widths |
| Gaussian splats | Positions, and animated scales, orientations and opacities (*untested*) |
| Point instancers, also nested | Time samples, or `velocities` and `angularVelocities` of the instances |
| Volumes | A velocity field (`vel`, `v`, `velocity`, or native `vel.x/y/z`), see [volumes.md](volumes.md#motion-blur) |
| Camera | Time samples of the camera transform; an animated focal length gives zoom blur (*untested*) |

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

## Transform and deformation samples

Like Karma's Render Geometry Settings, an object can have its own number of samples for its
transform and for its points, instead of the Motion Steps (Render Geometry Settings LOP, Cycles
tab, Motion Blur panel, see [objects.md](objects.md#motion-blur)). All of this is *untested*.

| Setting | Use |
|---|---|
| **Transform Samples** | Samples of the transform over the shutter. 1 is no transform blur; 3 or more for curved motion |
| **Deformation Samples** | Samples of the points (and curve widths, splat attributes and volume velocity blur) over the shutter. 1 is no deformation blur |
| **Samples from Stage** | Use as many samples as the stage has time samples inside the shutter (plus the shutter ends) |

0 (the default) uses the Motion Steps. Even numbers are rounded up to the next odd number, so one
sample stays at the frame, as Cycles needs. Without the Cycles settings, Karma's Transform Time
Samples, Geometry Time Samples and Motion Samples from Stage are used. Samples from Stage is off
when nothing is authored, unlike in Karma, so scenes keep their motion steps.

Point instancers take Karma's **Instance Time Samples** (`karma:object:instance_samples`) for the
instance transforms and for the prototype transform, which must have the same motion steps.

### Velocity blur modes

Karma's **Velocity Blur** menu (`karma:object:vblur` on geometry, `karma:object:instance_vblur` on
point instancers, Karma tab of the Render Geometry Settings LOP) chooses how points move over the
shutter, also with Cycles (*untested*):

| Karma Velocity Blur | Motion |
|---|---|
| No Velocity Blur | Velocities are ignored: the time samples of the points are interpolated |
| Velocity Blur | Velocities, without accelerations; the motion steps are used instead of the Deformation Samples |
| Acceleration Blur | Velocities and accelerations, with at least 2 deformation samples |

When it is not authored, velocities and accelerations are used as before. Karma's value on the
render settings prim (Karma Render Settings LOP) is not read, as its default No Velocity Blur
would turn velocity blur off in every scene with that LOP.

## Per-object motion blur

The Render Geometry Settings LOP's **Cycles** tab has Blender's object **Motion Blur** panel (see
[objects.md](objects.md#motion-blur)):

- **Motion Blur** off renders the object sharp, at the shutter center, while the rest of the scene
  blurs. Off, it also has no motion vectors in the Vector pass.
- **Steps** gives the object its own motion steps (0 uses the render setting), for example more
  steps for a spinning wheel only.

## Shutter curve

The Camera LOP's **Cycles** tab has Blender's **Shutter Curve** (Motion Blur panel, see
[cameras.md](cameras.md#motion-blur)): a ramp that weights the motion blur samples from Shutter
Open (left) to Shutter Close (right). Constant by default; a curve that rises and falls gives
softer blur trails, like a mechanical shutter. *Untested.*

## Rolling shutter

The Camera LOP's **Cycles** tab (Motion Blur > Rolling Shutter, see [cameras.md](cameras.md)) adds
Blender's rolling shutter: with **Top-Bottom**, the scanlines are exposed one after the other over
the shutter, like a CMOS sensor, so fast moving objects lean. **Rolling Shutter Duration** is the
time each scanline is exposed, as a fraction of the shutter (0.1 by default).

## Limits

- Point **widths** are not blurred, only their positions. Curve widths blur when they are animated.
- Instancers whose number of instances changes within the shutter get no motion blur.
- A volume field named `vel` or `v` is always used as velocity.
- Zoom blur: only perspective cameras, and an animated aperture is not sampled.
- Karma's Linear Blur motion style is not supported (Cycles always blurs transforms with rotation).

## The Vector pass

The **Vector** pass (Passes > Data > Vector on the Cycles Render Settings LOP, render var `Vector`)
gives Blender-style motion vectors for motion blur in compositing:

- It is only written when **Motion Blur is off**. With motion blur on, the pass is empty, like in
  Blender (a warning is printed).
- It holds the screen-space motion in pixels to the previous frame (first two channels) and to the
  next frame (last two channels), so it needs a 4-channel format; the LOP authors it as `float4`.
- It needs the scene at the previous and next frame. Animated geometry needs time samples there:
  cache a frame range around the rendered frame (for example with a Cache LOP).
- An animated focal length writes zoom vectors (*untested*).

Adding or removing the Vector pass while rendering updates the render without a restart.
