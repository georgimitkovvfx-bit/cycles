# SPDX-FileCopyrightText: 2026 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

"""Coverage map of MaterialX's standard library in the Hydra delegate (plan F1 of our fork).

Every node definition of the libraries the delegate loads (stdlib, pbrlib, bxdf, cmlib, nprlib) gets a material of its
own in one USD stage: the node with its default inputs, its output fed to a Standard Surface's base colour (through a
convert node for other types), to a `surface` node (BSDF, EDF), or bound directly (surface, displacement and volume
shaders). cycles_usd_render renders the stage once; the translator's warnings for each material say which nodes it
does not support. Each definition is then:

* **mapped**: one of the surface shaders the translator maps to the Principled BSDF itself;
* **translated**: a Cycles node (or a few) of its own in `hydra/materialx.cpp`;
* **expanded**: MaterialX defines it as a node graph, which the translator expands, and every node in it is supported;
* **approximated**: the translator warns that its result is close, not exact (as hex tiled images, plain images);
* **unsupported**: the translator warns (the nodes it warned about are listed); its output is zero;
* **not probed**: its output is a string, a light shader or a material, which no surface consumes directly.

With --values, every value output is also rendered unlit on a quad of a grid and compared with MaterialX's own OSL
(MaterialXGenOsl's code of the node, compiled by oslc, in a Cycles script node: Cycles' OSL) and, with --karma, with
Karma's (husk): ours agreeing with either reference is right, the references agreeing without ours is our bug.
--examples translates MaterialX's example materials (Standard Surface, OpenPBR, glTF...), referenced into USD.

    LD_LIBRARY_PATH=$(ls -d $PWD/lib/linux_x64/*/lib | tr '\\n' :) lib/linux_x64/python/bin/python3.13 \\
        src/hydra/test/materialx_coverage.py [--values [--karma]] [--examples <folder>] --output src/hydra/MATERIALX.md
"""

import argparse
import collections
import glob
import os
import re
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

import MaterialX as mx

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
HFS = os.environ.get("HOUDINI_ROOT") or (sorted(glob.glob("/opt/hfs[0-9]*")) or [""])[-1]

SDF_TYPES = {"float": "float", "integer": "int", "boolean": "bool", "color3": "color3f", "color4": "color4f",
             "vector2": "float2", "vector3": "vector3f", "vector4": "float4", "matrix33": "matrix3d",
             "matrix44": "matrix4d", "string": "string", "filename": "asset"}
CLOSURES = ("BSDF", "EDF", "VDF", "surfaceshader", "displacementshader", "volumeshader")
NOT_PROBED = ("string", "filename", "lightshader", "material")
# A closure of each kind for closure inputs: layering or passing nothing builds no shader at all. A volume shader is
# a volume of an absorption VDF (fed in turn).
FEEDERS = {"BSDF": "ND_oren_nayar_diffuse_bsdf", "EDF": "ND_uniform_edf", "VDF": "ND_absorption_vdf",
           "surfaceshader": "ND_surface_unlit", "volumeshader": "ND_volume"}
MAPPED = ("standard_surface", "open_pbr_surface", "UsdPreviewSurface", "gltf_pbr")
# Warnings of a result that is close, not zero.
APPROXIMATIONS = ("hex tiled image is rendered as a plain image", "hex tiled normal map is rendered as a plain normal map",
                  "measured_edf is rendered as uniform emission")
OSL = REPO / "lib" / "linux_x64" / "osl"
OSLC = OSL / "bin" / "oslc"
OSL_INCLUDE = OSL / "share" / "OSL" / "shaders"
LIBRARIES = ("stdlib", "pbrlib", "bxdf", "cmlib", "nprlib")

STATUSES = ("mapped", "translated", "expanded", "approximated", "unsupported")
WARNED = ("approximated", "unsupported")

