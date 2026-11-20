#!/usr/bin/env hython
"""
Generate the Cycles World LOP.

Usage:
    hython cyclesworld2hda.py --hda-install-dir <output_dir>

The node is a Blender-like world: a dome light whose Cycles world material is built from shader
nodes the user edits, like Blender's World nodes. Inside the (locked) node, a Material Library
holds a USD Cycles Material Builder that is an editable node of the HDA: Edit World Nodes jumps
into it, Apply Preset replaces its nodes with one of the presets (HDRI, Sky, Color, Gradient,
HDRI + Camera Background).

The presets link the Background strength and the HDRI file to the node's Strength and HDRI File
parameters by channel references, so these stay in sync until the user rewires the nodes.
"""

import argparse
import os
import sys

import hou
import voptoolutils

NODE_TYPE = "cyclesworld"
LABEL = "Cycles World"


def log(message):
    print("[cyclesworld2hda] " + message)


TOOL_XML = """<?xml version="1.0" encoding="UTF-8"?>
<shelfDocument>
  <tool name="$HDA_DEFAULT_TOOL" label="$HDA_LABEL" icon="$HDA_ICON">
    <toolMenuContext name="viewer">
      <contextNetType>LOP</contextNetType>
    </toolMenuContext>
    <toolMenuContext name="network">
      <contextOpType>$HDA_TABLE_AND_NAME</contextOpType>
    </toolMenuContext>
    <toolSubmenu>Cycles</toolSubmenu>
    <toolSubmenu>Lighting</toolSubmenu>
    <script scriptType="python"><![CDATA[import loptoolutils

loptoolutils.genericTool(kwargs, '$HDA_NAME')]]></script>
  </tool>
</shelfDocument>
"""

MATLIB = "world_material"
BUILDER = "world"

