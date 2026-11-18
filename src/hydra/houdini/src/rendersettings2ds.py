#!/usr/bin/env hython
"""
Generate the Houdini render settings UI for Cycles from render_settings.json.

Usage:
    hython rendersettings2ds.py --json <render_settings.json> --ds-install-dir <output_dir>

Houdini looks for dialog scripts named after the render delegate plugin in
$HOUDINI_PATH/soho/parameters:

  HdCyclesPlugin_Global.ds    Cycles tab of the Render Settings LOP.
  HdCyclesPlugin_Viewport.ds  Cycles section of the viewport display options.
  HdCyclesPlugin_Geometry.ds  Cycles tab of the Render Geometry Settings LOP (per object).
  HdCyclesPlugin_Light.ds     Cycles tab of the Light LOP (per light).

Each parameter is a "cycles:..." render setting forwarded to the delegate.
Types, defaults and menus come from the Cycles node sockets dumped to
render_settings.json, so the UI always shows what the delegate uses when a
setting is not authored. Labels and layout follow Blender's Cycles render
properties, with Blender's panels as collapsible folders. The same layout is
used by the Cycles Render Settings LOP (cyclesrendersettings2hda.py).
"""

import argparse
import json
import os
import sys

import hou


def log(message):
    print("[rendersettings2ds] " + message)


# Settings that are not node sockets, but session parameters of the delegate.
SESSION_SETTINGS = {
    "cycles:device": {
        "type": "menu",
        "default": "AUTO",
        "enum_values": {
            "AUTO": "Automatic (Best GPU)",
            "OPTIX": "GPU - OptiX",
            "CUDA": "GPU - CUDA",
            "HIP": "GPU - HIP",
            "ONEAPI": "GPU - oneAPI",
            "METAL": "GPU - Metal",
            "CPU": "CPU",
        },
    },
    "cycles:threads": {"type": "int", "default": 0},
    "cycles:shading_system": {"type": "boolean", "default": False},
    "cycles:use_resolution_divider": {"type": "boolean", "default": True},
    "cycles:curve_shape": {
        "type": "menu",
        "default": "ribbon",
        "enum_values": {"ribbon": "Rounded Ribbons", "thick": "3D Curves"},
    },
    "cycles:samples": {"type": "int", "default": 1024},
    "cycles:time_limit": {"type": "float", "default": 0.0},
    "cycles:sample_offset": {"type": "int", "default": 0},
    # Blender's Fast GI toggle, method, bounces and world AO factor, which the delegate maps to
    # the integrator's ao_factor, ao_additive_factor and ao_bounces.
    "cycles:fast_gi": {"type": "boolean", "default": False},
    "cycles:fast_gi_method": {
        "type": "menu",
        "default": "replace",
        "enum_values": {"replace": "Replace", "add": "Add"},
    },
    "cycles:ao_bounces": {"type": "int", "default": 1},
    "cycles:ao_factor": {"type": "float", "default": 1.0},
    "cycles:motion_steps": {"type": "int", "default": 1},
    "cycles:use_texture_cache": {"type": "boolean", "default": True},
    "cycles:auto_texture_cache": {"type": "boolean", "default": False},
    "cycles:texture_cache_path": {"type": "string", "default": ""},
    "cycles:tile_size": {"type": "int", "default": 2048},
    "cycles:pixel_size": {
        "type": "menu",
        "default": "1",
        "enum_values": {"1": "1x", "2": "2x", "4": "4x", "8": "8x"},
    },
    "cycles:use_spatial_splits": {"type": "boolean", "default": False},
    "cycles:use_compact_bvh": {"type": "boolean", "default": True},
    "cycles:curve_subdivisions": {"type": "int", "default": 3},
    # Blender's Subdivision panel, for objects with Adaptive Subdivision
    "cycles:dicing_rate": {"type": "float", "default": 1.0},
    "cycles:preview_dicing_rate": {"type": "float", "default": 8.0},
    "cycles:offscreen_dicing_scale": {"type": "float", "default": 4.0},
    "cycles:max_subdivisions": {"type": "int", "default": 12},
    "cycles:dicing_camera": {"type": "string", "default": ""},
    "cycles:bake_object": {"type": "string", "default": ""},
    "cycles:bake_margin": {"type": "int", "default": 16},
    "cycles:texture_limit": {
        "type": "menu",
        "default": "off",
        "enum_values": {"off": "No Limit", "128": "128", "256": "256", "512": "512",
                        "1024": "1024", "2048": "2048", "4096": "4096", "8192": "8192"},
    },
}

# Blender's defaults for final renders (Blender 5.2) where they differ from the delegate's, set by
# the Blender Defaults button of the Cycles Render Settings LOP. The parameter defaults stay what
# the delegate uses when a setting is not authored, which saved scenes rely on. GPU denoising
# stays on, while Blender denoises final renders on the CPU by default.
BLENDER_DEFAULTS = {
    "cycles:samples": 4096,
    "cycles:integrator:use_denoise": True,
    # What Blender's Automatic pattern uses for final renders
    "cycles:integrator:sampling_pattern": "blue_noise_pure",
    "cycles:integrator:light_sampling_threshold": 0.01,
    "cycles:integrator:max_bounce": 12,
    "cycles:integrator:max_diffuse_bounce": 4,
    "cycles:integrator:max_glossy_bounce": 4,
    "cycles:integrator:max_transmission_bounce": 12,
    "cycles:integrator:max_volume_bounce": 0,
    "cycles:integrator:transparent_max_bounce": 8,
    "cycles:film:filter_type": "blackman_harris",
    "cycles:film:filter_width": 1.5,
    "cycles:background:transparent_roughness_threshold": 0.1,
    "cycles:curve_shape": "thick",
    "cycles:curve_subdivisions": 2,
    "cycles:use_compact_bvh": False,
}

# Menu labels and items for socket enums, where the title cased token is not good enough.
# Items missing here are left out of the menu.
MENU_ITEMS = {
    "cycles:integrator:denoiser_type": {
        "openimagedenoise": "OpenImageDenoise",
        "optix": "OptiX",
        "dlss": "DLSS",
    },
    "cycles:integrator:denoiser_prefilter": {
        "none": "None",
        "fast": "Fast",
        "accurate": "Accurate",
    },
    "cycles:integrator:denoiser_quality": {
        "high": "High",
        "balanced": "Balanced",
        "fast": "Fast",
    },
    "cycles:integrator:sampling_pattern": {
        "tabulated_sobol": "Tabulated Sobol",
        "sobol_burley": "Sobol-Burley",
        "blue_noise_pure": "Blue-Noise (pure)",
        "blue_noise_round": "Blue-Noise (round)",
        "blue_noise_first": "Blue-Noise (first)",
    },
    "cycles:integrator:guiding_distribution_type": {
        "PARALLAX_AWARE_VMM": "Parallax-Aware VMM",
        "DIRECTIONAL_QUAD_TREE": "Directional Quad Tree",
        "VMM": "VMM",
    },
    "cycles:integrator:guiding_directional_sampling_type": {
        "MIS": "Diffuse Product MIS",
        "RIS": "Re-sampled Importance Sampling",
        "ROUGHNESS": "Roughness-based",
    },
    "cycles:film:filter_type": {
        "box": "Box",
        "gaussian": "Gaussian",
        "blackman_harris": "Blackman-Harris",
    },
}


