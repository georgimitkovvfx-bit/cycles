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

    def __call__(self, name: str, usda: str, size=(48, 48), samples=32, camera_path="/world/cam"):
        """Linear RGB pixels (rows top down) of the stage."""
        import numpy as np
        import OpenImageIO as oiio

        usd = self.folder / f"{name}.usda"
        usd.write_text(usda)
        command = [self.path, str(usd), str(self.folder / f"{name}.####.exr"), "--frames", "1",
                   "--resolution", f"{size[0]}x{size[1]}", "--samples", str(samples), "--camera", camera_path,
                   "--device", self.device]
        result = subprocess.run(command, capture_output=True, text=True, timeout=600, check=False)
        image = self.folder / f"{name}.0001.exr"
        if not image.is_file():
            raise AssertionError(f"{name}: no image\n{result.stdout[-2000:]}\n{result.stderr[-2000:]}")
        pixels = oiio.ImageBuf(str(image)).get_pixels(oiio.FLOAT)
        return np.asarray(pixels)[..., :3]


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


def test_materialx_bump(render):
    """MaterialX's bump (heighttonormal into normalmap) of a constant height leaves a flat plane as it is: 0.5 under
    the dome, as without it. Cycles' Bump node's world space normal decoded again as a tangent space color tilted the
    normal: bumped surfaces rendered nearly black."""
    value = middle(render("mtlx_bump", plane(MATERIALX % {
        "base_color": "color3f inputs:base_color = (0.5, 0.5, 0.5)",
        "normal": "vector3f inputs:geometry_normal.connect = </world/mat/bump.outputs:out>"})))
    assert abs(value - 0.5) < 0.03, value


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


TESTS = [test_dome_radiance, test_texture_colour_spaces, test_materialx, test_materialx_bump, test_light_and_shadow_links,
         test_implicit_cube_and_stage_units, test_catmull_rom_curve]


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