PREFACE = """Regenerate after a change to `hydra/materialx.cpp` (it takes minutes; `--karma` needs Houdini, `--examples`
MaterialX's `resources/Materials/Examples` from its source):

    LD_LIBRARY_PATH=$(ls -d $PWD/lib/linux_x64/*/lib | tr '\\n' :) lib/linux_x64/python/bin/python3.13 \\
        src/hydra/test/materialx_coverage.py --values --karma --examples <MaterialX>/resources/Materials/Examples \\
        --output src/hydra/MATERIALX.md

Known: the noise nodes (`noise2d`, `noise3d`, `fractal*`, `cellnoise3d`, `worleynoise*`, `random*`) are Cycles' noise,
not MaterialX's (upstream's MaterialX-exact noise, Blender PR #158054, is awaited); `heighttonormal` alone is Cycles'
Bump node's world space normal (it completes the `normalmap` it feeds); a missing primvar gives 0, not the
`geompropvalue` / `UsdPrimvarReader` default (Cycles' Attribute node cannot tell); `chiang_hair_roughness` outputs
Cycles' hair roughnesses (its consumer, `chiang_hair_bsdf`, takes those); a node defined in the material's own `.mtlx`
file (MaterialX's simple hair) needs its folder in `PXR_MTLX_PLUGIN_SEARCH_PATHS`, as in Storm. `conical_edf` follows
the specification's description (MaterialX's OSL and GLSL have no implementation, its MDL marks its own as TODO);
`divide` and `invertmatrix` of a matrix that varies over the surface are unsupported."""

HEADER = """#usda 1.0
(
    defaultPrim = "world"
    upAxis = "Z"
)
def Xform "world"
{
    def Camera "cam"
    {
        double3 xformOp:translate = (0, 0, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def DomeLight "sky"
    {
    }
"""

QUAD = """    def Mesh "q_%(name)s" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0,1,2,3]
        texCoord2f[] primvars:st = [(0,0),(1,0),(1,1),(0,1)] (interpolation = "vertex")
        rel material:binding = </world/%(name)s>
    }
"""


def shader(path: str, node_id: str, inputs: list[str], output: str, output_type: str) -> str:
    body = "".join(f"            {line}\n" for line in inputs)
    return (f'        def Shader "{path}"\n        {{\n            uniform token info:id = "{node_id}"\n{body}'
            f"            {output_type} outputs:{output}\n        }}\n")


def surface(base_color: str) -> str:
    return shader("pbr", "ND_standard_surface_surfaceshader",
                  [f"color3f inputs:base_color.connect = <{base_color}>", "float inputs:specular = 0"], "out", "token")


def material(name: str, nodedef: mx.NodeDef, output: mx.Output) -> str | None:
    """The material probing one output of a node definition, or None when no surface can consume it."""
    kind = output.getType()
    if kind in NOT_PROBED:
        return None
    root = f"/world/{name}"
    out = f"{root}/n.outputs:{output.getName()}"
    # Closure inputs get a closure of their kind: layering or mixing nothing builds no shader at all.
    feeds, node = [], ""
    for i, closure_input in enumerate(nodedef.getActiveInputs()):
        feeder = FEEDERS.get(closure_input.getType())
        if feeder:
            inner = []
            if feeder == "ND_volume":
                node += shader(f"v{i}", FEEDERS["VDF"], [], "out", "token")
                inner = [f"token inputs:vdf.connect = <{root}/v{i}.outputs:out>"]
            node += shader(f"f{i}", feeder, inner, "out", "token")
            feeds.append(f"token inputs:{closure_input.getName()}.connect = <{root}/f{i}.outputs:out>")
    node += shader("n", nodedef.getName(), feeds, output.getName(), SDF_TYPES.get(kind, "token"))
    terminals = {"surface": f"{root}/pbr.outputs:out"}
    if kind == "color3":
        node += surface(out)
    elif kind in ("matrix33", "matrix44"):
        node += shader("m", "ND_transformmatrix_vector3" + ("M4" if kind == "matrix44" else ""), [f"{SDF_TYPES[kind]} inputs:mat.connect = <{out}>"],
                       "out", "vector3f")
        node += shader("c", "ND_convert_vector3_color3", [f"vector3f inputs:in.connect = <{root}/m.outputs:out>"], "out",
                       "color3f")
        node += surface(f"{root}/c.outputs:out")
    elif kind in SDF_TYPES:
        node += shader("c", f"ND_convert_{kind}_color3", [f"{SDF_TYPES[kind]} inputs:in.connect = <{out}>"], "out",
                       "color3f")
        node += surface(f"{root}/c.outputs:out")
    elif kind in ("BSDF", "EDF"):
        node += shader("s", "ND_surface", [f"token inputs:{kind.lower()}.connect = <{out}>"], "out", "token")
        terminals = {"surface": f"{root}/s.outputs:out"}
    elif kind == "VDF":
        node += shader("v", "ND_volume", [f"token inputs:vdf.connect = <{out}>"], "out", "token")
        terminals = {"volume": f"{root}/v.outputs:out"}
    elif kind == "surfaceshader":
        terminals = {"surface": out}
    elif kind == "displacementshader":
        node += surface(f"{root}/n.outputs:none")
        terminals["displacement"] = out
    elif kind == "volumeshader":
        terminals = {"volume": out}
    else:
        return None
    links = "".join(f"        token outputs:mtlx:{key}.connect = <{value}>\n" for key, value in terminals.items())
    return f'    def Material "{name}"\n    {{\n{links}{node}    }}\n' + QUAD % {"name": name}


