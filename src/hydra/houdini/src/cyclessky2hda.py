#!/usr/bin/env hython
"""
Generate the Cycles Sun & Sky LOP.

Usage:
    hython cyclessky2hda.py --hda-install-dir <output_dir>

The node authors a dome light whose Cycles world material is a Sky Texture through a Background
shader, like a Blender world with a sky: the sky's sun disc lights the scene, and Cycles samples
the sun directly. The sun is placed by elevation and azimuth, or by a location, date and time
(NOAA solar position, like Blender's Sun Position add-on). An optional fog box adds fog.

Houdini is Y-up: the sky's zenith is the light's +Y, north is -Z and east is +X (North Offset
rotates the compass around +Y). The delegate orients sky textures of world materials this way
(HdCyclesMaterial, OrientWorldTextures).
"""

import argparse
import math
import os
import sys

import hou

NODE_TYPE = "cyclessky"
LABEL = "Cycles Sun & Sky"


def log(message):
    print("[cyclessky2hda] " + message)


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


# Solar position, shared by the node (PythonModule) and the self-check below.
SUN_POSITION = '''import math


def julian_day(year, month, day):
    if month <= 2:
        year -= 1
        month += 12
    a = year // 100
    b = 2 - a + a // 4
    return (math.floor(365.25 * (year + 4716)) + math.floor(30.6001 * (month + 1)) + day + b -
            1524.5)


def sun_position(latitude, longitude, utc_offset, year, month, day, hours):
    """Sun (elevation, azimuth) in degrees at a local clock time, NOAA's solar position
    equations. Azimuth is clockwise from north, longitude positive east of Greenwich, utc_offset
    in hours (daylight saving time included)."""
    jd = julian_day(year, month, day) + (hours - utc_offset) / 24.0
    t = (jd - 2451545.0) / 36525.0
    rad = math.radians
    l0 = (280.46646 + t * (36000.76983 + 0.0003032 * t)) % 360.0
    m = 357.52911 + t * (35999.05029 - 0.0001537 * t)
    e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t)
    c = (math.sin(rad(m)) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
         math.sin(rad(2 * m)) * (0.019993 - 0.000101 * t) + math.sin(rad(3 * m)) * 0.000289)
    omega = 125.04 - 1934.136 * t
    apparent_longitude = l0 + c - 0.00569 - 0.00478 * math.sin(rad(omega))
    obliquity = (23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) /
                 60.0 + 0.00256 * math.cos(rad(omega)))
    declination = math.asin(math.sin(rad(obliquity)) * math.sin(rad(apparent_longitude)))
    y = math.tan(rad(obliquity) / 2.0) ** 2
    equation_of_time = 4.0 * math.degrees(
        y * math.sin(2 * rad(l0)) - 2 * e * math.sin(rad(m)) +
        4 * e * y * math.sin(rad(m)) * math.cos(2 * rad(l0)) -
        0.5 * y * y * math.sin(4 * rad(l0)) - 1.25 * e * e * math.sin(2 * rad(m)))
    solar_minutes = (hours * 60.0 + equation_of_time + 4.0 * longitude - 60.0 * utc_offset) % 1440
    hour_angle = rad(solar_minutes / 4.0 - 180.0)
    lat = rad(latitude)
    cos_zenith = (math.sin(lat) * math.sin(declination) +
                  math.cos(lat) * math.cos(declination) * math.cos(hour_angle))
    elevation = 90.0 - math.degrees(math.acos(max(-1.0, min(1.0, cos_zenith))))
    azimuth = (math.degrees(math.atan2(
        math.sin(hour_angle),
        math.cos(hour_angle) * math.sin(lat) - math.tan(declination) * math.cos(lat))) + 180.0)
    return elevation, azimuth % 360.0


def sun_angles(node):
    """Sky Texture sun elevation and rotation (radians) from the node's parameters."""
    if node.parm("sunmode").evalAsString() == "location":
        year, month, day = (int(v) for v in node.evalParmTuple("date"))
        elevation, azimuth = sun_position(node.evalParm("latitude"), node.evalParm("longitude"),
                                          node.evalParm("utcoffset"), year, month, day,
                                          node.evalParm("time"))
    else:
        elevation, azimuth = node.evalParm("elevation"), node.evalParm("azimuth")
    # Sky space is (z, x, y) of the light (OrientWorldTextures) and Cycles negates the rotation
    # (SkyTextureNode::simplify_settings): rotation 0 points along +X, 90 along +Z, so azimuth 0
    # (north) is -Z.
    rotation = azimuth - 90.0 + node.evalParm("northoffset")
    return math.radians(elevation), math.radians(rotation)
'''