# The node's PythonModule: presets, Apply Preset and Edit World Nodes. The generator also runs it
# to build the default (HDRI) network stored in the HDA.
MODULE = r'''import hashlib

import hou

MATLIB = "%(matlib)s"
BUILDER = "%(builder)s"
SIGNATURE = "cyclesworld_signature"
# From a node in the builder: builder, Material Library, this node.
TOP = "../../../"
STRENGTH = 'ch("%%sstrength")' %% TOP
HDRI_FILE = '`chs("%%shdrifile")`' %% TOP
# Blender's default world color.
GREY = (0.0509, 0.0509, 0.0509)


def builder(node):
    return node.node(MATLIB + "/" + BUILDER)


def _node(net, node_type, name, parms=None, inputs=()):
    n = net.createNode(node_type, name)
    for parm, value in (parms or {}).items():
        if isinstance(value, tuple):
            n.parmTuple(parm).set(value)
        else:
            n.parm(parm).set(value)
    for input_name, (src, output_name) in inputs:
        n.setNamedInput(input_name, src, output_name)
    return n


def _link(node, parm, top=None):
    """Drive the node's parameter (all components) by the world node's parameter of the same
    name (or top), so the preset's settings show on the world node."""
    for p in node.parmTuple(parm):
        p.setExpression('ch("%%s%%s")' %% (TOP, (top or parm) + p.name()[len(parm):]),
                        hou.exprLanguage.Hscript)


def _background(net, name, color=None, parms=None):
    bg = _node(net, "cycles_background_shader", name, parms,
               [("color", color)] if color else ())
    bg.parm("strength").setExpression(STRENGTH, hou.exprLanguage.Hscript)
    return bg


def _hdri(net):
    env = _node(net, "cycles_environment_texture", "environment_texture",
                {"filename": HDRI_FILE})
    hsv = _node(net, "cycles_hsv", "hue_saturation_value", inputs=[("color", (env, "color"))])
    bc = _node(net, "cycles_brightness_contrast", "brightness_contrast",
               inputs=[("color", (hsv, "color"))])
    for node, parms in ((hsv, ("hue", "saturation", "value")), (bc, ("bright", "contrast"))):
        for parm in parms:
            _link(node, parm)
    return _background(net, "background", (bc, "color"))


def _sky(net):
    sky = _node(net, "cycles_sky_texture", "sky_texture", {"sky_type": "multiple_scattering"})
    for parm in SKY_PARMS:
        _link(sky, parm)
    return _background(net, "background", (sky, "color"))


def _color(net):
    bg = _background(net, "background")
    _link(bg, "color")
    return bg


def _gradient(net):
    # Ray direction (Generated in a world shader), Y up: -1 down, 1 up, mapped to the ramp.
    coords = _node(net, "cycles_texture_coordinate", "texture_coordinate")
    xyz = _node(net, "cycles_separate_xyz", "separate_xyz",
                inputs=[("vector", (coords, "generated"))])
    up = _node(net, "cycles_map_range", "map_range", {"from_min": -1.0, "from_max": 1.0},
               [("value", (xyz, "y"))])
    ramp = _node(net, "cycles_rgb_ramp", "color_ramp", inputs=[("fac", (up, "result"))])
    linear = hou.rampBasis.Linear
    ramp.parm("ramp").set(hou.Ramp(
        (linear, linear, linear, linear), (0.0, 0.49, 0.5, 1.0),
        ((0.2, 0.18, 0.16), (0.2, 0.18, 0.16), (0.8, 0.85, 0.9), (0.2, 0.4, 0.8))))
    # Ground below the horizon, then horizon to zenith.
    for key, top in ((1, "ground"), (2, "ground"), (3, "horizon"), (4, "zenith")):
        _link(ramp, "ramp%%dc" %% key, top)
    return _background(net, "background", (ramp, "color"))


def _hdri_camera(net):
    light = _hdri(net)
    light.setName("background_lighting")
    camera = _background(net, "background_camera")
    _link(camera, "color")
    path = _node(net, "cycles_light_path", "light_path")
    return _node(net, "cycles_mix_closure", "mix_shader", inputs=[
        ("fac", (path, "is_camera_ray")), ("closure1", (light, "background")),
        ("closure2", (camera, "background"))])


PRESETS = {"hdri": _hdri, "sky": _sky, "color": _color, "gradient": _gradient,
           "hdri_camera": _hdri_camera}
SKY_PARMS = ("sun_disc", "sun_size", "sun_intensity", "sun_elevation", "sun_rotation", "altitude",
             "air_density", "aerosol_density", "ozone_density")


def build(net, preset):
    """Replace the builder's shader nodes with the preset's network."""
    for child in net.children():
        if child.type().name() != "subinput":
            child.destroy()
    shader = PRESETS[preset](net)
    output = net.createNode("suboutput", "output")
    output.setInput(0, shader)
    net.layoutChildren()


def _parm_state(parm):
    try:
        return parm.expression()
    except hou.OperationFailed:
        pass
    if parm.parmTemplate().type() == hou.parmTemplateType.String:
        return parm.unexpandedString()
    return repr(parm.eval())


def signature(net):
    """Nodes, wires and parameter values of the builder (not their positions)."""
    items = []
    for child in sorted(net.children(), key=lambda n: n.name()):
        items.append((child.name(), child.type().name(),
                      [(c.inputIndex(), c.inputNode().name(), c.outputIndex())
                       for c in child.inputConnections()],
                      [(p.name(), _parm_state(p)) for p in child.parms()]))
    return hashlib.sha1(repr(items).encode()).hexdigest()


def is_edited(node):
    """True when the world nodes differ from the last applied preset."""
    return node.userData(SIGNATURE) != signature(builder(node))


def on_created(node):
    node.setUserData(SIGNATURE, signature(builder(node)))


def apply_preset(node):
    preset = node.parm("preset").evalAsString()
    if is_edited(node) and hou.isUIAvailable():
        label = node.parm("preset").menuLabels()[node.parm("preset").eval()]
        if hou.ui.displayMessage(
                "Replace the edited world nodes with the %%s preset?" %% label,
                buttons=("Replace", "Cancel"), default_choice=1, close_choice=1,
                severity=hou.severityType.Warning, title=node.name()) != 0:
            return
    net = builder(node)
    build(net, preset)
    node.setUserData(SIGNATURE, signature(net))
    # Shows the preset's parameters (hide when).
    node.parm("appliedpreset").set(preset)


def edit_nodes(node):
    """Show the world nodes in a network editor."""
    net = builder(node)
    if not hou.isUIAvailable():
        return
    editors = [p for p in hou.ui.paneTabs()
               if p.type() == hou.paneTabType.NetworkEditor and p.isCurrentTab()]
    editors += [p for p in hou.ui.paneTabs() if p.type() == hou.paneTabType.NetworkEditor]
    pane = editors[0] if editors else hou.ui.curDesktop().createFloatingPaneTab(
        hou.paneTabType.NetworkEditor)
    pane.setPwd(net)
''' % {"matlib": MATLIB, "builder": BUILDER}