def render(renderer: str, folder: Path, name: str, usda: str) -> dict[str, set[str]] | None:
    """The translator's warnings for each material directly under /world/ of the stage, rendered once."""
    stage = folder / f"{name}.usda"
    stage.write_text(usda)
    result = subprocess.run([renderer, str(stage), str(folder / f"{name}.####.exr"), "--frames", "1",
                             "--resolution", "8x8", "--samples", "1", "--camera", "/world/cam"],
                            capture_output=True, text=True, timeout=1800, check=False)
    log = result.stdout + result.stderr
    (folder / f"{name}.log").write_text(log)
    if not (folder / f"{name}.0001.exr").is_file():
        print(log[-3000:], file=sys.stderr)
        return None
    warnings = collections.defaultdict(set)
    for match in re.finditer(r"MaterialX material /world/(\w+)\S*: (.*)", log):
        warnings[match.group(1)].add(match.group(2).strip())
    for match in re.finditer(r"Could not build a shader graph for /world/(\w+)", log):
        warnings[match.group(1)].add("no shader graph built")
    return warnings


def corpus(renderer: str, folder: Path, root: Path) -> list[str] | None:
    """Markdown lines on the materials of every .mtlx file under root, each referenced into USD (usdMtlx)."""
    quads, files = [], {}
    for i, path in enumerate(sorted(root.rglob("*.mtlx"))):
        doc = mx.createDocument()
        mx.readFromXmlFile(doc, str(path))
        name = f"f{i}"
        files[name] = path
        quads.append(f'    def Scope "{name}" (references = @{path}@</MaterialX>)\n    {{\n    }}\n')
        for j, node in enumerate(doc.getMaterialNodes()):
            quads.append(QUAD.replace("q_%(name)s", f"q_{name}_{j}").replace(
                "/world/%(name)s", f"/world/{name}/Materials/{node.getName()}"))
    warnings = render(renderer, folder, "corpus", HEADER + "".join(quads) + "}\n")
    if warnings is None:
        return None
    nodes = collections.Counter()
    lines = ["", "## Example materials", "",
             (f"The {len(files)} files of MaterialX's `resources/Materials/Examples`, each referenced into USD: the "
              "warnings of their materials."), "", "| File | Warnings |", "|---|---|"]
    for name, path in files.items():
        found = sorted(warnings.get(name, ()))
        for warning in found:
            match = re.match(r"(?:unsupported MaterialX node|no MaterialX node definition for node \S+) \(?(\w+)", warning)
            if match:
                nodes[match.group(1)] += 1
        lines.append(f"| {path.relative_to(root)} | {'; '.join(found) if found else 'none'} |")
    lines[4:4] = [f"Nodes they use that are not supported: {', '.join(f'`{n}` ({c})' for n, c in nodes.most_common())}"
                  if nodes else "Every node they use is supported.", ""]
    return lines