def S(name, label, help, range=None, viewport=False, toolbar_icon=None, hidewhen=None,
      disablewhen=None, tags=()):
    """A single setting in the layout. tags are extra (name, value) parm tags."""
    return {
        "tags": tags,
        "name": name,
        "label": label,
        "help": help,
        "range": range,
        "viewport": viewport,
        "toolbar_icon": toolbar_icon,
        "hidewhen": hidewhen,
        "disablewhen": disablewhen,
    }


def P(label, items, open=False, toggle=None, legacy=()):
    """A panel of settings and sub-panels, like a Blender properties panel.

    toggle is the setting shown in the panel header in Blender. It is the first parameter of the
    folder, and the other settings in the panel are disabled when it is off. legacy settings are
    invisible parameters at the end of the panel, which are not in settings().
    """
    return {"panel": label, "items": items, "open": open, "toggle": toggle, "legacy": legacy}


# The integrator sockets the Fast GI panel showed before the Fast GI settings. Invisible
# parameters keep their values in saved scenes: the Render Settings LOP tab still authors them
# when set (the delegate ignores them when Fast GI settings are authored), the Cycles Render
# Settings LOP converts them to the Fast GI settings when a scene is loaded.
LEGACY_SETTINGS = [
    S("cycles:integrator:ao_bounces", "AO Bounces", "Replaced by Fast GI Approximation."),
    S("cycles:integrator:ao_factor", "AO Factor", "Replaced by Fast GI Approximation."),
    S("cycles:integrator:ao_additive_factor", "AO Additive Factor",
      "Replaced by Fast GI Approximation."),
]


# Parameter tags of prim path settings: select the prim in the viewer.
PRIM_PATH_TAGS = (
    ("script_action", "import loputils; loputils.selectPrimsInParm(kwargs, False)"),
    ("script_action_help", "Select a primitive in the Scene Viewer or Scene Graph Tree pane."),
    ("script_action_icon", "BUTTONS_reselect"),
    ("sidefx::usdpathtype", "prim"),
)


