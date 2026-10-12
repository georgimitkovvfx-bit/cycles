# SPDX-FileCopyrightText: 2026 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

"""PythonModule of the Cycles Script VOP (like Blender's Script node).

"Load Shader" reads the OSL shader in "filepath" (.osl is compiled with the oslc installed with
Cycles, .oso is read as is) and turns the node into a VOP type with the shader parameters as
inputs and outputs. That type is generated per shader file and embedded in the hip file. Every
type exports as the "cycles_script" shader with the file in "filepath", the delegate loads it.
"""

import os
import re
import subprocess
import tempfile

import hou

SHADER_NAME = "cycles_script"
BASE_TYPE = "cycles_script"
TYPE_PREFIX = "cycles_script_"

# OSL parameter type -> VOP type, number of components. Others (matrix, arrays, structs) are
# skipped, like Cycles does.
OSL_TYPES = {
    "float": ("float", 1),
    "int": ("int", 1),
    "string": ("string", 1),
    "color": ("color", 3),
    "point": ("vector", 3),
    "vector": ("vector", 3),
    "normal": ("vector", 3),
    "closure color": ("surface", 0),
}


def _oslc():
    root = hou.text.expandString("$CYCLES_ROOT")
    return (os.path.join(root, "bin", "oslc.exe" if os.name == "nt" else "oslc"),
            os.path.join(os.path.dirname(root), "shader"))


def oso_text(path):
    """Bytecode of an .oso file, or of an .osl file compiled with oslc."""
    if not os.path.isfile(path):
        raise hou.Error("Shader file not found: %s" % path)
    if path.lower().endswith(".oso"):
        with open(path, "r") as f:
            return f.read()
    oslc, include = _oslc()
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "shader.oso")
        proc = subprocess.run([oslc, "-q", "-I" + include, "-o", out, path],
                              capture_output=True, text=True, errors="replace")
        if proc.returncode != 0 or not os.path.isfile(out):
            raise hou.Error("oslc failed for %s:\n%s" % (path, proc.stderr or proc.stdout))
        with open(out, "r") as f:
            return f.read()


def _values(text):
    """Default values of an .oso parameter: numbers or quoted strings."""
    return [v[1:-1] if v.startswith('"') else v
            for v in re.findall(r'"(?:[^"\\]|\\.)*"|\S+', text)]


def parse_oso(text):
    """Shader name and parameters (name, type, default, output, metadata) of OSL bytecode."""
    shader = None
    params = []
    for line in text.splitlines():
        words = line.split()
        if shader is None and len(words) >= 2 and words[0] in (
                "shader", "surface", "displacement", "volume", "light"):
            shader = words[1]
            continue
        fields = line.split("\t")
        if len(fields) < 3 or fields[0] not in ("param", "oparam"):
            continue
        kind, osl_type, name = fields[0], fields[1], fields[2]
        rest = [f for f in fields[3:] if f]
        default = rest[0] if rest and not rest[0].startswith("%") else ""
        hints = " ".join(f for f in rest if f.startswith("%"))
        meta = {key: _values(value)[0] if _values(value) else ""
                for key, value in re.findall(r"%meta\{[\w ]+,(\w+),((?:\"(?:[^\"\\]|\\.)*\"|[^}])*)\}",
                                             hints)}
        params.append({"name": name, "type": osl_type, "default": _values(default),
                       "output": kind == "oparam", "meta": meta})
    if shader is None:
        raise hou.Error("Not OSL bytecode")
    return shader, params


def _parm_template(param):
    vop_type, size = OSL_TYPES[param["type"]]
    meta = param["meta"]
    label = meta.get("label") or param["name"]
    default = param["default"]
    if vop_type == "string":
        pt = hou.StringParmTemplate(param["name"], label, 1,
                                    default_value=(default[0] if default else "",))
        if meta.get("widget") == "filename":
            pt.setStringType(hou.stringParmType.FileReference)
    elif vop_type == "int" and meta.get("widget") in ("checkBox", "boolean"):
        pt = hou.ToggleParmTemplate(param["name"], label,
                                    default_value=bool(int(default[0])) if default else False)
    elif vop_type == "int":
        pt = hou.IntParmTemplate(param["name"], label, 1,
                                 default_value=(int(default[0]),) if default else (0,))
    else:
        values = [float(v) for v in default] or [0.0]
        values = (values * size)[:size]
        pt = hou.FloatParmTemplate(param["name"], label, size, default_value=values)
        if vop_type == "color":
            pt.setLook(hou.parmLook.ColorSquare)
            pt.setNamingScheme(hou.parmNamingScheme.RGBA)
    for key, setter in (("min", "setMinValue"), ("max", "setMaxValue")):
        if key in meta and hasattr(pt, setter):
            try:
                getattr(pt, setter)(float(meta[key]))
            except ValueError:
                pass
    if meta.get("help"):
        pt.setHelp(meta["help"])
    return pt