# Authors the dome light, the world material and its shaders.
AUTHOR_SCRIPT = """import math

import hou
from pxr import Sdf, UsdGeom, UsdLux, UsdShade

top = hou.pwd().parent()
stage = hou.pwd().editableStage()
elevation, rotation = top.hdaModule().sun_angles(top)

light_path = top.evalParm("lightpath")
material_path = top.evalParm("materialpath")

light = UsdLux.DomeLight.Define(stage, light_path)
light.CreateIntensityAttr(1.0)
light.GetPrim().CreateAttribute("inputs:cycles:light:world_material",
                                Sdf.ValueTypeNames.String).Set(material_path)

material = UsdShade.Material.Define(stage, material_path)


def shader(name, node_id, inputs, parent=material_path):
    s = UsdShade.Shader.Define(stage, parent + "/" + name)
    s.CreateIdAttr(node_id)
    for input_name, value_type, value in inputs:
        s.CreateInput(input_name, value_type).Set(value)
    return s


T = Sdf.ValueTypeNames
sky = shader("sky", "cycles_sky_texture", [
    ("sky_type", T.String, top.parm("skytype").evalAsString()),
    ("sun_disc", T.Bool, bool(top.evalParm("sundisc"))),
    ("sun_size", T.Float, math.radians(top.evalParm("sunsize"))),
    ("sun_intensity", T.Float, top.evalParm("sunintensity")),
    ("sun_elevation", T.Float, elevation),
    ("sun_rotation", T.Float, rotation),
    ("altitude", T.Float, top.evalParm("altitude")),
    ("air_density", T.Float, top.evalParm("air")),
    ("aerosol_density", T.Float, top.evalParm("aerosol")),
    ("ozone_density", T.Float, top.evalParm("ozone")),
    ("turbidity", T.Float, top.evalParm("turbidity")),
    ("ground_albedo", T.Float, top.evalParm("groundalbedo")),
])
sky_color = sky.CreateOutput("color", T.Color3f)

background = shader("background", "cycles_background_shader", [
    ("strength", T.Float, top.evalParm("strength")),
])
background.CreateInput("color", T.Color3f).ConnectToSource(sky_color)
material.CreateOutput("cycles:surface", T.Token).ConnectToSource(
    background.CreateOutput("background", T.Token))

# Fog in a box around the scene, not as the world volume: Cycles fully absorbs the sky and sun
# (infinitely far) in a world volume, they light the fog through the box's sides.
if top.evalParm("fog"):
    fog_path = top.evalParm("fogpath")
    fog_material = UsdShade.Material.Define(stage, fog_path + "_material")
    fog = shader("fog", "cycles_scatter_volume", [
        ("color", T.Color3f, top.evalParmTuple("fogcolor")),
        ("density", T.Float, top.evalParm("fogdensity")),
        ("anisotropy", T.Float, top.evalParm("foganisotropy")),
    ], parent=fog_path + "_material")
    fog_material.CreateOutput("cycles:volume", T.Token).ConnectToSource(
        fog.CreateOutput("volume", T.Token))
    box = UsdGeom.Cube.Define(stage, fog_path)
    box.CreateSizeAttr(top.evalParm("fogsize"))
    box.AddTranslateOp().Set(tuple(top.evalParmTuple("fogcenter")))
    UsdShade.MaterialBindingAPI.Apply(box.GetPrim()).Bind(fog_material)
"""