# Layout of the settings, following Blender's Cycles render properties.
LAYOUT = [
    P("Device", [
        S("cycles:device", "Device",
          "Device to render with. Automatic uses the best available GPU (OptiX, then CUDA, "
          "HIP, oneAPI, Metal) and falls back to the CPU. Changing it restarts the render.",
          viewport=True),
        S("cycles:shading_system", "Open Shading Language",
          "Use Open Shading Language. On the GPU only OptiX supports it. Changing it restarts "
          "the render.",
          viewport=True),
    ], open=True),
    P("Sampling", [
        P("Render", [
            S("cycles:integrator:use_adaptive_sampling", "Noise Threshold",
              "Automatically reduce the number of samples per pixel based on estimated noise "
              "level.",
              viewport=True),
            S("cycles:integrator:adaptive_threshold", "Threshold",
              "Noise level step to stop sampling at, lower values reduce noise at the cost of "
              "render time. 0 for automatic setting based on the number of samples.",
              range=(0, 1, True, True), viewport=True,
              disablewhen="{ cycles:integrator:use_adaptive_sampling == 0 }"),
            S("cycles:samples", "Max Samples",
              "Number of samples to render for each pixel.",
              range=(1, 4096, True, False), viewport=True),
            S("cycles:integrator:adaptive_min_samples", "Min Samples",
              "Minimum number of samples before adaptive sampling stops a pixel. 0 for automatic "
              "setting based on the noise threshold.",
              range=(0, 4096, True, False), viewport=True,
              disablewhen="{ cycles:integrator:use_adaptive_sampling == 0 }"),
            S("cycles:time_limit", "Time Limit",
              "Limit the render time in seconds, excluding synchronization time. 0 disables the "
              "limit.",
              range=(0, 600, True, False)),
            P("Denoise", [
                S("cycles:integrator:use_denoise", "Denoise",
                  "Denoise the rendered image.",
                  viewport=True, toolbar_icon="VIEW_display_denoise"),
                S("cycles:integrator:denoiser_type", "Denoiser",
                  "Denoiser to use. OptiX requires an NVIDIA GPU. DLSS Ray Reconstruction "
                  "denoises the viewport, it requires an NVIDIA RTX GPU and driver 620 or newer; "
                  "final renders and other GPUs use OpenImageDenoise or the fastest available "
                  "denoiser instead.",
                  viewport=True),
                S("cycles:integrator:denoiser_prefilter", "Prefilter",
                  "Prefilter of the guiding passes (albedo and normal). Accurate gives the best "
                  "detail, None should only be used when the guiding passes are noise free.",
                  viewport=True,
                  hidewhen="{ cycles:integrator:denoiser_type != openimagedenoise }"),
                S("cycles:integrator:denoiser_quality", "Quality",
                  "Quality of the OpenImageDenoise denoiser, balanced and fast trade quality for "
                  "speed.",
                  viewport=True,
                  hidewhen="{ cycles:integrator:denoiser_type != openimagedenoise }"),
                S("cycles:integrator:denoise_start_sample", "Start Sample",
                  "Sample to start denoising the interactive render at.",
                  range=(0, 64, True, False), viewport=True,
                  hidewhen="{ cycles:integrator:denoiser_type == dlss }"),
                S("cycles:integrator:denoise_use_gpu", "Use GPU",
                  "Run the denoiser on the GPU when available.",
                  viewport=True,
                  hidewhen="{ cycles:integrator:denoiser_type != openimagedenoise }"),
            ], toggle="cycles:integrator:use_denoise"),
        ], open=True),
        P("Path Guiding", [
            S("cycles:integrator:use_guiding", "Path Guiding",
              "Use path guiding for sampling paths. Path guiding incrementally learns the light "
              "distribution of the scene and guides paths into directions with high direct and "
              "indirect light contributions. Only supported on the CPU.",
              disablewhen="{ cycles:device != CPU }"),
            S("cycles:integrator:guiding_training_samples", "Training Samples",
              "The maximum number of samples used for training path guiding. 0 trains for all "
              "samples.",
              range=(0, 1024, True, False)),
            S("cycles:integrator:use_surface_guiding", "Surface",
              "Use guiding when sampling directions on a surface."),
            S("cycles:integrator:use_volume_guiding", "Volume",
              "Use guiding when sampling directions inside a volume."),
            P("Debug", [
                S("cycles:integrator:guiding_distribution_type", "Distribution Type",
                  "Type of representation for the guiding distribution."),
                S("cycles:integrator:guiding_roughness_threshold", "Roughness Threshold",
                  "The minimal roughness value of a material to apply guiding.",
                  range=(0, 1, True, True)),
                S("cycles:integrator:guiding_directional_sampling_type",
                  "Directional Sampling Type",
                  "Type of the directional sampling used for guiding."),
                S("cycles:integrator:surface_guiding_probability", "Surface Guiding Probability",
                  "The probability of guiding a direction on a surface.",
                  range=(0, 1, True, True)),
                S("cycles:integrator:volume_guiding_probability", "Volume Guiding Probability",
                  "The probability of guiding a direction inside a volume.",
                  range=(0, 1, True, True)),
                S("cycles:integrator:deterministic_guiding", "Deterministic",
                  "Make path guiding deterministic, which means multiple renders with the same "
                  "settings and scene give the same result. Rendering may be slower."),
                S("cycles:integrator:use_guiding_direct_light", "Guide Direct Light",
                  "Consider the contribution of directly visible light sources during guiding."),
                S("cycles:integrator:use_guiding_mis_weights", "Use MIS Weights",
                  "Use the MIS weight to weight the contribution of directly visible light "
                  "sources during guiding."),
            ]),
        ], toggle="cycles:integrator:use_guiding"),
        P("Advanced", [
            S("cycles:integrator:sampling_pattern", "Pattern",
              "Random sampling pattern used by the integrator."),
            S("cycles:integrator:seed", "Seed",
              "Seed value for the random number generator, to get different noise patterns.",
              range=(0, 100, True, False)),
            S("cycles:integrator:scrambling_distance", "Scrambling Distance",
              "Reduce the randomization between pixels to improve GPU rendering performance, at "
              "the cost of possible rendering artifacts if set too low.",
              range=(0, 1, True, True)),
            S("cycles:integrator:min_bounce", "Min Light Bounces",
              "Minimum number of light bounces. Setting this higher reduces noise in the first "
              "bounces, but can also be less efficient for more complex geometry like hair.",
              range=(0, 32, True, False)),
            S("cycles:integrator:transparent_min_bounce", "Min Transparent Bounces",
              "Minimum number of transparent bounces. Setting this higher reduces noise in the "
              "first bounces, but can also be less efficient for more complex geometry like "
              "hair.",
              range=(0, 32, True, False)),
            P("Sample Subset", [
                S("cycles:integrator:use_sample_subset", "Sample Subset",
                  "Render a subset of the specified max samples. Typically used for distributed "
                  "rendering across multiple devices."),
                S("cycles:integrator:sample_subset_offset", "Offset",
                  "0-based index of sample to start rendering from.",
                  range=(0, 4096, True, False)),
                S("cycles:integrator:sample_subset_length", "Length",
                  "The number of samples to render in this subset.",
                  range=(1, 4096, True, False)),
            ], toggle="cycles:integrator:use_sample_subset"),
        ]),
        P("Lights", [
            S("cycles:integrator:use_light_tree", "Light Tree",
              "Sample multiple lights more efficiently based on estimated contribution at every "
              "shading point."),
            S("cycles:integrator:light_sampling_threshold", "Light Threshold",
              "Probabilistically terminate light samples when the light contribution is below "
              "this threshold (more noise but faster rendering). 0 disables the test. Not "
              "used with the light tree.",
              range=(0, 1, True, False),
              disablewhen="{ cycles:integrator:use_light_tree == 1 }"),
        ]),
    ], open=True),
    P("Light Paths", [
        P("Max Bounces", [
            S("cycles:integrator:max_bounce", "Total",
              "Total maximum number of bounces.",
              range=(0, 32, True, False), viewport=True),
            S("cycles:integrator:max_diffuse_bounce", "Diffuse",
              "Maximum number of diffuse reflection bounces, bounded by total maximum.",
              range=(0, 32, True, False), viewport=True),
            S("cycles:integrator:max_glossy_bounce", "Glossy",
              "Maximum number of glossy reflection bounces, bounded by total maximum.",
              range=(0, 32, True, False), viewport=True),
            S("cycles:integrator:max_transmission_bounce", "Transmission",
              "Maximum number of transmission bounces, bounded by total maximum.",
              range=(0, 32, True, False), viewport=True),
            S("cycles:integrator:max_volume_bounce", "Volume",
              "Maximum number of volumetric scattering events.",
              range=(0, 32, True, False), viewport=True),
            S("cycles:integrator:transparent_max_bounce", "Transparent",
              "Maximum number of transparent bounces. This is independent of maximum number of "
              "other bounces.",
              range=(0, 32, True, False), viewport=True),
        ], open=True),
        P("Clamping", [
            S("cycles:integrator:sample_clamp_direct", "Direct Light",
              "If non-zero, the maximum value for a direct sample, higher values will be scaled "
              "down to avoid too much noise and slow convergence at the cost of accuracy.",
              range=(0, 100, True, False)),
            S("cycles:integrator:sample_clamp_indirect", "Indirect Light",
              "If non-zero, the maximum value for an indirect sample, higher values will be "
              "scaled down to avoid too much noise and slow convergence at the cost of accuracy.",
              range=(0, 100, True, False)),
        ], open=True),
        P("Caustics", [
            S("cycles:integrator:filter_glossy", "Filter Glossy",
              "Adaptively blur glossy shaders after blurry bounces, to reduce noise at the cost "
              "of accuracy.",
              range=(0, 10, True, False)),
            S("cycles:integrator:caustics_reflective", "Reflective",
              "Use reflective caustics, resulting in a brighter image (more noise but added "
              "realism)."),
            S("cycles:integrator:caustics_refractive", "Refractive",
              "Use refractive caustics, resulting in a brighter image (more noise but added "
              "realism)."),
        ], open=True),
        P("Fast GI Approximation", [
            S("cycles:fast_gi", "Fast GI Approximation",
              "Approximate diffuse indirect light with background tinted ambient occlusion. This "
              "provides a fast alternative to full global illumination, for interactive viewport "
              "rendering or final renders with reduced quality."),
            S("cycles:fast_gi_method", "Method",
              "Fast GI approximation method. Replace: replace global illumination with ambient "
              "occlusion after a specified number of bounces. Add: add ambient occlusion to "
              "diffuse surfaces."),
            S("cycles:ao_bounces", "Bounces",
              "After this number of light bounces, use approximate global illumination. 0 "
              "disables this feature.",
              range=(0, 16, True, False), hidewhen="{ cycles:fast_gi_method == add }"),
            S("cycles:ao_factor", "AO Factor",
              "Factor for ambient occlusion blending.",
              range=(0, 1, True, False)),
            S("cycles:integrator:ao_distance", "AO Distance",
              "Distance from shading point to trace rays for the approximation.",
              range=(0, 100, True, False)),
        ], toggle="cycles:fast_gi", legacy=LEGACY_SETTINGS),
    ]),
    P("Volumes", [
        S("cycles:integrator:volume_ray_marching", "Biased",
          "Use the biased ray marching volume rendering instead of the unbiased null "
          "scattering method."),
        S("cycles:integrator:volume_step_rate", "Step Rate",
          "Globally adjust detail for volume rendering, on top of automatically estimated step "
          "size. Higher values reduce render time, lower values render with more detail.",
          range=(0.01, 100, True, False),
          hidewhen="{ cycles:integrator:volume_ray_marching == 0 }"),
        S("cycles:integrator:volume_max_steps", "Max Steps",
          "Maximum number of steps through the volume before giving up, to avoid extremely "
          "long render times with big objects or small step sizes.",
          range=(2, 65536, True, False),
          hidewhen="{ cycles:integrator:volume_ray_marching == 0 }"),
    ]),
    P("Curves", [
        S("cycles:curve_shape", "Shape",
          "Form of curves. Rounded Ribbons render curves as flat ribbons with rounded normals, "
          "for fast rendering. 3D Curves render curves as circular 3D geometry, for accurate "
          "results when viewing closely. Changing it restarts the render.",
          viewport=True),
        S("cycles:curve_subdivisions", "Curve Subdivisions",
          "Number of subdivisions used in curve intersection, as a power of 2 (Blender uses 2). "
          "Changing it restarts the render. Only for Rounded Ribbons.",
          range=(0, 4, True, False), hidewhen="{ cycles:curve_shape != ribbon }"),
    ]),
    P("Subdivision", [
        S("cycles:dicing_rate", "Dicing Rate Render",
          "Size of a micropolygon in pixels for final renders, for objects with Adaptive "
          "Subdivision (Render Geometry Settings LOP).",
          range=(0.1, 1000, True, False)),
        S("cycles:preview_dicing_rate", "Viewport",
          "Size of a micropolygon in pixels in the viewport, for objects with Adaptive "
          "Subdivision.",
          range=(0.1, 1000, True, False), viewport=True),
        S("cycles:offscreen_dicing_scale", "Offscreen Scale",
          "Multiplier for the dicing rate of geometry outside of the camera view. The dicing "
          "rate of objects is gradually increased the further they are outside the camera "
          "view. Lower values provide higher quality reflections and shadows for off screen "
          "objects, while higher values use less memory.",
          range=(1, 25, True, False)),
        S("cycles:max_subdivisions", "Max Subdivisions",
          "Stop subdividing when this level is reached even if the dice rate would produce "
          "finer micropolygons.",
          range=(0, 16, True, True)),
        S("cycles:dicing_camera", "Dicing Camera",
          "Camera to use as reference point when subdividing geometry, useful to avoid crawling "
          "artifacts in animations when the scene camera is moving. Empty uses the render "
          "camera.",
          tags=PRIM_PATH_TAGS),
    ]),
    P("Simplify", [
        S("cycles:texture_limit", "Texture Size Limit",
          "Limit the size of image textures, for less memory use. Changing it restarts the "
          "render.",
          viewport=True),
    ]),
    P("Motion Blur", [
        S("cycles:integrator:motion_blur", "Motion Blur",
          "Render motion blur over the shutter of the camera (Shutter Open and Close on the "
          "Camera LOP, in frames), from animated transforms, points and instances, velocities "
          "and accelerations of points and instances, and velocity fields of volumes (vel, v "
          "or velocity). The Vector pass is empty with motion blur, like in Blender.",
          viewport=True),
        S("cycles:motion_steps", "Motion Steps",
          "Accuracy of motion blur of transforms, points and instances: more steps use more "
          "memory (2^steps + 1 motion samples over the shutter, like Blender's object "
          "motion steps).",
          range=(1, 7, True, True)),
    ], toggle="cycles:integrator:motion_blur"),
    P("Film", [
        S("cycles:film:exposure", "Exposure",
          "Image brightness scale.",
          range=(0, 10, True, False)),
        P("Pixel Filter", [
            S("cycles:film:filter_type", "Type",
              "Pixel filter type."),
            S("cycles:film:filter_width", "Width",
              "Pixel filter width.",
              range=(0.01, 10, True, False),
              hidewhen="{ cycles:film:filter_type == box }"),
        ], open=True),
        P("Transparent", [
            S("cycles:background:transparent", "Transparent",
              "World background is transparent, for compositing the render over another "
              "background. When not set, the background is transparent unless the scene has a "
              "dome light."),
            S("cycles:background:transparent_glass", "Transparent Glass",
              "Render transmissive surfaces as transparent, for compositing glass over another "
              "background."),
            S("cycles:background:transparent_roughness_threshold", "Roughness Threshold",
              "For transparent transmission, keep surfaces with roughness above the threshold "
              "opaque.",
              range=(0, 1, True, True),
              disablewhen="{ cycles:background:transparent_glass == 0 }"),
        ], open=True, toggle="cycles:background:transparent"),
    ]),
    P("Passes", [
        S("cycles:film:pass_alpha_threshold", "Alpha Threshold",
          "Z, Index, normal, UV and vector passes are only affected by surfaces with alpha "
          "transparency equal to or higher than this threshold.",
          range=(0, 1, True, True)),
        P("Mist Pass", [
            S("cycles:film:mist_start", "Start",
              "Starting distance of the mist, measured from the camera.",
              range=(0, 100, True, False)),
            S("cycles:film:mist_depth", "Depth",
              "Distance over which the mist effect fades in.",
              range=(0, 100, True, False)),
            S("cycles:film:mist_falloff", "Falloff",
              "Type of transition used to fade mist (Blender: 1 linear, 2 quadratic, 0.5 inverse "
              "quadratic).",
              range=(0, 4, True, False)),
        ], open=True),
    ]),
    P("Performance", [
        P("Threads", [
            S("cycles:threads", "Threads",
              "Number of CPU threads to use. 0 uses all cores.",
              range=(0, 64, True, False)),
        ], open=True),
        P("Viewport", [
            S("cycles:use_resolution_divider", "Start at Low Resolution",
              "Render the first samples after a change at a lower resolution, for faster "
              "feedback while navigating. Changing it restarts the render.",
              viewport=True),
            S("cycles:pixel_size", "Pixel Size",
              "Pixel size for viewport rendering, larger pixels render faster. Final renders "
              "always use full resolution. Changing it restarts the render.",
              viewport=True),
        ], open=True),
        P("Memory", [
            S("cycles:tile_size", "Tile Size",
              "Render high resolution images in tiles of this size, to reduce memory usage. "
              "Tiles are cached to disk while rendering. Final renders only. Changing it "
              "restarts the render.",
              range=(8, 8192, True, True)),
        ], open=True),
        P("Acceleration Structure", [
            S("cycles:use_spatial_splits", "Use Spatial Splits",
              "Use BVH spatial splits: longer build time, faster render. CPU only (Embree). "
              "Changing it restarts the render."),
            S("cycles:use_compact_bvh", "Use Compact BVH",
              "Use a compact BVH structure: uses less memory but renders slower. CPU only "
              "(Embree; Blender turns it off). Changing it restarts the render."),
        ], open=True),
        P("Texture Cache", [
            S("cycles:use_texture_cache", "Texture Cache",
              "Use tiled and mipmapped .tx files, loading only the tiles and mip levels the "
              "render needs: .tx, tiled and mipmapped textures are used directly, other images "
              "use the cache files of the Cache Directory or of a blender_tx folder next to "
              "them when those exist. Changing it restarts the render."),
            S("cycles:auto_texture_cache", "Auto Generate",
              "Generate missing cache files when rendering. Changing it restarts the render."),
            S("cycles:texture_cache_path", "Cache Directory",
              "Directory of the cache files, absolute or relative to each image. Empty uses a "
              "blender_tx folder next to each image. Changing it restarts the render."),
        ], open=True, toggle="cycles:use_texture_cache"),
    ]),
    P("Bake", [
        S("cycles:bake_object", "Bake Object",
          "Mesh to bake, like Blender's Bake: the render is the mesh's default UV map instead "
          "of the camera view, each pixel shading the point of the surface that covers it in "
          "UV space, with the render vars (beauty, diffuse color, normal, ...) as the baked "
          "maps. The resolution is the render resolution. Empty renders the camera view.",
          tags=PRIM_PATH_TAGS),
        S("cycles:bake_margin", "Margin",
          "Extends the baked result as a post process filter, in pixels (Blender's Extend "
          "margin), so seams of the UV islands do not show.",
          range=(0, 64, True, False)),
    ]),
]