# Pixels per probe in the value stage. The 2 x 2 pixels around (3/8, 3/8) of the cell are read: off the middle, where
# split and ramp nodes switch, and away from the edges, which the pixel filter blurs.
CELL = 16
SAMPLE = 3 * CELL // 8
# The image of every file input: a constant linear colour, so that filtering and colour spaces do not matter.
TEXTURE = (0.2, 0.5, 0.8, 0.6)
VALUE_TYPES = ("float", "color3", "color4", "vector2", "vector3", "vector4", "integer", "boolean", "matrix33",
               "matrix44")
# A matrix output is shown as this vector transformed by it (transformmatrix), then as a colour.
MATRIX_PROBE = {"matrix33": ("ND_transformmatrix_vector3", "vector3", "0.3, 0.5, 0.7"),
                "matrix44": ("ND_transformmatrix_vector4", "vector4", "0.3, 0.5, 0.7, 0.9")}


def constant(kind: str, seed: int) -> str | None:
    """A fixed non-default value in [0.1, 0.9] for an input, so that math nodes do more than their defaults (matrices:
    plus the identity)."""
    values = [0.1 + 0.8 * (((seed * 7919 + k * 104729) % 1000) / 1000.0) for k in range(16)]
    count = {"float": 1, "vector2": 2, "color3": 3, "vector3": 3, "color4": 4, "vector4": 4, "matrix33": 9,
             "matrix44": 16}.get(kind)
    if count is None:
        return None
    if count in (9, 16):
        # A matrix plus the identity: far from singular, so that inverses and determinants compare well.
        n = 3 if count == 9 else 4
        values = [v + (1.0 if k % (n + 1) == 0 else 0.0) for k, v in enumerate(values[:count])]
    text = ", ".join(f"{v:.3f}" for v in values[:count])
    return text if count == 1 else f"({text})"


def probe_inputs(name: str, nodedef: mx.NodeDef, texture: Path) -> list[tuple[str, str, str]]:
    """(input, MaterialX type, value string) of a value probe: constants, the texture for file inputs, geometric
    defaults left as they are."""
    inputs = []
    for node_input in nodedef.getActiveInputs():
        kind, input_name = node_input.getType(), node_input.getName()
        if kind == "filename":
            inputs.append((input_name, kind, str(texture)))
        elif input_name == "temperature":  # blackbody, in Kelvin
            inputs.append((input_name, kind, "3500"))
        elif not node_input.hasDefaultGeomPropString() and not node_input.getIsUniform():
            value = constant(kind, zlib.crc32(f"{name}/{input_name}".encode()))
            if value is not None:
                inputs.append((input_name, kind, value.strip("()")))
    return inputs


def usd_value(kind: str, value: str) -> str:
    if kind == "filename":
        return f"@{value}@"
    if kind in ("matrix33", "matrix44"):
        n = 3 if kind == "matrix33" else 4
        values = [v.strip() for v in value.split(",")]
        return "(" + ", ".join("(" + ", ".join(values[i * n:(i + 1) * n]) + ")" for i in range(n)) + ")"
    return value if kind in ("float", "integer", "boolean") else f"({value})"


def quad(name: str, column: int, row: int) -> str:
    x0, y0 = column, -row
    return (QUAD % {"name": name}).replace(
        "[(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]", f"[({x0},{y0 - 1},0),({x0 + 1},{y0 - 1},0),({x0 + 1},{y0},0),({x0},{y0},0)]")


