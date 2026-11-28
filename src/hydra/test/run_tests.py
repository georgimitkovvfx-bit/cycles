# SPDX-FileCopyrightText: 2026 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

"""Render tests of the Hydra delegate through cycles_usd_render (no host application needed).

Each test renders a small USD stage and checks a number known from physics, from the colour space maths, or from
Karma (the reference renderer in Houdini): a dome's radiance, textures' colour spaces, MaterialX, light and shadow
links, implicit shapes and stage units, curves of USD's bases. Run by ctest (src/app/CMakeLists.txt) after `make`,
or by hand:

    lib/linux_x64/python/bin/python3.13 src/hydra/test/run_tests.py --renderer install/cycles_usd_render \\
        [--device CPU|CUDA] [--keep <folder>] [test names]

Needs OpenImageIO's and numpy's Python modules: those of the precompiled libraries (--lib, default <repo>/lib/<platform>).
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]

HEADER = """#usda 1.0
(
    defaultPrim = "world"
    upAxis = "Z"
    metersPerUnit = %s
)
"""

PLANE = """def Xform "world"
{
    def Mesh "plane" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        point3f[] points = [(-500,-500,0),(500,-500,0),(500,500,0),(-500,500,0)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0,1,2,3]
        texCoord2f[] primvars:st = [(0,0),(1,0),(1,1),(0,1)] (interpolation = "vertex")
        normal3f[] normals = [(0,0,1),(0,0,1),(0,0,1),(0,0,1)] (interpolation = "vertex")
        rel material:binding = </world/mat>
    }
    def Material "mat"
    {
%(material)s
    }
    def DomeLight "sky"
    {
        float inputs:intensity = 1
    }
    def Camera "cam"
    {
        float focalLength = 50
        float horizontalAperture = 36
        float verticalAperture = 36
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = (0, 0, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
"""

PREVIEW = """        token outputs:surface.connect = </world/mat/surface.outputs:surface>
        def Shader "surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            %(diffuse)s
            float inputs:roughness = 1
            int inputs:useSpecularWorkflow = 1
            color3f inputs:specularColor = (0, 0, 0)
            token outputs:surface
        }
        def Shader "texture"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @./grey128.png@
            %(color_space)s
            float2 inputs:st.connect = </world/mat/st.outputs:result>
            float3 outputs:rgb
        }
        def Shader "st"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            token inputs:varname = "st"
            float2 outputs:result
        }"""

MATERIALX = """        token outputs:mtlx:surface.connect = </world/mat/pbr.outputs:out>
        token outputs:surface.connect = </world/mat/preview.outputs:surface>
        def Shader "pbr"
        {
            uniform token info:id = "ND_open_pbr_surface_surfaceshader"
            %(base_color)s
            %(normal)s
            float inputs:specular_weight = 0
            token outputs:out
        }
        def Shader "bump"
        {
            uniform token info:id = "ND_bump_vector3"
            float inputs:height = 0.3
            float inputs:scale = 2
            vector3f outputs:out
        }
        def Shader "image"
        {
            uniform token info:id = "ND_image_color3"
            asset inputs:file = @./grey128.png@ (colorSpace = "srgb_texture")
            color3f outputs:out
        }
        def Shader "double"
        {
            uniform token info:id = "ND_multiply_color3FA"
            color3f inputs:in1.connect = </world/mat/image.outputs:out>
            float inputs:in2 = 2
            color3f outputs:out
        }
        def Shader "preview"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.05, 0.05, 0.05)
            token outputs:surface
        }"""


def box(name: str, x: float, y: float, z: float, sx: float, sy: float, sz: float) -> str:
    """A mesh box with flat faces (every face its own corners)."""
    corners = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1), (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]
    faces = [[0, 3, 2, 1], [4, 5, 6, 7], [0, 1, 5, 4], [1, 2, 6, 5], [2, 3, 7, 6], [3, 0, 4, 7]]
    points = ", ".join(f"({x + c[0] * sx:g}, {y + c[1] * sy:g}, {z + c[2] * sz:g})"
                       for face in faces for i in face for c in [corners[i]])
    return f"""    def Mesh "{name}"
    {{
        point3f[] points = [{points}]
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [{", ".join(str(i) for i in range(24))}]
        uniform token subdivisionScheme = "none"
    }}
"""


def look_at(eye, target) -> str:
    """A camera transform (row-major, USD's) looking from eye at target, Z up."""
    import numpy as np

    eye, target = np.array(eye, float), np.array(target, float)
    back = eye - target
    back /= np.linalg.norm(back)
    right = np.cross([0, 0, 1], back)
    right /= np.linalg.norm(right)
    up = np.cross(back, right)
    rows = [(*right, 0), (*up, 0), (*back, 0), (*eye, 1)]
    return "(" + ", ".join("(" + ", ".join(f"{v:.9g}" for v in row) + ")" for row in rows) + ")"


def camera(focal: float, eye, target, aperture=(36, 20.25)) -> str:
    return f"""    def Camera "cam"
    {{
        float focalLength = {focal}
        float horizontalAperture = {aperture[0]}
        float verticalAperture = {aperture[1]}
        matrix4d xformOp:transform = {look_at(eye, target)}
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }}
"""


def linked_cubes(link: str, floor: bool) -> str:
    """Two cubes (on a floor) under one sphere light. link: "" (lights all), "light" (lights the left cube only) or
    "shadow" (only the left cube casts its shadow)."""
    if link:
        name = "lightLink" if link == "light" else "shadowLink"
        metadata = f' (\n        prepend apiSchemas = ["CollectionAPI:{name}"]\n    )'
        collection = (f"        uniform bool collection:{name}:includeRoot = 0\n"
                      f"        rel collection:{name}:includes = </world/left>\n")
    else:
        metadata = collection = ""
    lamp = f"""    def SphereLight "lamp"{metadata}
    {{
        float inputs:intensity = 2000
        float inputs:radius = 0.2
        double3 xformOp:translate = (0, -1, 6)
        uniform token[] xformOpOrder = ["xformOp:translate"]
{collection}    }}
"""
    body = box("left", -2.5, 0, 1, 1, 1, 1) + box("right", 2.5, 0, 1, 1, 1, 1)
    if floor:
        body += box("floor", 0, 0, -0.05, 8, 8, 0.05)
    return HEADER % 1 + 'def Xform "world"\n{\n' + camera(35, (0, -12, 3), (0, 0, 0.5)) + lamp + body + "}\n"


def lit_cube(meters: float, implicit: bool, lit: bool = True) -> str:
    """A 2-unit cube seen from a corner, lit by a 0.5-unit sphere light: the same picture in any unit."""
    body = camera(50, (6, -6, 4.5), (0, 0, 0))
    if lit:
        body += """    def SphereLight "lamp"
    {
        float inputs:intensity = 3000
        float inputs:radius = 0.5
        double3 xformOp:translate = (3, -8, 7)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
"""
    if implicit:
        body += '    def Cube "box"\n    {\n        double size = 2\n    }\n'
    else:
        body += box("box", 0, 0, 0, 1, 1, 1)
    return HEADER % meters + 'def Xform "world"\n{\n' + body + "}\n"


CURVES = """def Xform "world"
{
    def DomeLight "sky"
    {
    }
    def Camera "cam"
    {
        float focalLength = 35
        float horizontalAperture = 36
        float verticalAperture = 20.25
        double3 xformOp:translate = (0, 0, 12)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def BasisCurves "cubic"
    {
        uniform token type = "cubic"
        uniform token basis = "catmullRom"
        int[] curveVertexCounts = [4]
        point3f[] points = [(-4, 3, 0), (-1, 3.5, 0), (1, 2.5, 0), (4, 3, 0)]
        float[] widths = [0.3] (interpolation = "constant")
    }
}
"""


class Renderer:
    def __init__(self, path: str, device: str, folder: Path, lib: Path):
        self.path, self.device, self.folder = path, device, folder
        sys.path.insert(0, str(lib / "openimageio" / "lib" / f"python{sys.version_info.major}.{sys.version_info.minor}"
                               / "site-packages"))
        import OpenImageIO  # noqa: F401  (fails early with a clear message)

        grey = b"".join(b"\x00" + bytes([128] * 48) for _ in range(16))

        def chunk(kind: bytes, data: bytes) -> bytes:
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

        (folder / "grey128.png").write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 2, 0, 0, 0))
                                            + chunk(b"IDAT", zlib.compress(grey)) + chunk(b"IEND", b""))

    def __call__(self, name: str, usda: str, size=(48, 48), samples=32, camera_path="/world/cam", channel=""):
        """Linear RGB pixels (rows top down) of the stage; with ``channel`` (as "Depth.Z"), that channel of the
        multilayer EXR the stage's RenderVars ask for."""
        import numpy as np
        import OpenImageIO as oiio

        usd = self.folder / f"{name}.usda"
        usd.write_text(usda)
        command = [self.path, str(usd), str(self.folder / f"{name}.####.exr"), "--frames", "1",
                   "--resolution", f"{size[0]}x{size[1]}", "--samples", str(samples), "--camera", camera_path,
                   "--device", self.device]
        result = subprocess.run(command, capture_output=True, text=True, timeout=600, check=False)
        self.output = result.stdout
        image = self.folder / f"{name}.0001.exr"
        if not image.is_file():
            raise AssertionError(f"{name}: no image\n{result.stdout[-2000:]}\n{result.stderr[-2000:]}")
        buffer = oiio.ImageBuf(str(image))
        pixels = np.asarray(buffer.get_pixels(oiio.FLOAT))
        if channel:
            names = list(buffer.spec().channelnames)
            found = [i for i, n in enumerate(names) if n == channel or n.endswith("." + channel)]
            if not found:
                raise AssertionError(f"{name}: no {channel} channel in {names}")
            return pixels[..., found[0]]
        return pixels[..., :3]