# USD value type, Houdini parameter type and Edit Properties control type per socket type.
TYPES = {
    "boolean": ("bool", "toggle", "bool"),
    "int": ("int", "int", "int"),
    "float": ("float", "float", "float"),
    "menu": ("token", "string", "string"),
    "string": ("string", "string", "string"),
    "file": ("string", "file", "string"),
}


# Per-object settings, authored as constant primvars by the Render Geometry Settings LOP. The
# delegate reads "cycles:object:visible_<ray>" as ray visibility and the others as the Cycles
# object socket of the same name.
GEOMETRY_SETTINGS = {
    "primvars:cycles:object:visible_camera": {"type": "boolean", "default": True},
    "primvars:cycles:object:visible_diffuse": {"type": "boolean", "default": True},
    "primvars:cycles:object:visible_glossy": {"type": "boolean", "default": True},
    "primvars:cycles:object:visible_transmission": {"type": "boolean", "default": True},
    "primvars:cycles:object:visible_volume_scatter": {"type": "boolean", "default": True},
    "primvars:cycles:object:visible_shadow": {"type": "boolean", "default": True},
    "primvars:cycles:object:visible_raycast": {"type": "boolean", "default": True},
    "primvars:cycles:object:is_shadow_catcher": {"type": "boolean", "default": False},
    "primvars:cycles:object:use_holdout": {"type": "boolean", "default": False},
    "primvars:cycles:object:is_caustics_caster": {"type": "boolean", "default": False},
    "primvars:cycles:object:is_caustics_receiver": {"type": "boolean", "default": False},
    "primvars:cycles:object:shadow_terminator_geometry_offset": {"type": "float",
                                                                 "default": 0.1},
    "primvars:cycles:object:shadow_terminator_shading_offset": {"type": "float",
                                                                "default": 0.0},
    "primvars:cycles:object:ao_distance": {"type": "float", "default": 0.0},
    "primvars:cycles:object:lightgroup": {"type": "string", "default": ""},
    "primvars:cycles:object:pass_id": {"type": "int", "default": 0},
    "primvars:cycles:object:use_motion_blur": {"type": "boolean", "default": True},
    "primvars:cycles:object:motion_steps": {"type": "int", "default": 0},
    "primvars:cycles:object:use_adaptive_subdivision": {"type": "boolean", "default": False},
    "primvars:cycles:object:dicing_rate": {"type": "float", "default": 1.0},
    "primvars:cycles:object:volume_step_size": {"type": "float", "default": 0.0},
}