def parameters():
    group = hou.ParmTemplateGroup()

    def hide(template, hide_when):
        if hide_when:
            template.setConditional(hou.parmCondType.HideWhen, hide_when)
        return template

    def float_parm(name, label, default, low, high, help, strict_low=False, strict_high=False,
                   hide_when=None, **kwargs):
        return hide(hou.FloatParmTemplate(name, label, 1, default_value=(default,), min=low,
                                          max=high, min_is_strict=strict_low,
                                          max_is_strict=strict_high, help=help, **kwargs),
                    hide_when)

    group.append(hou.StringParmTemplate(
        "lightpath", "Light Path", 1, default_value=("/lights/$OS",),
        string_type=hou.stringParmType.Regular,
        help="Dome light the sky is the world of."))
    group.append(hou.StringParmTemplate(
        "materialpath", "Material Path", 1, default_value=("/materials/$OS",),
        help="World material with the Sky Texture (and the fog)."))

    sky = hou.FolderParmTemplate("sky_folder", "Sky", folder_type=hou.folderType.Simple)
    sky.addParmTemplate(hou.MenuParmTemplate(
        "skytype", "Sky Type",
        ("multiple_scattering", "single_scattering", "hosek_wilkie", "preetham"),
        ("Multiple Scattering", "Single Scattering", "Hosek / Wilkie", "Preetham"),
        default_value=0,
        help="Sky model, like Blender's Sky Texture. Multiple and Single Scattering are "
             "physically based (Nishita) and have a sun disc; Hosek / Wilkie and Preetham are "
             "analytic models without one.",
        store_default_value_as_string=True))
    sky.addParmTemplate(float_parm(
        "strength", "Strength", 1.0, 0, 10, "Strength of the world background (Background "
        "node). Physical skies are bright: lower it or the exposure for a darker image.",
        strict_low=True))
    physical = "{ skytype != multiple_scattering skytype != single_scattering }"
    analytic = "{ skytype == multiple_scattering } { skytype == single_scattering }"
    sky.addParmTemplate(float_parm(
        "altitude", "Altitude", 100.0, 0, 60000, "Height of the camera above sea level, in "
        "meters.", strict_low=True, disable_when=physical))
    sky.addParmTemplate(float_parm(
        "air", "Air", 1.0, 0, 10, "Density of air molecules (blue sky).", strict_low=True,
        disable_when=physical))
    sky.addParmTemplate(float_parm(
        "aerosol", "Aerosol", 1.0, 0, 10, "Density of dust, water droplets and pollutants "
        "(haze).", strict_low=True, disable_when=physical))
    sky.addParmTemplate(float_parm(
        "ozone", "Ozone", 1.0, 0, 10, "Density of ozone (sky color at sunset).",
        strict_low=True, disable_when=physical))
    sky.addParmTemplate(float_parm(
        "turbidity", "Turbidity", 2.2, 1, 10, "Atmospheric turbidity (haze) of the analytic "
        "models.", strict_low=True, disable_when=analytic))
    sky.addParmTemplate(float_parm(
        "groundalbedo", "Ground Albedo", 0.3, 0, 1, "Ground color reflected into the sky by "
        "the Hosek / Wilkie model.", strict_low=True, strict_high=True,
        disable_when="{ skytype != hosek_wilkie }"))
    group.append(sky)

    sun = hou.FolderParmTemplate("sun_folder", "Sun", folder_type=hou.folderType.Simple)
    sun.addParmTemplate(hou.ToggleParmTemplate(
        "sundisc", "Sun Disc", default_value=True, disable_when=physical,
        help="Include the sun in the sky. It lights the scene (sampled directly by Cycles)."))
    sun.addParmTemplate(float_parm(
        "sunsize", "Sun Size", 0.545, 0, 90, "Angular diameter of the sun disc, in degrees.",
        strict_low=True, disable_when="{ sundisc == 0 } " + physical))
    sun.addParmTemplate(float_parm(
        "sunintensity", "Sun Intensity", 1.0, 0, 10, "Strength of the sun disc.",
        strict_low=True, disable_when="{ sundisc == 0 } " + physical))
    sun.addParmTemplate(hou.MenuParmTemplate(
        "sunmode", "Sun Position", ("angles", "location"), ("Elevation and Azimuth",
                                                             "Location and Time"),
        default_value=0, store_default_value_as_string=True,
        help="Place the sun by angles, or by geographic location, date and time."))
    angles = "{ sunmode != angles }"
    location = "{ sunmode != location }"
    sun.addParmTemplate(float_parm(
        "elevation", "Elevation", 15.0, -90, 90, "Sun angle above the horizon, in degrees.",
        strict_low=True, strict_high=True, hide_when=angles))
    sun.addParmTemplate(float_parm(
        "azimuth", "Azimuth", 135.0, 0, 360, "Compass direction of the sun in degrees, "
        "clockwise from north (-Z) to east (+X), south (+Z) and west (-X).", hide_when=angles))
    sun.addParmTemplate(float_parm(
        "latitude", "Latitude", 48.8566, -90, 90, "Degrees north of the equator (negative "
        "south).", strict_low=True, strict_high=True, hide_when=location))
    sun.addParmTemplate(float_parm(
        "longitude", "Longitude", 2.3522, -180, 180, "Degrees east of Greenwich (negative "
        "west).", strict_low=True, strict_high=True, hide_when=location))
    sun.addParmTemplate(hide(hou.IntParmTemplate(
        "date", "Date (Y M D)", 3, default_value=(2026, 6, 21), min=1, max=2100,
        help="Year, month and day."), location))
    sun.addParmTemplate(float_parm(
        "time", "Local Time", 12.0, 0, 24, "Local clock time in hours (13.5 = 1:30 pm).",
        strict_low=True, strict_high=True, hide_when=location))
    sun.addParmTemplate(float_parm(
        "utcoffset", "UTC Offset", 2.0, -12, 14, "Time zone of the local time in hours, "
        "daylight saving time included (Paris in summer: 2).", strict_low=True,
        strict_high=True, hide_when=location))
    sun.addParmTemplate(float_parm(
        "northoffset", "North Offset", 0.0, -180, 180, "Rotates the compass around +Y, in "
        "degrees: 0 has north on -Z."))
    group.append(sun)

    fog = hou.FolderParmTemplate("fog_folder", "Fog", folder_type=hou.folderType.Simple)
    fog.addParmTemplate(hou.ToggleParmTemplate(
        "fog", "Fog", default_value=False,
        help="Fill a box around the scene with a homogeneous scattering volume, lit by the sky "
             "and sun through its sides. Keep the camera and scene inside it. (A world material "
             "volume would block the sky and sun completely: Cycles treats them as infinitely "
             "far away.)"))
    fog.addParmTemplate(hou.StringParmTemplate(
        "fogpath", "Fog Path", 1, default_value=("/fog/$OS",), disable_when="{ fog == 0 }",
        help="Fog box primitive (its material is <path>_material)."))
    fog.addParmTemplate(float_parm(
        "fogsize", "Size", 1000.0, 1, 10000, "Edge length of the fog box, in scene units.",
        strict_low=True, disable_when="{ fog == 0 }"))
    fog.addParmTemplate(hou.FloatParmTemplate(
        "fogcenter", "Center", 3, default_value=(0, 0, 0), disable_when="{ fog == 0 }",
        help="Center of the fog box."))
    fog.addParmTemplate(hou.FloatParmTemplate(
        "fogcolor", "Color", 3, default_value=(0.8, 0.8, 0.8), min=0, max=1,
        look=hou.parmLook.ColorSquare, naming_scheme=hou.parmNamingScheme.RGBA,
        disable_when="{ fog == 0 }"))
    fog.addParmTemplate(float_parm(
        "fogdensity", "Density", 0.002, 0, 1, "Volume density per scene unit.", strict_low=True,
        disable_when="{ fog == 0 }"))
    fog.addParmTemplate(float_parm(
        "foganisotropy", "Anisotropy", 0.0, -1, 1, "Scattering direction: negative back, "
        "positive forward.", strict_low=True, strict_high=True, disable_when="{ fog == 0 }"))
    group.append(fog)
    return group