def middle(pixels) -> float:
    n = pixels.shape[0]
    return float(pixels[n // 3: 2 * n // 3, n // 3: 2 * n // 3].mean())


def plane(material: str) -> str:
    return HEADER % 1 + PLANE % {"material": material}


def test_dome_radiance(render):
    """A 0.5 albedo plane under a white dome of intensity 1 reflects 0.5 (the dome's intensity is its radiance)."""
    value = middle(render("dome", plane(PREVIEW % {"diffuse": "color3f inputs:diffuseColor = (0.5, 0.5, 0.5)",
                                                     "color_space": ""})))
    assert abs(value - 0.5) < 0.03, value


def test_texture_colour_spaces(render):
    """sRGB 128 decodes to 0.214 (sRGB, and auto for 8-bit files); raw keeps 0.5."""
    textured = "color3f inputs:diffuseColor.connect = </world/mat/texture.outputs:rgb>"
    values = {space: middle(render(f"texture_{space}", plane(PREVIEW % {
        "diffuse": textured, "color_space": f'token inputs:sourceColorSpace = "{space}"' if space != "auto" else ""})))
        for space in ("sRGB", "auto", "raw")}
    assert abs(values["sRGB"] - 0.214) < 0.03 and abs(values["auto"] - 0.214) < 0.03, values
    assert abs(values["raw"] - 0.5) < 0.03, values


def test_materialx(render):
    """OpenPBR with a 0.5 base colour reflects 0.5 (its preview surface, 0.05, must not be used); an sRGB image × 2
    feeding the base colour gives 0.428 (a relative file path, resolved)."""
    flat = middle(render("mtlx_flat", plane(MATERIALX % {"base_color": "color3f inputs:base_color = (0.5, 0.5, 0.5)",
                                                         "normal": ""})))
    image = middle(render("mtlx_image", plane(MATERIALX % {
        "base_color": "color3f inputs:base_color.connect = </world/mat/double.outputs:out>", "normal": ""})))
    assert abs(flat - 0.5) < 0.03, flat
    assert abs(image - 0.428) < 0.03, image


def test_materialx_shader_named_as_material(render):
    """A surface shader named as its material (usdMtlx's looks: Materials/Bishop_B/Bishop_B, MaterialX's chess set):
    HdMtlx names the material node after the shader and renames the shader, the material node was taken for the
    shader (no shader graph). The OpenPBR of a 0.5 base colour reflects 0.5."""
    material = MATERIALX.replace("/world/mat/pbr.", "/world/mat/mat.").replace('def Shader "pbr"', 'def Shader "mat"')
    value = middle(render("mtlx_same_name", plane(material % {
        "base_color": "color3f inputs:base_color = (0.5, 0.5, 0.5)", "normal": ""})))
    assert abs(value - 0.5) < 0.03, value


def test_materialx_bump(render):
    """MaterialX's bump (heighttonormal into normalmap) of a constant height leaves a flat plane as it is: 0.5 under
    the dome, as without it. Cycles' Bump node's world space normal decoded again as a tangent space color tilted the
    normal: bumped surfaces rendered nearly black."""
    value = middle(render("mtlx_bump", plane(MATERIALX % {
        "base_color": "color3f inputs:base_color = (0.5, 0.5, 0.5)",
        "normal": "vector3f inputs:geometry_normal.connect = </world/mat/bump.outputs:out>"})))
    assert abs(value - 0.5) < 0.03, value


def unlit(shaders: str, value: str, kind: str = "float") -> str:
    """A material showing a MaterialX value (``value``: the output's path) as unlit emission: the picture is the
    value, whatever the lights (a float or vector through a convert node)."""
    sdf = {"float": "float", "vector2": "float2", "vector3": "vector3f", "vector4": "float4", "color3": "color3f"}
    convert = "" if kind == "color3" else f"""
        def Shader "convert"
        {{
            uniform token info:id = "ND_convert_{kind}_color3"
            {sdf[kind]} inputs:in.connect = <{value}>
            color3f outputs:out
        }}"""
    shown = value if kind == "color3" else "/world/mat/convert.outputs:out"
    return f"""        token outputs:mtlx:surface.connect = </world/mat/unlit.outputs:out>
{shaders}{convert}
        def Shader "unlit"
        {{
            uniform token info:id = "ND_surface_unlit"
            color3f inputs:emission_color.connect = <{shown}>
            token outputs:out
        }}"""


def mtlx_node(name: str, node_id: str, inputs: str, kind: str = "float") -> str:
    sdf = {"float": "float", "vector2": "float2", "vector3": "vector3f", "vector4": "float4", "color3": "color3f",
           "matrix33": "matrix3d", "edf": "token"}
    return f"""
        def Shader "{name}"
        {{
            uniform token info:id = "{node_id}"
            {inputs}
            {sdf[kind]} outputs:out
        }}"""


def test_materialx_values(render):
    """MaterialX nodes whose translation disagreed with MaterialX's own OSL and with Karma (materialx_coverage.py):
    top and bottom of ramptb / splittb (MaterialX's top is at v = 0), vector4 dot products, lengths and normalize with
    their fourth component, facingratio's default view direction (1 facing the camera), tiledcircles (a vector2's Z
    of uv * 2 - 1 was -1 and moved every circle out: black), roughness_anisotropy as pbrlib defines it (it was
    isotropic), tangent and bitangent along the UV map (Cycles' geometry tangent is radial)."""
    ramp = 'float inputs:valuet = 0.2\n            float inputs:valueb = 0.6\n            float2 inputs:texcoord = (0.5, 0.25)'
    vec4 = "float4 inputs:in1 = (0.1, 0.2, 0.2, 0.4)\n            float4 inputs:in2 = (0.1, 0.2, 0.2, 0.4)"
    cases = {
        # mix(valuet, valueb, v): 0.2 + 0.25 * 0.4
        "ramptb": (mtlx_node("n", "ND_ramptb_float", ramp), "/world/mat/n.outputs:out", "float", 0.3),
        # v = 0.25 below the center 0.5: valuet
        "splittb": (mtlx_node("n", "ND_splittb_float", ramp), "/world/mat/n.outputs:out", "float", 0.2),
        "dotproduct": (mtlx_node("n", "ND_dotproduct_vector4", vec4), "/world/mat/n.outputs:out", "float", 0.25),
        "magnitude": (mtlx_node("n", "ND_magnitude_vector4", vec4.split("\n")[0].replace("in1", "in")),
                      "/world/mat/n.outputs:out", "float", 0.5),
        # (0.1, 0.2, 0.2, 0.4) / 0.5: its X
        "normalize": (mtlx_node("n", "ND_normalize_vector4", vec4.split("\n")[0].replace("in1", "in"), "vector4"),
                      "/world/mat/n.outputs:out", "vector4", 0.2),
        "facingratio": (mtlx_node("n", "ND_facingratio_float", ""), "/world/mat/n.outputs:out", "float", 1.0),
        # alpha 0.25 / sqrt(0.5), its X (the Y, 0.25 * sqrt(0.5), below)
        "roughness_anisotropy": (mtlx_node("n", "ND_roughness_anisotropy", "float inputs:roughness = 0.5\n"
                                           "            float inputs:anisotropy = 0.5", "vector2"),
                                 "/world/mat/n.outputs:out", "vector2", 0.3536),
        # The plane's U runs along +X: MaterialX's tangent (Cycles' radial one was about (0.25, 0.97, 0))
        "tangent": (mtlx_node("n", "ND_tangent_vector3", "", "vector3"), "/world/mat/n.outputs:out", "vector3", 1.0),
        "tiledcircles": (mtlx_node("n", "ND_tiledcircles_color3", "", "color3"), "/world/mat/n.outputs:out", "color3",
                         1.0),
    }
    # Second components: (name, case, channel, expected)
    others = [("roughness_anisotropy_y", "roughness_anisotropy", 1, 0.1768), ("bitangent", "bitangent", 1, 1.0)]
    cases["bitangent"] = (mtlx_node("n", "ND_bitangent_vector3", "", "vector3"), "/world/mat/n.outputs:out", "vector3",
                          0.0)
    values, pixels = {}, {}
    for name, (shaders, value, kind, expected) in cases.items():
        pixels[name] = render(f"mtlx_value_{name}", plane(unlit(shaders, value, kind)))
        values[name] = (float(pixels[name][16:32, 16:32, 0].mean()), expected)
    for name, case, channel, expected in others:
        values[name] = (float(pixels[case][16:32, 16:32, channel].mean()), expected)
    wrong = {name: v for name, v in values.items() if abs(v[0] - v[1]) > 0.01}
    assert not wrong, f"(got, expected): {wrong}"


def test_materialx_matrices(render):
    """MaterialX's matrices (the translator had none: matrix math, transformmatrix and cmlib's colour-space
    conversions rendered black). MaterialX multiplies row vectors, out_j = sum_i in_i m_ij, the last row translating:
    a column-vector reading gives other numbers in every case below."""
    m33 = "matrix3d inputs:mat = ((1, 2, 0), (0, 1, 0), (0, 0, 1))"
    cases = {
        # y = 0.1 * 2 + 0.2 * 1 (columns would give 0.2)
        "transform33": (mtlx_node("n", "ND_transformmatrix_vector3", "vector3f inputs:in = (0.1, 0.2, 0.3)\n"
                                  f"            {m33}", "vector3"), "vector3", 1, 0.4),
        # A point: z = 0.3 + 0.25 from the fourth row
        "transform34": (mtlx_node("n", "ND_transformmatrix_vector3M4", "vector3f inputs:in = (0.1, 0.2, 0.3)\n"
                                  "            matrix4d inputs:mat = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), "
                                  "(0, 0, 0.25, 1))", "vector3"), "vector3", 2, 0.55),
        "determinant": (mtlx_node("n", "ND_determinant_matrix33",
                                  "matrix3d inputs:in = ((2, 0, 0), (0, 3, 0), (1, 1, 0.1))"), "float", 0, 0.6),
        # inverse(diag(2, 4, 5)) * (1, 1, 1): its Y
        "invert": (mtlx_node("i", "ND_invertmatrix_matrix33",
                             "matrix3d inputs:in = ((2, 0, 0), (0, 4, 0), (0, 0, 5))", "matrix33")
                   + mtlx_node("n", "ND_transformmatrix_vector3", "vector3f inputs:in = (1, 1, 1)\n"
                               "            matrix3d inputs:mat.connect = </world/mat/i.outputs:out>", "vector3"),
                   "vector3", 1, 0.25),
        # ACEScg green in linear Rec.709: the matrix's second row, its green 1.1408
        "acescg": (mtlx_node("n", "ND_acescg_to_lin_rec709_color3", "color3f inputs:in = (0, 1, 0)", "color3"),
                   "color3", 1, 1.1408),
    }
    values = {}
    for name, (shaders, kind, channel, expected) in cases.items():
        pixels = render(f"mtlx_matrix_{name}", plane(unlit(shaders, "/world/mat/n.outputs:out", kind)))
        values[name] = (float(pixels[16:32, 16:32, channel].mean()), expected)
    wrong = {name: v for name, v in values.items() if abs(v[0] - v[1]) > 0.01}
    assert not wrong, f"(got, expected): {wrong}"


def test_materialx_edfs_and_artistic_ior(render):
    """The MaterialX nodes the translator did not support (they rendered black): generalized_schlick_edf (color0
    facing the viewer, color90 at grazing angles), conical_edf (light only inside its cone around its normal),
    an EDF multiplied by a colour (exactly: it was the colour's average), artistic_ior (Gulbrandsen's complex IOR:
    reflectivity 0.5 and edge colour 0.5 make an IOR of 3.0809)."""
    def emitting(edf: str) -> str:
        return """        token outputs:mtlx:surface.connect = </world/mat/s.outputs:out>""" + edf + """
        def Shader "s"
        {
            uniform token info:id = "ND_surface"
            token inputs:edf.connect = </world/mat/n.outputs:out>
            token outputs:out
        }"""

    uniform = '\n        def Shader "u"\n        {\n            uniform token info:id = "ND_uniform_edf"\n' \
              '            color3f inputs:color = (1, 1, 1)\n            token outputs:out\n        }'
    tilted = "vector3f inputs:normal = (0.9397, 0, 0.3420)"  # 70 degrees from the view (64-76 over the pixels read)
    cases = {
        "schlick": (emitting(uniform + mtlx_node("n", "ND_generalized_schlick_edf", "color3f inputs:color0 = (0.2, 0.2, 0.2)"
                                                 "\n            color3f inputs:color90 = (1, 1, 1)\n"
                                                 "            token inputs:base.connect = </world/mat/u.outputs:out>",
                                                 "edf")), 0, 0.2),
        "multiply": (emitting(uniform + mtlx_node("n", "ND_multiply_edfC", "token inputs:in1.connect = </world/mat/u.outputs:out>"
                                                  "\n            color3f inputs:in2 = (0.2, 0.5, 0.8)", "edf")), 0, 0.2),
        "cone_outside": (emitting(mtlx_node("n", "ND_conical_edf", f"{tilted}\n            float inputs:inner_angle = 30",
                                            "edf")), 0, 0.0),
        "cone_inside": (emitting(mtlx_node("n", "ND_conical_edf", f"{tilted}\n            float inputs:inner_angle = 85",
                                           "edf")), 0, 1.0),
        "artistic_ior": (unlit(mtlx_node("n", "ND_artistic_ior", "color3f inputs:reflectivity = (0.5, 0.5, 0.5)\n"
                                         "            color3f inputs:edge_color = (0.5, 0.5, 0.5)\n"
                                         "            vector3f outputs:ior", "vector3"),
                               "/world/mat/n.outputs:ior", "vector3"), 0, 3.0809),
    }
    values = {}
    for name, (material, channel, expected) in cases.items():
        pixels = render(f"mtlx_edf_{name}", plane(material))
        values[name] = (float(pixels[16:32, 16:32, channel].mean()), expected)
    wrong = {name: v for name, v in values.items() if abs(v[0] - v[1]) > 0.01 + 0.01 * v[1]}
    assert not wrong, f"(got, expected): {wrong}"


def test_materialx_layering_conserves_energy(render):
    """MaterialX's layer as its GLSL (Storm) layers: the base under the top's throughput, 1 - the top's directional
    albedo. A white furnace: a clear coat (dielectric, IOR 3, so 25 % reflection head-on) over a white diffuse base,
    under a uniform white dome, reflects all of the light, 1 (the translator added the layers: 1.25 head-on).
    A metal over the white base lets nothing through: the metal alone (below 1; the sum was the metal + 1). Lama's
    dielectric (a reflection over a transmission over its interior) layered over the white base: its transmission goes
    into the base, so the furnace stays near 1 (0.96: MaterialX's GLSL counts the coat's throughput twice); the
    transmission through the plane was added too (about 2)."""
    def surface(shaders: str, layer_id: str, top_input: str, base_input: str) -> str:
        return f"""        token outputs:mtlx:surface.connect = </world/mat/s.outputs:out>{shaders}
        def Shader "l"
        {{
            uniform token info:id = "{layer_id}"
            token inputs:{top_input}.connect = </world/mat/top.outputs:out>
            token inputs:{base_input}.connect = </world/mat/base.outputs:out>
            token outputs:out
        }}
        def Shader "s"
        {{
            uniform token info:id = "ND_surface"
            token inputs:bsdf.connect = </world/mat/l.outputs:out>
            token outputs:out
        }}"""

    def layered(top: str, base: str) -> str:
        return surface(top + base, "ND_layer_bsdf", "top", "base")

    coat = mtlx_node("top", "ND_dielectric_bsdf", "float inputs:ior = 3\n            float2 inputs:roughness = (0.05, 0.05)",
                     "edf")
    white = mtlx_node("base", "ND_oren_nayar_diffuse_bsdf", "color3f inputs:color = (1, 1, 1)\n"
                      "            float inputs:roughness = 0", "edf")
    metal = mtlx_node("top", "ND_conductor_bsdf", "float2 inputs:roughness = (0.05, 0.05)", "edf")
    lama = mtlx_node("top", "ND_lama_dielectric", "", "edf")
    furnace = middle(render("mtlx_layer_furnace", plane(layered(coat, white))))
    metal_over_white = middle(render("mtlx_layer_metal", plane(layered(metal, white))))
    lama_furnace = middle(render("mtlx_layer_lama", plane(surface(lama + white, "ND_lama_layer_bsdf", "materialTop",
                                                                  "materialBase"))))
    assert abs(furnace - 1.0) < 0.03, furnace
    assert 0.3 < metal_over_white < 1.0, metal_over_white
    assert 0.9 < lama_furnace < 1.03, lama_furnace


def test_materialx_sheen_is_conty_kulla(render):
    """MaterialX's default sheen is Conty-Kulla's (Karma's, Storm's); Cycles' sheen is Zeltner's, which fades out at low
    roughness. The translator gives Cycles' sheen the roughness of the same average albedo and scales its colour by
    the ratio of the two directional albedos: a white sheen of roughness 0.3 facing the camera under a white dome
    reflects Conty-Kulla's 0.052 head-on (Zeltner's: 0.002, Karma's render 7 times brighter than ours)."""
    material = """        token outputs:mtlx:surface.connect = </world/mat/s.outputs:out>
        def Shader "sheen"
        {
            uniform token info:id = "ND_sheen_bsdf"
            color3f inputs:color = (1, 1, 1)
            float inputs:roughness = 0.3
            token outputs:out
        }
        def Shader "s"
        {
            uniform token info:id = "ND_surface"
            token inputs:bsdf.connect = </world/mat/sheen.outputs:out>
            token outputs:out
        }"""
    value = middle(render("mtlx_sheen", plane(material), samples=128))
    assert abs(value - 0.052) < 0.01, value


def test_materialx_anisotropy_follows_uv(render):
    """An anisotropic Standard Surface metal stretches its highlight along the UV map's tangent, as MaterialX (and
    Karma) do: swapping U and V turns the highlight, so a sun off to the side of the mirror direction lights the plane
    differently. Cycles' own default tangent is radial (the same for both UV maps)."""
    material = """        token outputs:mtlx:surface.connect = </world/mat/metal.outputs:out>
        def Shader "metal"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            float inputs:metalness = 1
            float inputs:base = 1
            color3f inputs:base_color = (1, 1, 1)
            float inputs:specular_roughness = 0.3
            float inputs:specular_anisotropy = 0.8
            token outputs:out
        }"""
    sun = ('    def DistantLight "sun"\n    {\n        float inputs:intensity = 20\n'
           '        float3 xformOp:rotateXYZ = (0, 12, 0)\n        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]\n'
           '    }\n    def DomeLight "sky"')
    # The sun only: a uniform dome reflects the same whichever way the highlight stretches.
    stage = plane(material).replace('    def DomeLight "sky"', sun).replace(
        "float inputs:intensity = 1", "float inputs:intensity = 0")
    swapped = stage.replace("[(0,0),(1,0),(1,1),(0,1)]", "[(0,0),(0,1),(1,1),(1,0)]")
    along, across = middle(render("aniso_u", stage)), middle(render("aniso_v", swapped))
    assert max(along, across) > 1.5 * min(along, across), (along, across)


def test_osl_shading_system(render):
    """A stage's cycles:shading_system turns OSL on in cycles_usd_render (it made the scene before the delegate read
    the setting, so script nodes were dropped), and Cycles' own OSL nodes are found in this repository's install (an
    emission's node_emission.oso was not): a script node's 0.25 through an emission renders 0.25."""
    (render.folder / "quarter.osl").write_text("shader quarter(output color out = 0.25) { out = 0.25; }\n")
    material = """        token outputs:cycles:surface.connect = </world/mat/emission.outputs:emission>
        def Shader "script"
        {
            uniform token info:id = "cycles_script"
            asset inputs:filepath = @./quarter.osl@
            color3f outputs:out
        }
        def Shader "emission"
        {
            uniform token info:id = "cycles_emission"
            color3f inputs:color.connect = </world/mat/script.outputs:out>
            float inputs:strength = 1
            token outputs:emission
        }"""
    stage = plane(material).replace("    metersPerUnit = 1\n", '    metersPerUnit = 1\n    renderSettingsPrimPath = "/Render/settings"\n', 1)
    stage += """
def Scope "Render"
{
    def RenderSettings "settings"
    {
        rel camera = </world/cam>
        custom bool cycles:shading_system = 1
    }
}
"""
    if render.device != "CPU":
        return
    value = middle(render("osl_script", stage))
    assert abs(value - 0.25) < 0.01, value


def test_shader_registry(render):
    """Cycles' shader registry (sdrCycles, hydra/registry.h): its nodes in USD's shader registry as `cycles_<node>`,
    each input of the USD type USD Nodes and the delegate write (UsdShade's validation checks authored inputs against
    it: `bool`, `token` for menus and closures, `asset` for file names, no dots in names), with a label and the
    Houdini package's menu category. The category table (resources/sdr/categories.json) covers exactly Cycles'
    nodes: run houdini/src/sdr_categories.py again when they change."""
    import json

    result = subprocess.run([render.path, "--shaders"], capture_output=True, text=True, timeout=120, check=False)
    assert result.returncode == 0, result.stderr[-2000:]
    nodes = json.loads(result.stdout)
    assert len(nodes) > 90, len(nodes)
    principled = nodes["cycles_principled_bsdf"]
    assert principled["context"] == "surface" and principled["category"] == "Cycles/Shader", principled
    expected = {("cycles_principled_bsdf", "base_color"): "color3f", ("cycles_principled_bsdf", "distribution"): "token",
                ("cycles_image_texture", "filename"): "asset", ("cycles_image_texture", "tex_mapping_scale"): "vector3f",
                ("cycles_math", "use_clamp"): "bool", ("cycles_math", "value1"): "float",
                ("cycles_mix_closure", "closure1"): "token"}
    wrong = {key: nodes[key[0]]["inputs"].get(key[1]) for key, kind in expected.items()
             if nodes[key[0]]["inputs"].get(key[1]) != kind}
    assert not wrong, wrong
    assert nodes["cycles_principled_bsdf"]["outputs"] == {"BSDF": "token"}, nodes["cycles_principled_bsdf"]["outputs"]
    assert not [name for node in nodes.values() for name in node["inputs"] if "." in name]
    table = json.loads((REPO / "src" / "hydra" / "resources" / "sdr" / "categories.json").read_text())
    published = {name[len("cycles_"):] for name in nodes}
    tabled = {name for name in table if not name.startswith("convert_")}
    assert published == tabled, (sorted(published - tabled), sorted(tabled - published))


def test_light_and_shadow_links(render):
    """collection:lightLink lights only the left cube; collection:shadowLink leaves the right cube without shadow."""
    size = (192, 108)
    half = size[0] // 2
    everything = render("links_all", linked_cubes("", False), size).mean(axis=2)
    only_left = render("links_light", linked_cubes("light", False), size).mean(axis=2)
    assert everything[:, :half].mean() > 0.02 and everything[:, half:].mean() > 0.02, "both cubes lit"
    assert abs(only_left[:, :half].mean() / everything[:, :half].mean() - 1) < 0.15, "the left cube lit"
    assert only_left[:, half:].mean() < 0.1 * everything[:, half:].mean(), "the right cube dark"
    import numpy as np

    with_shadows = render("links_floor", linked_cubes("", True), size).mean(axis=2)
    without = render("links_shadow", linked_cubes("shadow", True), size).mean(axis=2)
    floor_right = np.zeros_like(with_shadows, dtype=bool)
    floor_right[int(size[1] * 0.5):int(size[1] * 0.62), int(size[0] * 0.76):int(size[0] * 0.95)] = True  # rows top down
    assert without[floor_right].mean() > with_shadows[floor_right].mean() * 1.3, "the right cube casts no shadow"


def test_implicit_cube_and_stage_units(render):
    """An implicit Cube renders like a mesh cube with flat faces, also in a centimetre stage; a stage without lights is
    black."""
    import numpy as np

    size = (160, 90)
    mesh = render("cube_mesh", lit_cube(1, False), size).mean(axis=2)
    lit = mesh > 0.05
    assert lit.sum() > 500, "the cube is in the picture and lit"
    for name, meters in (("cube_implicit", 1), ("cube_centimetres", 0.01)):
        pixels = render(name, lit_cube(meters, True), size).mean(axis=2)
        ratio = float(np.median(pixels[lit] / mesh[lit]))
        assert abs(ratio - 1) < 0.03, f"{name}: {ratio:.3f} of the mesh cube"
    dark = render("cube_dark", lit_cube(1, False, lit=False), size)
    assert dark.mean() < 0.01, "a stage without lights is black"


def test_catmull_rom_curve(render):
    """A non-periodic Catmull-Rom curve uses its end points as tangents: 4 points are one segment, x = -1 to 1, as
    Karma renders it."""
    import numpy as np

    size = (160, 90)
    pixels = render("curves", HEADER % 1 + CURVES, size).mean(axis=2)
    columns = np.where((pixels < 0.9).any(axis=0))[0]
    assert len(columns), "the curve renders"

    def column(x: float) -> float:  # 12 above, focal length 35, aperture 36, 160 pixels wide
        return 80 + x * 80 * 35 / (12 * 18)

    assert abs(columns.min() - column(-1)) <= 4 and abs(columns.max() - column(1)) <= 4, (columns.min(), columns.max())


def preview_shading_stage(mode: str, display_color: str = "", depth: bool = False,
                          color: str = "color3f cycles:integrator:preview_color = (0.2, 0.4, 0.6)") -> str:
    """The grey plane under the dome, seen straight down, with preview shading ``mode`` and ``color`` in its render
    settings (``depth``: and a depth RenderVar)."""
    stage = plane(PREVIEW % {"diffuse": "color3f inputs:diffuseColor = (0.5, 0.5, 0.5)", "color_space": ""})
    stage = stage.replace("    metersPerUnit = 1\n", '    metersPerUnit = 1\n    renderSettingsPrimPath = "/Render/settings"\n', 1)
    if display_color:
        stage = stage.replace("        rel material:binding = </world/mat>",
                              f"        color3f[] primvars:displayColor = [{display_color}]\n"
                              "        rel material:binding = </world/mat>", 1)
    products = "        rel products = </Render/product>\n" if depth else ""
    product = ("""    def RenderProduct "product"
    {
        rel orderedVars = </Render/product/depth>
        def RenderVar "depth"
        {
            uniform string sourceName = "depth"
        }
    }
""" if depth else "")
    return stage + f"""
def Scope "Render"
{{
    def RenderSettings "settings"
    {{
        rel camera = </world/cam>
        custom token cycles:integrator:preview_shading = "{mode}"
        custom {color}
{products}    }}
{product}}}
"""


def test_preview_shading(render):
    """Preview shading (ADR 0081 in USD Nodes): a camera ray's first hit is its colour under a headlight, whatever the
    material and the lights: the setting's colour, or the object's displayColor (0.8 grey without one). The plane
    faces the camera, so the headlight's |N . V| is about 1."""
    def colour(pixels):
        n = pixels.shape[0]
        return pixels[n // 3: 2 * n // 3, n // 3: 2 * n // 3].reshape(-1, 3).mean(axis=0)

    one = colour(render("preview_color", preview_shading_stage("color")))
    assert max(abs(a - b) for a, b in zip(one, (0.2, 0.4, 0.6))) < 0.02, one
    # As Blender's Hydra API passes a colour: a string (it has no vector values).
    text = colour(render("preview_color_string", preview_shading_stage(
        "color", color='string cycles:integrator:preview_color = "0.2 0.4 0.6"')))
    assert max(abs(a - b) for a, b in zip(text, (0.2, 0.4, 0.6))) < 0.02, text
    red = colour(render("preview_object", preview_shading_stage("object", "(1, 0, 0)")))
    assert abs(red[0] - 1.0) < 0.02 and red[1] < 0.01 and red[2] < 0.01, red
    grey = colour(render("preview_object_grey", preview_shading_stage("object")))
    assert max(abs(v - 0.8) for v in grey) < 0.02, grey
    normal = middle(render("preview_none", preview_shading_stage("none")))
    assert abs(normal - 0.5) < 0.03, normal  # the material under the dome, as test_dome_radiance
    # The data passes as in a normal render: viewports (Blender's Live) composite the image by its depth.
    shaded = middle(render("depth_none", preview_shading_stage("none", depth=True), channel="Depth.Z"))
    preview = middle(render("depth_preview", preview_shading_stage("color", depth=True), channel="Depth.Z"))
    assert shaded > 0.0 and abs(preview - shaded) < 1e-3 * shaded, (preview, shaded)


def id_stage(aovs=()) -> str:
    """Three instances of a box in a row (a point instancer) and a small box above them on its own, seen from the
    front; the instances glow with their instanceId (white 0, 1, 2), the box is plain (a material: the
    default surface is cycles_usd_render's own). ``aovs``: RenderVars."""
    proto = (box("proto", 0, 0, 0, 1, 1, 1)
             .replace('def Mesh "proto"', 'def Mesh "proto" (prepend apiSchemas = ["MaterialBindingAPI"])')
             .replace('uniform token subdivisionScheme = "none"',
                      'uniform token subdivisionScheme = "none"\n        rel material:binding = </world/glow>'))
    single = (box("single", 0, 0, 2.5, 0.4, 0.4, 0.4)
              .replace('def Mesh "single"', 'def Mesh "single" (prepend apiSchemas = ["MaterialBindingAPI"])')
              .replace('uniform token subdivisionScheme = "none"',
                       'uniform token subdivisionScheme = "none"\n        rel material:binding = </world/plain>'))
    stage = HEADER % 1 + 'def Xform "world"\n{\n' + camera(35, (0, -12, 0), (0, 0, 0)) + f"""    def PointInstancer "row"
    {{
        point3f[] positions = [(-3, 0, 0), (0, 0, 0), (3, 0, 0)]
        int[] protoIndices = [0, 0, 0]
        rel prototypes = </world/row/protos/proto>
        def Scope "protos"
        {{
{proto}        }}
    }}
{single}    def Material "plain"
    {{
        token outputs:surface.connect = </world/plain/surface.outputs:surface>
        def Shader "surface"
        {{
            uniform token info:id = "UsdPreviewSurface"
            token outputs:surface
        }}
    }}
    def Material "glow"
    {{
        token outputs:surface.connect = </world/glow/surface.outputs:surface>
        def Shader "surface"
        {{
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0, 0, 0)
            int inputs:useSpecularWorkflow = 1
            color3f inputs:specularColor = (0, 0, 0)
            color3f inputs:emissiveColor.connect = </world/glow/id.outputs:result>
            token outputs:surface
        }}
        def Shader "id"
        {{
            uniform token info:id = "UsdPrimvarReader_float3"
            token inputs:varname = "instanceId"
            float3 outputs:result
        }}
    }}
}}
"""
    if aovs:
        stage = stage.replace("    metersPerUnit = 1\n",
                              '    metersPerUnit = 1\n    renderSettingsPrimPath = "/Render/settings"\n', 1)
        targets = ", ".join(f"</Render/product/{a}>" for a in aovs)
        variables = "".join(f"""        def RenderVar "{a}"
        {{
            uniform token dataType = "int"
            uniform string sourceName = "{a}"
        }}
""" for a in aovs)
        stage += f"""
def Scope "Render"
{{
    def RenderSettings "settings"
    {{
        rel camera = </world/cam>
        rel products = </Render/product>
    }}
    def RenderProduct "product"
    {{
        rel orderedVars = [{targets}]
{variables}    }}
}}
"""
    return stage


def test_id_aovs(render):
    """Hydra's ID AOVs (picking): the instanceId is the instance's index in its instancer and -1 for a prim without
    instancer and the background; the primId one per prim, -1 for the background. Shaders write them only while a
    render pass binds them, and objects carry the IDs only when their shaders read them (USD Nodes plan 28 › C3); a
    shader reading the instanceId primvar gets it without the AOVs."""
    size = (96, 54)
    points = {"left": (27, 22), "middle": (27, 48), "right": (27, 73), "single": (7, 48), "background": (50, 5)}

    def ids(name, channel):
        pixels = render(name, id_stage(("primId", "instanceId")), size=size, samples=8, channel=channel)
        return {k: round(float(pixels[r, c])) for k, (r, c) in points.items()}

    instance = ids("ids_instance", "Instance ID.X")
    assert instance == {"left": 0, "middle": 1, "right": 2, "single": -1, "background": -1}, instance
    prim = ids("ids_prim", "Prim ID.X")
    assert prim["left"] == prim["middle"] == prim["right"] >= 0 and prim["single"] >= 0, prim
    assert prim["single"] != prim["left"] and prim["background"] == -1, prim

    glow = render("ids_shader", id_stage(), size=size, samples=8)
    values = [float(glow[r, c].mean()) for r, c in (points["left"], points["middle"], points["right"])]
    assert values[0] < 0.01 and abs(values[1] - 1) < 0.02 and abs(values[2] - 2) < 0.04, values


def nested_forest(arrays: bool, motion: bool, tint: bool = True, deform: bool = False, vector: bool = False,
                  sss: bool = False, emit: bool = False) -> str:
    """Two trees of a nested point instancer seen from above, the second turned and scaled, each a trunk and an
    instancer of three leaves under a sun: per-tree displayColor on the forest (the trunks' default surface), a per-leaf
    "tint" on the leaves (their diffuse colour; ``tint`` false: a constant green; not emission, which a mesh light's
    sampling would expand). ``arrays``: cycles:instance_arrays; ``motion``: both
    levels move over the shutter; ``deform``: the leaf's points move (it flutters); ``vector``: a motion pass
    (Blender's "vector"), without motion blur; ``sss``: the leaves are small boxes of a subsurface Principled BSDF
    whose normal is a Bevel node's (Cycles nodes: local rays on each placement); ``emit``: the leaves glow (mesh
    lights, an emitter per placement)."""
    def positions(still, moved):
        return (f"point3f[] positions.timeSamples = {{ 1: {still}, 2: {moved} }}" if motion
                else f"point3f[] positions = {still}")
    tile = """int[] faceVertexCounts = [4]
                        int[] faceVertexIndices = [0, 1, 2, 3]
                        point3f[] points = [(-{s}, -{s}, 0), ({s}, -{s}, 0), ({s}, {s}, 0), (-{s}, {s}, 0)]"""
    flutter = """int[] faceVertexCounts = [4]
                        int[] faceVertexIndices = [0, 1, 2, 3]
                        point3f[] points.timeSamples = {
                            1: [(-0.6, -0.6, 0), (0.6, -0.6, 0), (0.6, 0.6, 0), (-0.6, 0.6, 0)],
                            2: [(-0.6, -0.6, 0), (0.6, -0.6, 0), (0.9, 0.9, 0.3), (-0.3, 0.9, 0.3)]
                        }"""
    cube = "\n".join(line.strip() for line in box("leaf", 0, 0, 0, 0.6, 0.6, 0.3).splitlines()[2:-1])
    stage = HEADER % 1
    stage = stage.replace("    metersPerUnit = 1\n", '    metersPerUnit = 1\n    renderSettingsPrimPath = "/Render/settings"\n'
                          "    timeCodesPerSecond = 24\n    startTimeCode = 1\n    endTimeCode = 2\n", 1)
    return stage + f"""def Xform "world"
{{
    def PointInstancer "forest"
    {{
        rel prototypes = [</world/forest/protos/tree>]
        int[] protoIndices = [0, 0]
        {positions("[(-3, 0, 0), (3, 0, 0)]", "[(-2, 0.5, 0), (4, 0.5, 0)]")}
        quath[] orientations = [(1, 0, 0, 0), (0.7071068, 0, 0, 0.7071068)]
        float3[] scales = [(1, 1, 1), (1.3, 1.3, 1.3)]
        color3f[] primvars:displayColor = [(1, 0.2, 0.1), (0.1, 0.3, 1)] (interpolation = "vertex")
        def Scope "protos"
        {{
            def Xform "tree"
            {{
                def Mesh "trunk"
                {{
                        {tile.format(s=0.4)}
                }}
                def PointInstancer "leaves"
                {{
                    rel prototypes = [</world/forest/protos/tree/leaves/protos/leaf>]
                    int[] protoIndices = [0, 0, 0]
                    {positions("[(-1.2, 1.8, 0), (1.2, 1.8, 0), (0, -1.8, 0)]",
                               "[(-1.2, 2.3, 0), (1.2, 2.3, 0), (0, -1.3, 0)]")}
                    color3f[] primvars:tint = [(1, 0, 0), (0, 1, 0), (0, 0, 1)] (interpolation = "vertex")
                    def Scope "protos"
                    {{
                        def Mesh "leaf" (prepend apiSchemas = ["MaterialBindingAPI"])
                        {{
                        {cube if sss else (flutter if deform else tile.format(s=0.6))}
                        rel material:binding = </world/{"skin" if sss else "glow"}>
                        }}
                    }}
                }}
            }}
        }}
    }}
    def Material "skin"
    {{
        token outputs:cycles:surface.connect = </world/skin/bsdf.outputs:BSDF>
        def Shader "bsdf"
        {{
            uniform token info:id = "cycles_principled_bsdf"
            color3f inputs:base_color = (0.8, 0.4, 0.2)
            float inputs:subsurface_weight = 1
            float inputs:subsurface_scale = 0.5
            normal3f inputs:normal.connect = </world/skin/bevel.outputs:normal>
            token outputs:BSDF
        }}
        def Shader "bevel"
        {{
            uniform token info:id = "cycles_bevel"
            float inputs:radius = 0.15
            int inputs:samples = 4
            normal3f outputs:normal
        }}
    }}
    def Material "glow"
    {{
        token outputs:surface.connect = </world/glow/surface.outputs:surface>
        def Shader "surface"
        {{
            uniform token info:id = "UsdPreviewSurface"
            {"color3f inputs:diffuseColor.connect = </world/glow/tint.outputs:result>" if tint
             else "color3f inputs:diffuseColor = (0.1, 0.9, 0.2)"}
            {"color3f inputs:emissiveColor = (0.5, 2, 0.5)" if emit else ""}
            token outputs:surface
        }}
        def Shader "tint"
        {{
            uniform token info:id = "UsdPrimvarReader_float3"
            token inputs:varname = "tint"
            float3 outputs:result
        }}
    }}
    def DistantLight "sun"
    {{
        float inputs:intensity = 3
        bool inputs:normalize = 1
    }}
    def Camera "cam"
    {{
        float focalLength = 35
        float horizontalAperture = 36
        float verticalAperture = 36
        double shutter:open = 0
        double shutter:close = {1 if motion and not vector else 0}
        double3 xformOp:translate = (0, 0, 14)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
}}

def Scope "Render"
{{
    def RenderSettings "settings"
    {{
        rel camera = </world/cam>
        custom bool cycles:instance_arrays = {1 if arrays else 0}
{"        rel products = </Render/product>" if vector else ""}
    }}
{'''    def RenderProduct "product"
    {
        rel orderedVars = [</Render/product/color>, </Render/product/vector>]
        def RenderVar "color"
        {
            uniform string sourceName = "color"
        }
        def RenderVar "vector"
        {
            uniform string sourceName = "vector"
        }
    }''' if vector else ""}
}}
"""


def test_instance_arrays(render):
    """Instance arrays (USD Nodes ADR 0082: instanced prims placed by Cycles) render nested instancing as the
    per-instance objects do: transforms through both levels, the inner and outer primvars, motion of both levels. On
    the CPU (Embree, E2) and CUDA (Cycles' own BVH, E3), chains without what the first version flattens are nested
    (one object, the kernel places it, its colour, random number, primvars and motion per placement); the others are
    expanded into objects (E1). Transforms are composed in floats instead of doubles, so a few edge pixels may differ a little."""
    cases = [("still", False, True, False), ("motion", True, True, False), ("plain", False, False, False),
             ("deform", True, True, True), ("vector", True, True, False), ("sss", False, False, False),
             ("emit", False, False, False)]
    for name, motion, tint, deform in cases:
        vector, sss, emit = name == "vector", name == "sss", name == "emit"
        channel = "Vector.Z" if vector else ""  # the movement to the next frame (the first frame has no previous)
        on = render(f"arrays_{name}", nested_forest(True, motion, tint, deform, vector, sss, emit), size=(96, 96),
                    samples=16, channel=channel)
        placed = render.output
        off = render(f"objects_{name}", nested_forest(False, motion, tint, deform, vector, sss, emit),
                     size=(96, 96), samples=16, channel=channel)
        if vector:
            on = on[..., None]
            off = off[..., None]
        assert "CYCLES_USD_RENDER_INSTANCES" not in render.output, name
        # 2 trunks and 6 leaves, nested: the leaves' per-leaf tint a per-placement attribute, both levels' motion
        # interpolated at the ray's time, the fluttering leaf's motion its geometry's.
        expected = "placed 2 nested 2 (8 instances)"
        assert f"CYCLES_USD_RENDER_INSTANCES {expected}" in placed, (name, placed[-500:])
        difference = abs(on - off).max(axis=2)
        if name == "vector":
            # the motion pass: each pixel's movement to the previous and next frame, as the expanded objects' (pixels)
            assert abs(on).max() > 1.0, (name, abs(on).max())
            assert abs(on - off).mean() < 1e-3 and (difference > 0.05).mean() < 0.005, \
                (name, abs(on - off).mean(), (difference > 0.05).mean())
            continue
        # the leaves' red, green and blue tints, the trunks' colours under the sun, a black background
        assert (not tint or min(on[..., c].max() for c in range(3)) > 0.5) and 0.005 < on.mean() < 0.5, (name, on.mean())
        if sss or emit:  # random walks, bevel samples, mesh lights: noise of 16 samples, the same on average
            assert abs(on.mean() - off.mean()) < 0.02 * off.mean() and difference.mean() < 0.02, \
                (name, on.mean(), off.mean(), difference.mean())
            continue
        assert difference.mean() < 1e-3 and (difference > 0.02).mean() < 0.005, \
            (name, difference.mean(), (difference > 0.02).mean())


def write_cloud(path: Path) -> None:
    """A fog ball of radius 1 (5 cm voxels) whose density varies from voxel to voxel: a heterogeneous VDB volume."""
    import numpy as np
    import openvdb

    n = 20
    ijk = np.indices((2 * n, 2 * n, 2 * n)).transpose(1, 2, 3, 0) - n + 0.5
    inside = (ijk ** 2).sum(axis=3) < n * n
    noise = np.random.default_rng(7).uniform(0.5, 1.0, inside.shape)
    grid = openvdb.FloatGrid()
    grid.copyFromArray(np.where(inside, 2.0 * noise, 0.0).astype(np.float32), ijk=(-n, -n, -n))
    grid.name = "density"
    grid.transform = openvdb.createLinearTransform(voxelSize=1.0 / n)
    openvdb.write(str(path), grids=[grid])


def nested_clouds(arrays: bool, inside: bool = False) -> str:
    """Two clusters of a nested point instancer seen from above, the second turned and scaled, each three VDB clouds
    (two of them overlapping) over a white ground under a sun and a dome: the clouds' shadows on the ground. ``inside``:
    the camera is in the first cluster's third cloud (its volume stack starts there). The clouds are at slightly
    different heights: translated copies at one height have coplanar boundary faces, whose hits at one distance
    Cycles' volume stack misses (the second is skipped), as for expanded instances."""
    camera = "(-2.5, -1, 0.007)" if inside else "(0, 0, 14)"
    stage = HEADER % 1
    stage = stage.replace("    metersPerUnit = 1\n", '    metersPerUnit = 1\n    renderSettingsPrimPath = "/Render/settings"\n', 1)
    return stage + f"""def Xform "world"
{{
    def PointInstancer "field"
    {{
        rel prototypes = [</world/field/protos/cluster>]
        int[] protoIndices = [0, 0]
        point3f[] positions = [(-2.5, 0, 0), (2.5, 0, 0)]
        quath[] orientations = [(1, 0, 0, 0), (0.7071068, 0, 0, 0.7071068)]
        float3[] scales = [(1, 1, 1), (1.3, 1.3, 1.3)]
        def Scope "protos"
        {{
            def Xform "cluster"
            {{
                def PointInstancer "clouds"
                {{
                    rel prototypes = [</world/field/protos/cluster/clouds/protos/cloud>]
                    int[] protoIndices = [0, 0, 0]
                    point3f[] positions = [(-0.6, 0.8, 0.013), (0.6, 0.8, -0.021), (0, -1, 0.007)]
                    def Scope "protos"
                    {{
                        def Volume "cloud"
                        {{
                            rel field:density = </world/field/protos/cluster/clouds/protos/cloud/density>
                            def OpenVDBAsset "density"
                            {{
                                asset filePath = @./cloud.vdb@
                                token fieldName = "density"
                            }}
                        }}
                    }}
                }}
            }}
        }}
    }}
    def Mesh "ground"
    {{
        point3f[] points = [(-30, -30, -2), (30, -30, -2), (30, 30, -2), (-30, 30, -2)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        color3f[] primvars:displayColor = [(0.8, 0.8, 0.8)]
    }}
    def DistantLight "sun"
    {{
        float inputs:intensity = 3
        bool inputs:normalize = 1
        float3 xformOp:rotateXYZ = (30, 20, 0)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
    }}
    def DomeLight "dome"
    {{
        float inputs:intensity = 0.3
    }}
    def Camera "cam"
    {{
        float focalLength = 35
        float horizontalAperture = 36
        float verticalAperture = 36
        double3 xformOp:translate = {camera}
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
}}

def Scope "Render"
{{
    def RenderSettings "settings"
    {{
        rel camera = </world/cam>
        custom bool cycles:instance_arrays = {1 if arrays else 0}
        custom bool cycles:integrator:use_adaptive_sampling = 0
    }}
}}
"""


def test_nested_volumes(render):
    """Volumes of nested placements (USD Nodes ADR 0082) render as the per-instance objects do: one VDB cloud object
    placed six times, each placement a volume of its own in the volume stack (two overlap), entered through its
    transform into the object's octree; shadows through them; a camera inside one (the volume stack from the
    camera)."""
    write_cloud(render.folder / "cloud.vdb")
    for name, inside in (("above", False), ("inside", True)):
        on = render(f"arrays_clouds_{name}", nested_clouds(True, inside), size=(96, 96), samples=64)
        placed = render.output
        off = render(f"objects_clouds_{name}", nested_clouds(False, inside), size=(96, 96), samples=64)
        expected = "placed 1 nested 1 (6 instances)"
        assert f"CYCLES_USD_RENDER_INSTANCES {expected}" in placed, (name, placed[-500:])
        assert 0.02 < on.mean() < 2.0, (name, on.mean())
        # random walks through the clouds: noise of 64 samples (no adaptive sampling), the same on average and in
        # each 12 x 12 block (a missed placement in the overlaps or in their shadows moved blocks by 0.05)
        blocks = abs((on - off).mean(axis=2).reshape(8, 12, 8, 12).mean(axis=(1, 3)))  # a bias, not the noise
        assert abs(on.mean() - off.mean()) < 0.005 * off.mean() and blocks.max() < 0.01, \
            (name, on.mean(), off.mean(), blocks.max())

TESTS = [test_dome_radiance, test_texture_colour_spaces, test_materialx, test_materialx_shader_named_as_material, test_materialx_bump, test_materialx_values, test_materialx_matrices, test_materialx_edfs_and_artistic_ior,
         test_materialx_layering_conserves_energy, test_materialx_sheen_is_conty_kulla,
         test_materialx_anisotropy_follows_uv, test_osl_shading_system, test_shader_registry, test_light_and_shadow_links,
         test_implicit_cube_and_stage_units, test_catmull_rom_curve, test_preview_shading,
         test_id_aovs, test_instance_arrays, test_nested_volumes]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--renderer", required=True, help="cycles_usd_render")
    parser.add_argument("--device", default="CPU")
    parser.add_argument("--keep", default="", help="a folder for the stages and images (default: a temporary one)")
    parser.add_argument("--lib", default="", help="the precompiled libraries (OpenImageIO's Python module)")
    parser.add_argument("tests", nargs="*")
    args = parser.parse_args()
    platform = {"linux": "linux_x64", "darwin": "macos_arm64", "win32": "windows_x64"}.get(sys.platform, "")
    lib = Path(args.lib) if args.lib else REPO / "lib" / platform
    if sys.platform == "linux" and os.environ.get("HYDRA_TESTS_LIBS") != str(lib):
        # OpenImageIO's module links the precompiled libraries: once more with them on the library path.
        folders = [str(p) for p in sorted(lib.glob("*/lib"))]
        environment = dict(os.environ, HYDRA_TESTS_LIBS=str(lib),
                           LD_LIBRARY_PATH=os.pathsep.join(folders + [os.environ.get("LD_LIBRARY_PATH", "")]))
        os.execve(sys.executable, [sys.executable, *sys.argv], environment)
    with tempfile.TemporaryDirectory() as temporary:
        folder = Path(args.keep or temporary)
        folder.mkdir(parents=True, exist_ok=True)
        render = Renderer(os.path.abspath(args.renderer), args.device, folder, lib)
        failed = []
        for test in TESTS:
            if args.tests and test.__name__ not in args.tests and test.__name__[5:] not in args.tests:
                continue
            try:
                test(render)
                print(f"PASS {test.__name__}", flush=True)
            except AssertionError as error:
                failed.append(test.__name__)
                print(f"FAIL {test.__name__}: {error}", flush=True)
    print(f"{'HYDRA_TESTS OK' if not failed else 'HYDRA_TESTS FAILED: ' + ', '.join(failed)} ({args.device})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