# Layout of the per-object settings, following Blender's Cycles object panels.
GEOMETRY_LAYOUT = [
    P("Relations", [
        S("primvars:cycles:object:pass_id", "Pass Index",
          "Index number for the Object Index pass and the Object Index output of the Object "
          "Info node.",
          range=(0, 1000, True, False)),
    ]),
    P("Motion Blur", [
        S("primvars:cycles:object:use_motion_blur", "Motion Blur",
          "Use motion blur for this object, when the Motion Blur render setting is on."),
        S("primvars:cycles:object:motion_steps", "Steps",
          "Motion steps of this object: 2^steps + 1 motion samples over the shutter, for "
          "curved motion. 0 uses the Motion Steps render setting.",
          range=(0, 7, True, True)),
    ], toggle="primvars:cycles:object:use_motion_blur"),
    P("Visibility", [
        P("Ray Visibility", [
            S("primvars:cycles:object:visible_camera", "Camera",
              "Object visibility for camera rays."),
            S("primvars:cycles:object:visible_diffuse", "Diffuse",
              "Object visibility for diffuse reflection rays."),
            S("primvars:cycles:object:visible_glossy", "Glossy",
              "Object visibility for glossy reflection rays."),
            S("primvars:cycles:object:visible_transmission", "Transmission",
              "Object visibility for transmission rays."),
            S("primvars:cycles:object:visible_volume_scatter", "Volume Scatter",
              "Object visibility for volume scatter rays."),
            S("primvars:cycles:object:visible_shadow", "Shadow",
              "Object visibility for shadow rays."),
            S("primvars:cycles:object:visible_raycast", "Raycast",
              "Object visibility for the rays of Raycast nodes."),
        ], open=True),
        P("Mask", [
            S("primvars:cycles:object:is_shadow_catcher", "Shadow Catcher",
              "Only render shadows and reflections on this object, for compositing renders "
              "into real footage."),
            S("primvars:cycles:object:use_holdout", "Holdout",
              "Render a hole in the image with transparent alpha instead of the object."),
        ], open=True),
        P("Caustics", [
            S("primvars:cycles:object:is_caustics_caster", "Cast Shadow Caustics",
              "With Manifold Next Event Estimation, cast shadow caustics from refractive "
              "surfaces onto caustics receivers."),
            S("primvars:cycles:object:is_caustics_receiver", "Receive Shadow Caustics",
              "With Manifold Next Event Estimation, receive shadow caustics from caustics "
              "casters."),
        ]),
        P("Fast GI Approximation", [
            S("primvars:cycles:object:ao_distance", "AO Distance",
              "AO distance used for the Fast GI approximation (0 means use the world setting).",
              range=(0, 10, True, False)),
        ]),
    ], open=True),
    P("Shading", [
        P("Shadow Terminator", [
            S("primvars:cycles:object:shadow_terminator_geometry_offset", "Geometry Offset",
              "Offset rays from the surface to reduce shadow terminator artifact on low poly "
              "geometry. Only affects triangles at grazing angles to light.",
              range=(0, 1, True, True)),
            S("primvars:cycles:object:shadow_terminator_shading_offset", "Shading Offset",
              "Push the shadow terminator towards the light to hide artifacts on low poly "
              "geometry.",
              range=(0, 1, True, True)),
        ], open=True),
        P("Light Group", [
            S("primvars:cycles:object:lightgroup", "Light Group",
              "Light group the object's emission belongs to (render it with a light group pass "
              "on the Cycles Render Settings LOP)."),
        ], open=True),
    ]),
    P("Subdivision", [
        S("primvars:cycles:object:use_adaptive_subdivision", "Adaptive Subdivision",
          "Subdivide subdivision surfaces (Catmull-Clark or Bilinear subdivision scheme) to the "
          "dicing rate of the render settings in pixels of the render camera, up to Max "
          "Subdivisions, also without a refinement level. Off, they are subdivided up to their "
          "refinement level."),
        S("primvars:cycles:object:dicing_rate", "Dicing Scale",
          "Multiplier for the dicing rate of the render settings.",
          range=(0.1, 1000, True, False)),
    ], toggle="primvars:cycles:object:use_adaptive_subdivision"),
    P("Volume", [
        S("primvars:cycles:object:volume_step_size", "Step Size",
          "Distance between volume samples in object space, with Biased volume rendering. 0 "
          "computes it from the voxel size.",
          range=(0, 100, True, False)),
    ]),
]