# Authors the dome light. The Material Library before it authors the world material.
AUTHOR_SCRIPT = """import hou
from pxr import Gf, Sdf, UsdGeom, UsdLux

top = hou.pwd().parent()
stage = hou.pwd().editableStage()

light = UsdLux.DomeLight.Define(stage, top.evalParm("lightpath"))
light.CreateIntensityAttr(1.0)
light.GetPrim().CreateAttribute("inputs:cycles:light:world_material",
                                Sdf.ValueTypeNames.String).Set(top.evalParm("materialpath"))
xform = UsdGeom.Xformable(light)
xform.ClearXformOpOrder()
xform.AddRotateXYZOp().Set(Gf.Vec3f(*top.evalParmTuple("r")))
"""


def parameters(module):
    group = hou.ParmTemplateGroup()
    group.append(hou.StringParmTemplate(
        "lightpath", "Light Path", 1, default_value=("/lights/$OS",),
        help="Dome light the world material is the world of."))
    group.append(hou.StringParmTemplate(
        "materialpath", "Material Path", 1, default_value=("/materials/$OS",),
        help="World material built from the world nodes."))
    group.append(hou.SeparatorParmTemplate("sep1"))

    group.append(hou.MenuParmTemplate(
        "preset", "Preset", ("hdri", "sky", "color", "gradient", "hdri_camera"),
        ("HDRI", "Sky", "Color", "Gradient", "HDRI + Camera Background"),
        default_value=0, store_default_value_as_string=True, join_with_next=True,
        help="World node setup that Apply Preset builds: an Environment Texture with color "
             "adjustments, a physical Sky Texture with sun disc, a constant color, a "
             "sky/horizon/ground gradient by direction, or an HDRI for the lighting with a "
             "separate background color seen by the camera (Light Path Is Camera Ray)."))
    group.append(hou.ButtonParmTemplate(
        "applypreset", "Apply Preset", join_with_next=True,
        script_callback="kwargs['node'].hm().apply_preset(kwargs['node'])",
        script_callback_language=hou.scriptLanguage.Python,
        help="Replace the world nodes with the preset. Asks first when the nodes were edited."))
    group.append(hou.ButtonParmTemplate(
        "editnodes", "Edit World Nodes",
        script_callback="kwargs['node'].hm().edit_nodes(kwargs['node'])",
        script_callback_language=hou.scriptLanguage.Python,
        help="Show the world nodes (a USD Cycles Material Builder) in the network editor, to "
             "edit them like Blender's World nodes."))

    group.append(hou.StringParmTemplate(
        "appliedpreset", "Applied Preset", 1, default_value=("hdri",), is_hidden=True,
        help="Preset the world nodes were last built from: its parameters are shown."))
    hdri = ("hdri", "hdri_camera")
    hdrifile = hou.StringParmTemplate(
        "hdrifile", "HDRI File", 1,
        default_value=("$HFS/houdini/pic/lookdev/envmap.exr",),
        string_type=hou.stringParmType.FileReference, file_type=hou.fileType.Image,
        help="Latlong image of the HDRI presets' Environment Texture (its Filename refers to "
             "this parameter).")
    _show_for(hdrifile, hdri)
    group.append(hdrifile)
    group.append(hou.FloatParmTemplate(
        "strength", "Strength", 1, default_value=(1.0,), min=0.0, max=10.0, min_is_strict=True,
        help="Strength of the world: the presets' Background nodes refer to this parameter."))
    group.append(hou.FloatParmTemplate(
        "r", "Rotation", 3, default_value=(0.0, 0.0, 0.0),
        help="Rotation of the dome light in degrees. Environment and Sky Textures follow it; "
             "other nodes see world-space directions (Y up)."))

    # The applied preset's node settings, which its nodes refer to (hidden for other presets).
    group.append(hou.SeparatorParmTemplate("sep2"))
    # The Cycles Sun & Sky LOP's defaults: sun 15 degrees up, compass azimuth 135 (south-east).
    sky = {"sun_disc": 1, "sun_size": 0.545, "sun_intensity": 1.0, "sun_elevation": 15.0,
           "sun_rotation": 45.0}
    linked = [("cycles_hsv", name, name, None, None, hdri)
              for name in ("hue", "saturation", "value")]
    linked += [("cycles_brightness_contrast", name, name, None, None, hdri)
               for name in ("bright", "contrast")]
    linked += [("cycles_background_shader", "color", "color", None, module["GREY"],
                ("color", "hdri_camera"))]
    linked += [("cycles_sky_texture", name, name, None, sky.get(name), ("sky",))
               for name in module["SKY_PARMS"]]
    linked += [("cycles_background_shader", "color", name, label, value, ("gradient",))
               for name, label, value in (("ground", "Ground", (0.2, 0.18, 0.16)),
                                          ("horizon", "Horizon", (0.8, 0.85, 0.9)),
                                          ("zenith", "Sky", (0.2, 0.4, 0.8)))]
    for vop, vop_parm, name, label, value, presets in linked:
        template = hou.nodeType(hou.vopNodeTypeCategory(), vop).parmTemplateGroup().find(
            vop_parm).clone()
        template.setName(name)
        if label:
            template.setLabel(label)
        if isinstance(value, tuple):
            template.setDefaultValue(value)
        elif value is not None:
            template.setDefaultValue(bool(value) if template.type() == hou.parmTemplateType.Toggle
                                     else (value,))
        template.setConditional(hou.parmCondType.DisableWhen, "")
        _show_for(template, presets)
        group.append(template)
    return group