def value_material(name: str, nodedef: mx.NodeDef, output: mx.Output, column: int, row: int,
                   inputs: list[tuple[str, str, str]]) -> str:
    """An unlit quad of one grid cell showing one output."""
    root = f"/world/{name}"
    kind = output.getType()
    lines = [f"{SDF_TYPES[k]} inputs:{n} = {usd_value(k, v)}" for n, k, v in inputs]
    out = f"{root}/n.outputs:{output.getName()}"
    node = shader("n", nodedef.getName(), lines, output.getName(), SDF_TYPES[kind])
    if kind in MATRIX_PROBE:
        transform, kind, vector = MATRIX_PROBE[kind]
        node += shader("t", transform, [f"{SDF_TYPES[kind]} inputs:in = {usd_value(kind, vector)}",
                                        f"{SDF_TYPES[output.getType()]} inputs:mat.connect = <{out}>"], "out",
                       SDF_TYPES[kind])
        out = f"{root}/t.outputs:out"
    if kind != "color3":
        node += shader("c", f"ND_convert_{kind}_color3", [f"{SDF_TYPES[kind]} inputs:in.connect = <{out}>"], "out",
                       "color3f")
        out = f"{root}/c.outputs:out"
    node += shader("u", "ND_surface_unlit", [f"color3f inputs:emission_color.connect = <{out}>"], "out", "token")
    return (f'    def Material "{name}"\n    {{\n        token outputs:mtlx:surface.connect = <{root}/u.outputs:out>\n'
            f"{node}    }}\n{quad(name, column, row)}")


def osl_material(name: str, nodedef: mx.NodeDef, output: mx.Output, column: int, row: int,
                 inputs: list[tuple[str, str, str]], libraries: mx.Document, search: Path, folder: Path) -> str | None:
    """The same probe as MaterialX's own OSL (its genosl implementations, generated by MaterialXGenOsl and compiled
    by oslc) in a Cycles script node feeding an emission, or None when MaterialX generates or oslc compiles none."""
    # PyMaterialXGenShader first: PyMaterialXGenOsl links to it.
    import MaterialX.PyMaterialXGenShader as mx_gen  # noqa: I001
    import MaterialX.PyMaterialXGenOsl as mx_osl

    doc = mx.createDocument()
    doc.importLibrary(libraries)
    node = doc.addNodeInstance(nodedef, "n")
    for input_name, kind, value in inputs:
        node.addInput(input_name, kind).setValueString(value)
    if output.getName() != "out" or len(nodedef.getOutputs()) > 1:
        node.setType("multioutput")
    source, kind = node, output.getType()
    if kind in MATRIX_PROBE:
        transform, kind, vector = MATRIX_PROBE[kind]
        source = doc.addNodeInstance(doc.getNodeDef(transform), "t")
        source.addInput("in", kind).setValueString(vector)
        source.addInput("mat", output.getType()).setConnectedNode(node)
    if kind != "color3":
        converted = doc.addNodeInstance(doc.getNodeDef(f"ND_convert_{kind}_color3"), "c")
        converted.addInput("in", kind).setConnectedNode(source)
        if source is node and node.getType() == "multioutput":
            converted.getInput("in").setOutputString(output.getName())
        source = converted
    result = doc.addOutput("out", "color3")
    result.setNodeName(source.getName())
    if source is node and node.getType() == "multioutput":
        result.setOutputString(output.getName())
    generator = mx_osl.OslShaderGenerator.create()
    context = mx_gen.GenContext(generator)
    context.registerSourceCodeSearchPath(mx.FileSearchPath(str(search)))
    try:
        code = generator.generate("probe", result, context).getSourceCode("pixel")
    except Exception:  # noqa: BLE001  (MaterialX raises LookupError, ExceptionShaderGenError...)
        return None
    # MaterialX reads texture coordinates as OSL's u, v (barycentric in Cycles): the mesh's UV map instead.
    code = code.replace("vector2(u,v)", "mx_cycles_uv()").replace(
        "vector(u,v,0)", "vector(mx_cycles_uv().x, mx_cycles_uv().y, 0)").replace(
        "shader probe", 'vector2 mx_cycles_uv()\n{\n    point uv = 0;\n    getattribute("geom:uv", uv);\n'
        '    return vector2(uv[0], uv[1]);\n}\n\nshader probe', 1)
    osl = folder / "osl" / f"{name}.osl"
    osl.write_text(code)
    oso = osl.with_suffix(".oso")
    compiled = subprocess.run([str(OSLC), "-q", f"-I{OSL_INCLUDE}", f"-I{search / 'libraries' / 'stdlib' / 'genosl' / 'include'}", "-o", str(oso),
                               str(osl)], capture_output=True, text=True, check=False)
    if compiled.returncode != 0 or not oso.is_file():
        return None
    root = f"/world/{name}"
    return (f'    def Material "{name}"\n    {{\n        token outputs:cycles:surface.connect = <{root}/e.outputs:emission>\n'
            + shader("s", "cycles_script", [f"asset inputs:filepath = @{oso}@"], "out", "color3f")
            + shader("e", "cycles_emission", [f"color3f inputs:color.connect = <{root}/s.outputs:out>",
                                              "float inputs:strength = 1"], "emission", "token")
            + f"    }}\n{quad(name, column, row)}")