# Per-light settings, authored as "inputs:cycles:light:<setting>" attributes by the Light LOP.
# The delegate reads "visible_<ray>" as ray visibility, "lightgroup" as the object's light group
# and the others as the Cycles light socket of the same name. Defaults are Blender's.
LIGHT_SETTINGS = {
    "inputs:cycles:light:max_bounces": {"type": "int", "default": 1024},
    "inputs:cycles:light:use_mis": {"type": "boolean", "default": True},
    "inputs:cycles:light:use_caustics": {"type": "boolean", "default": False},
    "inputs:cycles:light:use_soft_falloff": {"type": "boolean", "default": False},
    "inputs:cycles:light:spread": {"type": "float", "default": 180.0},
    "inputs:cycles:light:is_portal": {"type": "boolean", "default": False},
    "inputs:cycles:light:visible_camera": {"type": "boolean", "default": False},
    "inputs:cycles:light:visible_diffuse": {"type": "boolean", "default": True},
    "inputs:cycles:light:visible_glossy": {"type": "boolean", "default": True},
    "inputs:cycles:light:visible_transmission": {"type": "boolean", "default": True},
    "inputs:cycles:light:visible_volume_scatter": {"type": "boolean", "default": True},
    "inputs:cycles:light:lightgroup": {"type": "string", "default": ""},
    "inputs:cycles:light:world_material": {"type": "string", "default": ""},
    "inputs:cycles:light:map_resolution": {"type": "int", "default": 0},
}

# Per-camera settings, authored as "primvars:cycles:camera:<setting>" attributes by the Camera LOP
# (primvars, so that UsdImaging dirties the camera when they change). Angles are in degrees, like
# Blender's camera properties. Defaults are Blender's.
PANORAMA_TYPES = {
    "none": "None",
    "equirectangular": "Equirectangular",
    "equiangular_cubemap_face": "Equiangular Cubemap Face",
    "mirrorball": "Mirror Ball",
    "fisheye_equidistant": "Fisheye Equidistant",
    "fisheye_equisolid": "Fisheye Equisolid",
    "fisheye_lens_polynomial": "Fisheye Lens Polynomial",
    "panorama_central_cylindrical": "Central Cylindrical",
}
CAMERA_SETTINGS = {
    "primvars:cycles:camera:panorama_type": {"type": "menu", "default": "none",
                                    "enum_values": PANORAMA_TYPES},
    "primvars:cycles:camera:fisheye_fov": {"type": "float", "default": 180.0},
    "primvars:cycles:camera:fisheye_lens": {"type": "float", "default": 10.5},
    "primvars:cycles:camera:latitude_min": {"type": "float", "default": -90.0},
    "primvars:cycles:camera:latitude_max": {"type": "float", "default": 90.0},
    "primvars:cycles:camera:longitude_min": {"type": "float", "default": -180.0},
    "primvars:cycles:camera:longitude_max": {"type": "float", "default": 180.0},
    "primvars:cycles:camera:fisheye_polynomial_k0": {"type": "float", "default": -1.1735143712967577e-05},
    "primvars:cycles:camera:fisheye_polynomial_k1": {"type": "float", "default": -0.019988736953434998},
    "primvars:cycles:camera:fisheye_polynomial_k2": {"type": "float", "default": -3.3525322965709175e-06},
    "primvars:cycles:camera:fisheye_polynomial_k3": {"type": "float", "default": 3.099275275886036e-06},
    "primvars:cycles:camera:fisheye_polynomial_k4": {"type": "float", "default": -2.6064646454854524e-08},
    "primvars:cycles:camera:central_cylindrical_range_u_min": {"type": "float", "default": -180.0},
    "primvars:cycles:camera:central_cylindrical_range_u_max": {"type": "float", "default": 180.0},
    "primvars:cycles:camera:central_cylindrical_range_v_min": {"type": "float", "default": -1.0},
    "primvars:cycles:camera:central_cylindrical_range_v_max": {"type": "float", "default": 1.0},
    "primvars:cycles:camera:blades": {"type": "int", "default": 0},
    "primvars:cycles:camera:blades_rotation": {"type": "float", "default": 0.0},
    "primvars:cycles:camera:aperture_ratio": {"type": "float", "default": 1.0},
    "primvars:cycles:camera:rolling_shutter_type": {"type": "menu", "default": "none",
                                           "enum_values": {"none": "None",
                                                           "top": "Top-Bottom"}},
    "primvars:cycles:camera:rolling_shutter_duration": {"type": "float", "default": 0.1},
    "primvars:cycles:camera:custom_script": {"type": "file", "default": ""},
}


def not_panorama(*types):
    return "{ %s }" % " ".join("primvars:cycles:camera:panorama_type != %s" % t for t in types)


# Layout of the per-camera settings, following Blender's camera properties.
CAMERA_LAYOUT = [
    P("Lens", [
        S("primvars:cycles:camera:panorama_type", "Panorama Type",
          "Render a panoramic image with this projection instead of the camera's perspective "
          "or orthographic projection. Fisheye lens types use the camera's aperture as the "
          "sensor size."),
        S("primvars:cycles:camera:custom_script", "Custom Lens Script",
          "OSL camera shader (.osl or .oso file) that generates the camera rays, like Blender's "
          "Custom camera type. It replaces the projection and the panorama type. Its parameters "
          "come from primvars:cycles:camera:script:<parameter> primvars on the camera. CPU and "
          "OptiX only."),
        S("primvars:cycles:camera:fisheye_fov", "Field of View",
          "Field of view of the fisheye lens, in degrees.",
          range=(10, 360, True, True),
          hidewhen=not_panorama("fisheye_equidistant", "fisheye_equisolid",
                                "fisheye_lens_polynomial")),
        S("primvars:cycles:camera:fisheye_lens", "Fisheye Lens",
          "Lens focal length of the fisheye lens (in the unit of the aperture, millimeters).",
          range=(0.01, 100, True, False),
          hidewhen=not_panorama("fisheye_equisolid")),
        S("primvars:cycles:camera:latitude_min", "Latitude Min",
          "Minimum latitude (vertical angle) of the panorama, in degrees.",
          range=(-90, 90, True, True), hidewhen=not_panorama("equirectangular")),
        S("primvars:cycles:camera:latitude_max", "Latitude Max",
          "Maximum latitude (vertical angle) of the panorama, in degrees.",
          range=(-90, 90, True, True), hidewhen=not_panorama("equirectangular")),
        S("primvars:cycles:camera:longitude_min", "Longitude Min",
          "Minimum longitude (horizontal angle) of the panorama, in degrees.",
          range=(-180, 180, True, True), hidewhen=not_panorama("equirectangular")),
        S("primvars:cycles:camera:longitude_max", "Longitude Max",
          "Maximum longitude (horizontal angle) of the panorama, in degrees.",
          range=(-180, 180, True, True), hidewhen=not_panorama("equirectangular")),
    ] + [
        S("primvars:cycles:camera:fisheye_polynomial_k%d" % i, "K%d" % i,
          "Coefficient K%d of the lens polynomial." % i,
          hidewhen=not_panorama("fisheye_lens_polynomial"))
        for i in range(5)
    ] + [
        S("primvars:cycles:camera:central_cylindrical_range_u_min", "Min Longitude",
          "Minimum longitude of the central cylindrical panorama, in degrees.",
          range=(-180, 180, True, True), hidewhen=not_panorama("panorama_central_cylindrical")),
        S("primvars:cycles:camera:central_cylindrical_range_u_max", "Max Longitude",
          "Maximum longitude of the central cylindrical panorama, in degrees.",
          range=(-180, 180, True, True), hidewhen=not_panorama("panorama_central_cylindrical")),
        S("primvars:cycles:camera:central_cylindrical_range_v_min", "Min Height",
          "Minimum height of the central cylindrical panorama.",
          range=(-10, 10, False, False),
          hidewhen=not_panorama("panorama_central_cylindrical")),
        S("primvars:cycles:camera:central_cylindrical_range_v_max", "Max Height",
          "Maximum height of the central cylindrical panorama.",
          range=(-10, 10, False, False),
          hidewhen=not_panorama("panorama_central_cylindrical")),
    ], open=True),
    P("Depth of Field", [
        P("Aperture", [
            S("primvars:cycles:camera:blades", "Blades",
              "Number of blades in the aperture for polygonal bokeh (at least 3, 0 for a round "
              "bokeh). The aperture size comes from the camera's F-Stop.",
              range=(0, 16, True, False)),
            S("primvars:cycles:camera:blades_rotation", "Rotation",
              "Rotation of the blades in the aperture, in degrees.",
              range=(-180, 180, True, True)),
            S("primvars:cycles:camera:aperture_ratio", "Ratio",
              "Distortion to simulate an anamorphic lens bokeh.",
              range=(0.01, 2, True, False)),
        ], open=True),
    ], open=True),
    P("Motion Blur", [
        P("Rolling Shutter", [
            S("primvars:cycles:camera:rolling_shutter_type", "Rolling Shutter",
              "Type of rolling shutter effect matching CMOS-based cameras, with motion blur."),
            S("primvars:cycles:camera:rolling_shutter_duration", "Rolling Shutter Duration",
              "Scanline \"exposure\" time for the rolling shutter effect: 0 scans the image "
              "over the whole shutter, 1 exposes all scanlines over the whole shutter.",
              range=(0, 1, True, True),
              disablewhen="{ primvars:cycles:camera:rolling_shutter_type == none }"),
        ], open=True),
    ], open=True),
]