def check_sun_position():
    """Known solar positions (NOAA calculator)."""
    module = {}
    exec(SUN_POSITION, module)
    # Paris, 2026-06-21 14:00 CEST: solar noon is ~13:52, elevation ~64.7 degrees, south.
    elevation, azimuth = module["sun_position"](48.8566, 2.3522, 2.0, 2026, 6, 21, 14.0)
    if abs(elevation - 64.7) > 0.5 or abs(azimuth - 185.0) > 5.0:
        raise RuntimeError("Paris noon sun at %.2f / %.2f" % (elevation, azimuth))
    # Equator at an equinox, 06:00 local solar time: sun rising in the east.
    elevation, azimuth = module["sun_position"](0.0, 0.0, 0.0, 2026, 3, 20, 6.12)
    if abs(elevation) > 1.5 or abs(azimuth - 90.0) > 2.0:
        raise RuntimeError("Equinox sunrise at %.2f / %.2f" % (elevation, azimuth))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hda-install-dir", required=True, help="Directory to write the HDA to")
    args = parser.parse_args()

    check_sun_position()

    os.makedirs(args.hda_install_dir, exist_ok=True)
    hda_path = os.path.join(args.hda_install_dir, "cycles-sky.hda")

    # Start from scratch, also when an earlier install of the node is loaded.
    lop_type = hou.lopNodeTypeCategory().nodeTypes().get(NODE_TYPE)
    if lop_type:
        for definition in lop_type.allInstalledDefinitions():
            hou.hda.uninstallFile(definition.libraryFilePath(), change_oplibraries_file=False)
    if os.path.exists(hda_path):
        os.remove(hda_path)

    stage = hou.node("/stage")
    subnet = stage.createNode("subnet", NODE_TYPE)
    script = subnet.createNode("pythonscript", "sun_and_sky")
    script.parm("python").set(AUTHOR_SCRIPT)
    script.setInput(0, subnet.indirectInputs()[0])
    outputs = [c for c in subnet.children() if c.type().name() == "output"]
    output = outputs[0] if outputs else subnet.createNode("output", "output0")
    output.setInput(0, script)

    hda_node = subnet.createDigitalAsset(
        name=NODE_TYPE, hda_file_name=hda_path, description=LABEL,
        min_num_inputs=0, max_num_inputs=1, ignore_external_references=True)
    definition = hda_node.type().definition()
    definition.setParmTemplateGroup(parameters())
    definition.setIcon("$CYCLES_ICON")
    definition.addSection("Tools.shelf", TOOL_XML)
    definition.addSection("PythonModule", SUN_POSITION)
    definition.setExtraFileOption("PythonModule/IsPython", True)
    definition.setExtraFileOption("UnlockOnCreate", False)
    definition.save(hda_path)

    # Check the authored light, material and sun angles.
    hou.hda.installFile(hda_path, change_oplibraries_file=False)
    test = stage.createNode(NODE_TYPE)
    test.parm("elevation").set(30.0)
    test.parm("azimuth").set(90.0)
    test.parm("fog").set(True)
    usd = test.stage()
    light = usd.GetPrimAtPath("/lights/%s" % test.name())
    if not light or light.GetTypeName() != "DomeLight":
        raise RuntimeError("Dome light not created")
    material_path = "/materials/%s" % test.name()
    if light.GetAttribute("inputs:cycles:light:world_material").Get() != material_path:
        raise RuntimeError("World material not set on the dome light")
    sky = usd.GetPrimAtPath(material_path + "/sky")
    if abs(sky.GetAttribute("inputs:sun_elevation").Get() - math.radians(30.0)) > 1e-5:
        raise RuntimeError("Sun elevation not authored in radians")
    if abs(sky.GetAttribute("inputs:sun_rotation").Get()) > 1e-5:
        raise RuntimeError("East azimuth is not sky rotation 0")
    material = usd.GetPrimAtPath(material_path)
    if not material.GetAttribute("outputs:cycles:surface").HasAuthoredConnections():
        raise RuntimeError("outputs:cycles:surface not connected")
    fog_box = usd.GetPrimAtPath("/fog/%s" % test.name())
    if not fog_box or fog_box.GetTypeName() != "Cube":
        raise RuntimeError("Fog box not created")
    test.parm("sunmode").set(1)
    test.parm("time").set(14.0)
    if test.stage().GetPrimAtPath(material_path + "/sky").GetAttribute(
            "inputs:sun_elevation").Get() < math.radians(60.0):
        raise RuntimeError("Location and Time does not place the Paris noon sun high")
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