def values(renderer: str, folder: Path, probes: dict, statuses: dict, libraries: mx.Document, search: Path,
           karma: bool) -> list[str] | None:
    """Markdown lines comparing every translated and expanded value output with MaterialX's own OSL, rendered by
    Cycles' OSL, and with Karma's (husk) when asked."""
    import numpy as np

    sys.path.insert(0, str(REPO / "lib" / "linux_x64" / "openimageio" / "lib" /
                           f"python{sys.version_info.major}.{sys.version_info.minor}" / "site-packages"))
    import OpenImageIO as oiio

    names = [n for n, (nd, o) in probes.items() if o.getType() in VALUE_TYPES
             and statuses[n] in ("translated", "expanded", "approximated")]
    columns = int(len(names) ** 0.5) + 1
    rows = (len(names) + columns - 1) // columns
    camera = f"""    def Camera "grid"
    {{
        token projection = "orthographic"
        float horizontalAperture = {columns * 10}
        float verticalAperture = {rows * 10}
        float2 clippingRange = (0.1, 100)
        double3 xformOp:translate = ({columns / 2}, {-rows / 2}, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
"""
    header = HEADER.replace('    def DomeLight "sky"\n    {\n    }\n', camera)
    texture = folder / "constant.exr"
    image = oiio.ImageBuf(oiio.ImageSpec(4, 4, 4, oiio.FLOAT))
    oiio.ImageBufAlgo.fill(image, TEXTURE)
    image.write(str(texture))
    inputs = {n: probe_inputs(n, probes[n][0], texture) for n in names}
    # Every library folder, with targets/ (the genosl implementations' target).
    osl_libraries = mx.createDocument()
    mx.loadLibraries(["libraries"], mx.FileSearchPath(str(search)), osl_libraries)

    (folder / "osl").mkdir(exist_ok=True)
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor() as pool:
        osl = list(pool.map(lambda item: osl_material(item[1], *probes[item[1]], item[0] % columns, item[0] // columns,
                                                       inputs[item[1]], osl_libraries, search, folder),
                               enumerate(names)))
    stages = {
        "cycles": "".join(value_material(n, *probes[n], i % columns, i // columns, inputs[n])
                          for i, n in enumerate(names)),
        "osl": "".join(text for text in osl if text),
    }
    (folder / "values.usda").write_text(header + stages["cycles"] + "}\n")
    (folder / "values_osl.usda").write_text(
        header.replace("    upAxis", '    renderSettingsPrimPath = "/Render/settings"\n    upAxis', 1) + stages["osl"]
        + '}\ndef Scope "Render"\n{\n    def RenderSettings "settings"\n    {\n        rel camera = </world/grid>\n'
        "        custom bool cycles:shading_system = 1\n    }\n}\n")
    size = (columns * CELL, rows * CELL)
    cycles = lambda usd, key: [renderer, str(folder / usd), str(folder / f"values_{key}.####.exr"), "--frames", "1",
                               "--resolution", f"{size[0]}x{size[1]}", "--samples", "16", "--camera", "/world/grid"]
    commands = {"cycles": cycles("values.usda", "cycles"), "osl": cycles("values_osl.usda", "osl")}
    if karma:
        commands["karma"] = [str(Path(HFS) / "bin" / "husk"), "-R", "BRAY_HdKarma", "-c", "/world/grid", "--res",
                             str(size[0]), str(size[1]), "-p", "16", "-f", "1", "-o",
                             str(folder / "values_karma.0001.exr"), str(folder / "values.usda")]
    pixels = {}
    for key, command in commands.items():
        # husk brings its own libraries: the precompiled ones this script runs with would replace them.
        env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"} if key == "karma" else None
        result = subprocess.run(command, capture_output=True, text=True, timeout=1800, check=False, env=env)
        (folder / f"values_{key}.log").write_text(result.stdout + result.stderr)
        image = folder / f"values_{key}.0001.exr"
        if not image.is_file():
            print(f"{key}: no image\n{(result.stdout + result.stderr)[-3000:]}", file=sys.stderr)
            return None
        pixels[key] = np.asarray(oiio.ImageBuf(str(image)).get_pixels(oiio.FLOAT))[..., :3]

    def at(key: str, i: int):
        y, x = (i // columns) * CELL + SAMPLE, (i % columns) * CELL + SAMPLE
        return pixels[key][y - 1:y + 1, x - 1:x + 1].reshape(-1, 3).mean(axis=0)

    def close(a, b) -> bool:
        return bool(np.abs(a - b).max() <= 0.01 + 0.02 * np.abs(b).max())

    # Ours agrees with a reference: right (the references are independent implementations). Both references agree
    # and ours does not: ours is wrong. Otherwise the references disagree with each other too.
    wrong, unclear, agree = [], [], 0
    for i, name in enumerate(names):
        ours = at("cycles", i)
        references = [at("osl", i) if osl[i] else None, at("karma", i) if karma else None]
        if any(r is not None and close(ours, r) for r in references):
            agree += 1
            continue
        nodedef, output = probes[name]
        row = (nodedef.getNodeString(), nodedef.getName(), output.getName(), ours, *references)
        both = all(r is not None for r in references) and close(references[0], references[1])
        (wrong if both or (references[1] is None) else unclear).append(row)
    fmt = lambda v: "–" if v is None else "(" + ", ".join(f"{c:.3f}" for c in v) + ")"
    table = lambda rows: (["| Node | Definition | Output | Cycles | OSL | Karma |", "|---|---|---|---|---|---|"]
                          + [f"| `{n}` | {nd} | {out} | {fmt(o)} | {fmt(r)} | {fmt(k)} |"
                             for n, nd, out, o, r, k in sorted(rows)])
    lines = ["", "## Values", "",
             f"Every translated, expanded or approximated output of a value type ({len(names)}) on an unlit quad, "
             "against MaterialX's own OSL (its genosl implementations through MaterialXGenOsl and oslc, rendered by "
             "Cycles' OSL)" + (" and Karma (husk, Houdini's MaterialX)" if karma else "") + ": constant inputs in "
             f"[0.1, 0.9], file inputs a constant image {TEXTURE}, geometric defaults (texture coordinates, position, "
             "normal) as they are, read at (3/8, 3/8) of each quad. Within 0.01 + 2 %, "
             f"{agree} agree with a reference, {len(wrong)} with neither while the references agree (ours is wrong), "
             f"{len(unclear)} with neither while the references disagree too.", "",
             "### Cycles is wrong", "", *table(wrong), "", "### The references disagree", "", *table(unclear)]
    return lines


def is_expanded(nodedef: mx.NodeDef) -> bool:
    implementation = nodedef.getImplementation()
    return implementation is not None and implementation.isA(mx.NodeGraph)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--renderer", default=str(REPO / "install" / "bin" / "cycles_usd_render"))
    parser.add_argument("--libraries", default=str(REPO / "install" / "usd" / "usdMtlx" / "resources"),
                        help="folder holding libraries/ (the delegate's own, by default)")
    parser.add_argument("--output", help="markdown file to write (default: print)")
    parser.add_argument("--keep", help="folder for the stage and its log")
    parser.add_argument("--values", action="store_true", help="compare the values with MaterialX's own OSL")
    parser.add_argument("--karma", action="store_true", help="and with Karma's (needs Houdini)")
    parser.add_argument("--examples", help="MaterialX's resources/Materials/Examples folder, to translate its materials")
    args = parser.parse_args()

    doc = mx.createDocument()
    mx.loadLibraries(list(LIBRARIES), mx.FileSearchPath(str(Path(args.libraries) / "libraries")), doc)

    probes, materials = {}, []
    for nodedef in doc.getNodeDefs():
        for output in nodedef.getOutputs():
            name = f"{nodedef.getName()}__{output.getName()}"
            text = material(name, nodedef, output)
            if text is not None:
                probes[name] = (nodedef, output)
                materials.append(text)

    folder = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="mtlx_coverage_"))
    folder.mkdir(parents=True, exist_ok=True)
    warnings = render(args.renderer, folder, "coverage", HEADER + "".join(materials) + "}\n")
    if warnings is None:
        return 1

    rows, statuses = collections.defaultdict(list), {}
    for name, (nodedef, output) in probes.items():
        label = nodedef.getName() + ("" if len(nodedef.getOutputs()) == 1 else f" ({output.getName()})")
        if nodedef.getNodeString() in MAPPED:
            status = "mapped"
        elif any(not w.startswith(APPROXIMATIONS) for w in warnings.get(name, ())):
            status = "unsupported"
        elif name in warnings:
            status = "approximated"
        else:
            status = "expanded" if is_expanded(nodedef) else "translated"
        library = nodedef.getSourceUri().split("libraries/")[-1].split("/")[0]
        statuses[name] = status
        rows[status].append((library, nodedef.getNodeGroup(), nodedef.getNodeString(), label,
                             "; ".join(sorted(warnings.get(name, ())))))
    skipped = sorted(nd.getName() for nd in doc.getNodeDefs()
                     if all(o.getType() in NOT_PROBED for o in nd.getOutputs()))

    lines = ["# MaterialX coverage of the Hydra delegate", "", *PREFACE.splitlines(), "",
             (f"Generated by `src/hydra/test/materialx_coverage.py` (MaterialX {mx.getVersionString()}, the libraries "
              f"{', '.join(LIBRARIES)}): what the translator in `hydra/materialx.cpp` accepts, one probe per node "
              "definition and output, and (*Values*) whether its values are MaterialX's."), "",
             "| Status | Outputs |", "|---|---|"]
    for status in STATUSES:
        lines.append(f"| {status} | {len(rows[status])} |")
    lines.append(f"| not probed | {len(skipped)} node definitions |")
    for status in ("unsupported", "approximated", "mapped", "translated", "expanded"):
        lines += ["", f"## {status.capitalize()}", ""]
        by_node = collections.defaultdict(list)
        for library, group, node, label, warning in sorted(rows[status]):
            by_node[(library, group, node)].append((label, warning))
        lines += ["| Library | Group | Node | Definitions" + (" | Warnings |" if status in WARNED else " |"),
                  "|---|---|---|---" + ("|---|" if status in WARNED else "|")]
        for (library, group, node), defs in by_node.items():
            names = ", ".join(label for label, _ in defs)
            if status in WARNED:
                warned = "; ".join(sorted({w for _, w in defs if w}))
                lines.append(f"| {library} | {group} | `{node}` | {names} | {warned} |")
            else:
                lines.append(f"| {library} | {group} | `{node}` | {names} |")
    lines += ["", "## Not probed", "", "Outputs no surface consumes directly (strings, file names, light shaders, "
              "materials): " + ", ".join(skipped) + ".", ""]
    if args.values:
        compared = values(args.renderer, folder, probes, statuses, doc, Path(args.libraries), args.karma)
        if compared is None:
            return 1
        lines[-1:-1] = compared
    if args.examples:
        examples = corpus(args.renderer, folder, Path(args.examples))
        if examples is None:
            return 1
        lines[-1:-1] = examples
    text = "\n".join(lines)
    if args.output:
        Path(args.output).write_text(text)
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