# Prim path field with Houdini's prim picker button.

# Layout of the per-light settings, following Blender's light properties and object panels.
LIGHT_LAYOUT = [
    P("Light", [
        S("inputs:cycles:light:max_bounces", "Max Bounces",
          "Maximum number of bounces the light will contribute to the render.",
          range=(0, 1024, True, False),
          hidewhen="{ inputs:cycles:light:is_portal == 1 }"),
        S("inputs:cycles:light:use_mis", "Multiple Importance",
          "Use multiple importance sampling for the light, reduces noise for area lights and "
          "sharp glossy materials.",
          disablewhen="{ inputs:cycles:light:is_portal == 1 }"),
        S("inputs:cycles:light:use_caustics", "Shadow Caustics",
          "Generate approximate caustics in shadows of refractive surfaces. Lights, caster and "
          "receiver objects must have shadow caustics options set to enable this."),
        S("inputs:cycles:light:use_soft_falloff", "Soft Falloff",
          "Sphere lights only: apply falloff to avoid sharp edges when the light geometry "
          "intersects with other objects. Off by default, so the light is a sphere like in USD "
          "and Karma (in Blender it is on)."),
        S("inputs:cycles:light:spread", "Spread",
          "Rect and disk lights only: how widely the emitted light fans out, in degrees, as "
          "in the case of a gridded softbox. 180 emits in all directions of the front "
          "hemisphere.",
          range=(0, 180, True, True)),
        S("inputs:cycles:light:is_portal", "Portal",
          "Rect and disk lights only: use the light as a portal. It emits no light itself, but "
          "guides the sampling of the world (dome light) through windows and openings, to "
          "reduce noise in interiors."),
    ], open=True),
    P("Visibility", [
        P("Ray Visibility", [
            S("inputs:cycles:light:visible_camera", "Camera",
              "Light visibility for camera rays. Off by default, like with Karma (in Blender it is "
              "on)."),
            S("inputs:cycles:light:visible_diffuse", "Diffuse",
              "Light contribution to diffuse reflection."),
            S("inputs:cycles:light:visible_glossy", "Glossy",
              "Light contribution to glossy reflection."),
            S("inputs:cycles:light:visible_transmission", "Transmission",
              "Light contribution to transmission."),
            S("inputs:cycles:light:visible_volume_scatter", "Volume Scatter",
              "Light contribution to volume scattering."),
        ], open=True),
    ], open=True),
    P("Light Group", [
        S("inputs:cycles:light:lightgroup", "Light Group",
          "Light group the light belongs to (render it with a light group pass on the Cycles "
          "Render Settings LOP)."),
    ], open=True),
    P("World", [
        S("inputs:cycles:light:world_material", "World Material",
          "Dome lights only: material used as the world, like a Blender world. Its surface "
          "(a Background shader, e.g. with a Sky Texture) replaces the dome light's color and "
          "texture, its volume fills the scene. Sky and environment textures follow the light's "
          "rotation, with their pole on the light's +Y like dome light textures.",
          tags=PRIM_PATH_TAGS),
        S("inputs:cycles:light:map_resolution", "Map Resolution",
          "Dome lights only: resolution of the importance map used to sample the world. 0 is "
          "Auto: the resolution of the environment texture (Blender's Sampling Auto).",
          range=(0, 8192, True, False)),
    ], open=True),
]


def settings(items=LAYOUT):
    """All settings in the layout, depth first."""
    for item in items:
        if "panel" in item:
            yield from settings(item["items"])
        else:
            yield item


def load_sockets(json_path):
    """Map "cycles:<node>:<socket>" names to socket descriptions."""
    with open(json_path, "r") as f:
        data = json.load(f)
    sockets = dict(SESSION_SETTINGS)
    for node in data["nodes"]:
        for socket in node["inputs"]:
            sockets["cycles:%s:%s" % (node["name"], socket["name"])] = socket
    return sockets


def quote(text):
    return '"%s"' % text.replace("\\", "\\\\").replace('"', '\\"')


def format_value(value):
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, float):
        return repr(value)
    if isinstance(value, str):
        return quote(value)
    return str(value)


def encode_condition(condition, encode):
    """Encode the setting names in a disable/hide condition as parameter names."""
    tokens = []
    for token in condition.split():
        tokens.append(encode(token) if token.startswith(("cycles:", "primvars:", "inputs:")) else token)
    return " ".join(tokens)


def menu_items(setting, socket):
    if setting["name"] in MENU_ITEMS:
        items = MENU_ITEMS[setting["name"]]
        missing = [token for token in items if token not in socket["enum_values"]]
        if missing:
            raise RuntimeError("Unknown menu items for %s: %s" % (setting["name"], missing))
        return items
    if setting["name"] == "cycles:device" and sys.platform != "darwin":
        return {k: v for k, v in socket["enum_values"].items() if k != "METAL"}
    return socket["enum_values"]


def encode_parm(name):
    return hou.text.encodeParm(name)