def _filepath_template():
    pt = hou.StringParmTemplate("filepath", "Shader File", 1,
                                string_type=hou.stringParmType.FileReference,
                                file_type=hou.fileType.Any)
    pt.setTags({"filechooser_pattern": "*.osl *.oso"})
    pt.setHelp("OSL shader source (.osl) or bytecode (.oso). Load Shader updates the ports.")
    return pt


def _load_button():
    pt = hou.ButtonParmTemplate("load", "Load Shader")
    pt.setScriptCallbackLanguage(hou.scriptLanguage.Python)
    pt.setScriptCallback("hou.phm().load_shader(kwargs['node'])")
    pt.setHelp("Read the shader file and update the inputs and outputs of this node.")
    return pt


def type_name(path):
    stem = os.path.splitext(os.path.basename(path))[0]
    return TYPE_PREFIX + (re.sub(r"[^0-9a-zA-Z_]", "_", stem).lower() or "shader")


def create_type(path, python_module):
    """Create (or update) the embedded VOP type for a shader file, return its name."""
    import shaderhda

    shader, params = parse_oso(oso_text(path))
    name = type_name(path)
    label = "Cycles Script " + shader

    inputs = [shaderhda.ShaderParameter("filepath", "string", _filepath_template(), None, False,
                                        shaderhda.ShaderParameterExtraInfos())]
    outputs = []
    for param in params:
        if param["type"] not in OSL_TYPES:
            continue
        vop_type = OSL_TYPES[param["type"]][0]
        if param["output"]:
            outputs.append(shaderhda.ShaderParameter(
                param["name"], vop_type, None, None, True, shaderhda.ShaderParameterExtraInfos()))
        else:
            ui = None if vop_type == "surface" else _parm_template(param)
            inputs.append(shaderhda.ShaderParameter(
                param["name"], vop_type, ui, None, True, shaderhda.ShaderParameterExtraInfos()))

    sh = shaderhda.Shader(name)
    sh.setShaderName(SHADER_NAME)
    sh.setHdaLabel(label)
    sh.setVopnetMask("cycles")
    sh.setRenderMask("cycles")
    sh.setShaderType("generic")
    sh.setToolKeywords(["cycles", "osl"])
    sh.signatureSet().addSignature(shaderhda.ShaderSignature(name, label, SHADER_NAME,
                                                             inputs, outputs))
    sh.signatureSet().setBestDefaultSignature()

    with tempfile.TemporaryDirectory() as tmp:
        hda_path = os.path.join(tmp, name + ".hda").replace("\\", "/")
        sh.addHDAToFile(hda_path, False)
        hou.hda.installFile(hda_path, change_oplibraries_file=False)
        definition = [d for d in hou.hda.definitionsInFile(hda_path)
                      if d.nodeTypeName() == name][0]
        definition.setIcon("$CYCLES_ICON")
        group = definition.parmTemplateGroup()
        group.insertAfter("filepath", _load_button())
        definition.setParmTemplateGroup(group)
        definition.addSection("PythonModule", python_module)
        definition.setExtraFileOption("PythonModule/IsPython", True)
        definition.copyToHDAFile("Embedded")
        hou.hda.uninstallFile(hda_path, change_oplibraries_file=False)
    return name


def load_shader(node):
    """Callback of "Load Shader": turn the node into the type of its shader file."""
    raw = node.parm("filepath").unexpandedString()
    path = node.parm("filepath").evalAsString()
    if not path:
        raise hou.NodeError("Set the shader file first")
    python_module = node.type().definition().sections()["PythonModule"].contents()
    name = create_type(path, python_module)
    if node.type().name() != name:
        node = node.changeNodeType(name, keep_name=True, keep_parms=True)
    node.parm("filepath").set(raw)
    return node