def _show_for(template, presets):
    template.setConditional(hou.parmCondType.HideWhen, "{ %s }" % " ".join(
        'appliedpreset != "%s"' % preset for preset in presets))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hda-install-dir", required=True, help="Directory to write the HDA to")
    args = parser.parse_args()

    os.makedirs(args.hda_install_dir, exist_ok=True)
    hda_path = os.path.join(args.hda_install_dir, "cycles-world.hda")

    # Start from scratch, also when an earlier install of the node is loaded.
    lop_type = hou.lopNodeTypeCategory().nodeTypes().get(NODE_TYPE)
    if lop_type:
        for definition in lop_type.allInstalledDefinitions():
            hou.hda.uninstallFile(definition.libraryFilePath(), change_oplibraries_file=False)
    if os.path.exists(hda_path):
        os.remove(hda_path)
    # The presets use the Cycles shader nodes: on a first install no package loads them yet, take
    # the ones shader2hda.py just wrote next to this HDA.
    if not hou.nodeType(hou.vopNodeTypeCategory(), "cycles_background_shader"):
        for name in sorted(os.listdir(args.hda_install_dir)):
            if name.startswith("cycles-") and name.endswith(".hda"):
                hou.hda.installFile(os.path.join(args.hda_install_dir, name),
                                    change_oplibraries_file=False)

    module = {}
    exec(MODULE, module)

    stage = hou.node("/stage")
    subnet = stage.createNode("subnet", NODE_TYPE)
    matlib = subnet.createNode("materiallibrary", MATLIB)
    matlib.setInput(0, subnet.indirectInputs()[0])
    net = voptoolutils._setupUsdPreviewBuilderSubnet(
        destination_node=matlib, name=BUILDER, mask="cycles " + voptoolutils.USDPREVIEW_TAB_MASK,
        folder_label="USD Cycles Material Builder", render_context="cycles")
    module["build"](net, "hdri")
    matlib.parm("materials").set(1)
    matlib.parm("matnode1").set(BUILDER)
    matlib.parm("matpath1").set('`chs("../materialpath")`')
    script = subnet.createNode("pythonscript", "dome_light")
    script.parm("python").set(AUTHOR_SCRIPT)
    script.setInput(0, matlib)
    outputs = [c for c in subnet.children() if c.type().name() == "output"]
    output = outputs[0] if outputs else subnet.createNode("output", "output0")
    output.setInput(0, script)
    subnet.layoutChildren()

    hda_node = subnet.createDigitalAsset(
        name=NODE_TYPE, hda_file_name=hda_path, description=LABEL,
        min_num_inputs=0, max_num_inputs=1, ignore_external_references=True)
    definition = hda_node.type().definition()
    definition.setParmTemplateGroup(parameters(module))
    definition.setIcon("$CYCLES_ICON")
    definition.addSection("Tools.shelf", TOOL_XML)
    definition.addSection("PythonModule", MODULE)
    definition.setExtraFileOption("PythonModule/IsPython", True)
    definition.addSection("OnCreated", "kwargs['node'].hm().on_created(kwargs['node'])")
    definition.setExtraFileOption("OnCreated/IsPython", True)
    # The world nodes stay editable while the node is locked (saved with the scene).
    definition.addSection("EditableNodes", MATLIB + "/" + BUILDER)
    definition.setExtraFileOption("UnlockOnCreate", False)
    definition.save(hda_path)

    # Check the authored light and material, editing inside the locked node and Apply Preset.
    hou.hda.installFile(hda_path, change_oplibraries_file=False)
    test = stage.createNode(NODE_TYPE)
    test.parmTuple("r").set((0.0, 30.0, 0.0))
    usd = test.stage()
    light = usd.GetPrimAtPath("/lights/%s" % test.name())
    if not light or light.GetTypeName() != "DomeLight":
        raise RuntimeError("Dome light not created")
    material_path = "/materials/%s" % test.name()
    if light.GetAttribute("inputs:cycles:light:world_material").Get() != material_path:
        raise RuntimeError("World material not set on the dome light")
    if tuple(light.GetAttribute("xformOp:rotateXYZ").Get()) != (0.0, 30.0, 0.0):
        raise RuntimeError("Rotation not authored on the dome light")
    material = usd.GetPrimAtPath(material_path)
    if not material or not material.GetAttribute("outputs:cycles:surface").HasAuthoredConnections():
        raise RuntimeError("World material %s has no connected outputs:cycles:surface"
                           % material_path)
    env = [p for p in material.GetChildren()
           if p.GetAttribute("info:id").Get() == "cycles_environment_texture"]
    hdri = hou.text.expandString(test.evalParm("hdrifile"))
    if not env or env[0].GetAttribute("inputs:filename").Get().path != hdri:
        raise RuntimeError("Environment Texture does not use the HDRI File parameter")
    net = test.hm().builder(test)
    if not test.isLockedHDA() or not net.isEditableInsideLockedHDA():
        raise RuntimeError("World nodes are not editable inside the locked node")
    if test.hm().is_edited(test):
        raise RuntimeError("New node reports edited world nodes")
    net.node("hue_saturation_value").parm("fac").set(0.5)
    if not test.hm().is_edited(test):
        raise RuntimeError("Edit of the world nodes not detected")
    # Per preset: a parameter shown only for it, and the node it drives.
    shown = {"hdri": ("saturation", "hue_saturation_value", "saturation"),
             "sky": ("sun_elevation", "sky_texture", "sun_elevation"),
             "color": ("colorg", "background", "colorg"),
             "gradient": ("horizong", "color_ramp", "ramp3cg"),
             "hdri_camera": ("colorg", "background_camera", "colorg")}
    for preset in module["PRESETS"]:
        test.parm("preset").set(preset)
        test.hm().apply_preset(test)
        material = test.stage().GetPrimAtPath(material_path)
        if not material.GetAttribute("outputs:cycles:surface").HasAuthoredConnections():
            raise RuntimeError("Preset %s: outputs:cycles:surface not connected" % preset)
        if test.hm().is_edited(test):
            raise RuntimeError("Preset %s reports edited world nodes" % preset)
        parm, node, node_parm = shown[preset]
        test.updateParmStates()  # Evaluates the hide when conditionals without a parameter pane.
        visible = {name for name, *_ in shown.values() if not test.parm(name).isHidden()}
        if visible != {parm} | ({"saturation"} if preset == "hdri_camera" else set()):
            raise RuntimeError("Preset %s shows %s" % (preset, sorted(visible)))
        test.parm(parm).set(0.25)
        if abs(net.node(node).evalParm(node_parm) - 0.25) > 1e-6:
            raise RuntimeError("Preset %s: %s does not drive %s" % (preset, parm, node))
        test.parm(parm).revertToDefaults()
    log("Wrote %s" % hda_path)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        import traceback
        traceback.print_exc()
        log("Error: %s" % e)
        sys.exit(1)