class Writer:
    """Writes the layout as a dialog script.

    mode is "global" (Render Settings LOP tab, with Edit Properties controls), "viewport"
    (viewport display options) or "lop" (Cycles Render Settings LOP parameters, always authored).
    """

    def __init__(self, sockets, mode, encode=encode_parm, layout=LAYOUT):
        self.sockets = sockets
        self.mode = mode
        self.encode = encode
        self.layout = layout
        self.lines = []

    def include(self, setting):
        return self.mode != "viewport" or setting["viewport"]

    def has_settings(self, panel):
        return any(self.include(s) for s in settings(panel["items"]))

    def write_panel(self, panel, depth, parent_conditions):
        indent = "    " * depth
        name = panel["panel"]
        self.lines += [
            "",
            indent + "groupcollapsible {",
            indent + "    name    %s" % quote(
                "cycles_" + name.lower().replace(" ", "_").replace("-", "_") + "_%d" % depth),
            indent + "    label   %s" % quote(name),
        ]
        if panel["open"]:
            self.lines.append(indent + '    grouptag { "group_default" "1" }')
        # Everything in a panel with a header toggle is disabled when the toggle is off.
        conditions = list(parent_conditions)
        if panel["toggle"]:
            conditions.append("{ %s == 0 }" % panel["toggle"])
        for item in panel["items"]:
            if "panel" in item:
                if self.has_settings(item):
                    self.write_panel(item, depth + 1, conditions)
                continue
            if not self.include(item):
                continue
            is_toggle = item["name"] == panel["toggle"]
            self.write_parm(item, name, depth + 1,
                            parent_conditions if is_toggle else conditions)
        if self.mode != "viewport":
            for setting in panel["legacy"]:
                self.write_parm(dict(setting, invisible=True), name, depth + 1, [])
        self.lines.append(indent + "}")

    def write_parm(self, setting, section, depth, disable_conditions):
        socket = self.sockets.get(setting["name"])
        if socket is None:
            raise RuntimeError("Setting %s not found in render_settings.json" % setting["name"])
        usd_type, parm_type, control_type = TYPES[socket["type"]]
        name = setting["name"]
        parm_name = self.encode(name)
        control_name = self.encode(name + "_control")
        with_control = self.mode == "global"
        indent = "    " * depth
        lines = self.lines

        if self.mode == "viewport":
            scope = "viewport toolbar" if setting["toolbar_icon"] else "viewport"
        else:
            scope = "None"

        if with_control:
            lines += [
                indent + "parm {",
                indent + "    name    %s" % quote(control_name),
                indent + "    label   %s" % quote(setting["label"]),
                indent + "    type    string",
                indent + '    default { "none" }',
                indent + "    menujoin {",
                indent + '        [ "import loputils" ]',
                indent + "        [ \"return loputils.createEditPropertiesControlMenu(kwargs, "
                "'%s')\" ]" % control_type,
                indent + "        language python",
                indent + "    }",
                indent + '    parmtag { "sidefx::look" "icon" }',
            ]
            if setting.get("invisible"):
                lines.append(indent + "    invisible")
            if setting["hidewhen"]:
                lines.append(indent + "    hidewhen %s" % quote(
                    encode_condition(setting["hidewhen"], self.encode)))
            lines.append(indent + "}")

        lines += [
            indent + "parm {",
            indent + "    name    %s" % quote(parm_name),
            indent + "    label   %s" % quote(setting["label"]),
            indent + "    type    %s" % parm_type,
            indent + "    size    1",
            indent + "    default { %s }" % format_value(socket["default"]),
        ]
        if setting["range"]:
            low, high, low_strict, high_strict = setting["range"]
            lines.append(indent + "    range   { %s%s %s%s }" % (
                low, "!" if low_strict else "", high, "!" if high_strict else ""))
        if socket["type"] == "menu":
            lines.append(indent + "    menu {")
            for token, label in menu_items(setting, socket).items():
                lines.append(indent + "        %s %s" % (quote(token), quote(label)))
            lines.append(indent + "    }")
        lines.append(indent + "    help    %s" % quote(setting["help"]))
        for tag, value in setting.get("tags", ()):
            lines.append(indent + "    parmtag { %s %s }" % (quote(tag), quote(value)))
        if self.mode != "lop":
            lines.append(indent + '    parmtag { "spare_category" %s }' % quote(section))
            lines.append(indent + '    parmtag { "uiscope" %s }' % quote(scope))
            lines.append(indent + '    parmtag { "usdvaluetype" %s }' % quote(usd_type))
        if setting["toolbar_icon"] and scope != "None":
            lines.append(indent + '    parmtag { "uiicon" %s }' % setting["toolbar_icon"])
        disable = []
        if with_control:
            disable.append("{ %s == block } { %s == none }" % (control_name, control_name))
        disable += disable_conditions
        if setting["disablewhen"]:
            disable.append(setting["disablewhen"])
        if disable:
            lines.append(indent + "    disablewhen %s" % quote(
                encode_condition(" ".join(disable), self.encode)))
        if setting["hidewhen"]:
            lines.append(indent + "    hidewhen %s" % quote(
                encode_condition(setting["hidewhen"], self.encode)))
        if setting.get("invisible"):
            lines.append(indent + "    invisible")
        lines.append(indent + "}")

    def generate(self):
        lines = self.lines
        if self.mode == "lop":
            lines += ["{", '    name    "cycles"', '    label   "Cycles"']
        else:
            lines += [
                "/// Automatically generated by rendersettings2ds.py, do not edit.",
                "/// %s for the Cycles render delegate." % (
                    "Viewport display options" if self.mode == "viewport" else
                    "Object settings" if self.layout is GEOMETRY_LAYOUT else
                    "Light settings" if self.layout is LIGHT_LAYOUT else
                    "Camera settings" if self.layout is CAMERA_LAYOUT else "Render settings"),
                "",
                '#include "$HFS/houdini/soho/parameters/CommonMacros.ds"',
                "",
                "{",
                '    name    "cycles"',
                '    label   "Cycles"',
                '    parmtag { spare_opfilter "!!SHOP/PROPERTIES!!" }',
                '    parmtag { spare_classtags "render" }',
            ]
        for panel in self.layout:
            if self.has_settings(panel):
                self.write_panel(panel, 1, [])
        lines += ["}", ""]
        return "\n".join(lines)


def generate(sockets, viewport):
    return Writer(sockets, "viewport" if viewport else "global").generate()


def check_blender_defaults(sockets):
    """Every Blender default must be a setting in the layout, menu values one of its items."""
    layout = {setting["name"]: setting for setting in settings()}
    for name, value in BLENDER_DEFAULTS.items():
        if name not in layout:
            raise RuntimeError("Blender default %s is not in the layout" % name)
        socket = sockets[name]
        if socket["type"] == "menu" and value not in menu_items(layout[name], socket):
            raise RuntimeError("Blender default %s = %r is not a menu item" % (name, value))
        if value == socket["default"]:
            raise RuntimeError("Blender default %s = %r is the delegate default" % (name, value))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--json", required=True, help="Path to render_settings.json")
    parser.add_argument("--ds-install-dir", required=True,
                        help="Directory to write the dialog scripts to (soho/parameters)")
    args = parser.parse_args()

    sockets = load_sockets(args.json)
    check_blender_defaults(sockets)
    os.makedirs(args.ds_install_dir, exist_ok=True)

    for suffix, ds in (("Global", generate(sockets, False)),
                       ("Viewport", generate(sockets, True)),
                       ("Geometry", Writer(GEOMETRY_SETTINGS, "global",
                                           layout=GEOMETRY_LAYOUT).generate()),
                       ("Light", Writer(LIGHT_SETTINGS, "global",
                                        layout=LIGHT_LAYOUT).generate()),
                       ("Camera", Writer(CAMERA_SETTINGS, "global",
                                         layout=CAMERA_LAYOUT).generate())):

        # Validate by parsing the dialog script like Houdini does.
        group = hou.ParmTemplateGroup()
        group.setToDialogScript(ds)

        path = os.path.join(args.ds_install_dir, "HdCyclesPlugin_%s.ds" % suffix)
        with open(path, "w", newline="\n") as f:
            f.write(ds)
        log("Wrote %s" % path)

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        log("Error: %s" % e)
        sys.exit(1)
