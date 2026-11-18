"""Material settings of USD Cycles Material Builders, like Blender's Material Properties > Settings.

The Cycles shader translator (husdplugins/shadertranslators/cycles.py) authors the parameters
tagged cycles_material_setting as config:cycles:<setting> attributes on the material when they
differ from their defaults (the Cycles defaults, which are Blender's).

Used by the USD Cycles Material Builder shelf tool and by the "Add Cycles Material Settings" entry
of the node menu (OPmenu.xml), for builders made before the settings existed.
"""

import hou


def _menu(setting, label, items, default, help):
    return hou.MenuParmTemplate(
        "cycles_" + setting, label, [i[0] for i in items], [i[1] for i in items],
        default_value=[i[0] for i in items].index(default), help=help,
        tags={"cycles_material_setting": setting})


def _toggle(setting, label, default, help):
    return hou.ToggleParmTemplate("cycles_" + setting, label, default_value=default, help=help,
                                  tags={"cycles_material_setting": setting})


def settings_folder():
    """The Settings tab with every material setting."""
    surface = hou.FolderParmTemplate("cycles_surface", "Surface",
                                     folder_type=hou.folderType.Collapsible)
    surface.setTags({"group_default": "1"})
    surface.addParmTemplate(_menu(
        "displacement_method", "Displacement",
        [("bump", "Bump Only"), ("true", "Displacement Only"), ("both", "Displacement and Bump")],
        "bump",
        "Method to use for the displacement output: bump mapping, true displacement of the "
        "surface, or both (true displacement with bump mapping for the finer details)."))
    surface.addParmTemplate(_menu(
        "emission_sampling_method", "Emission Sampling",
        [("none", "None"), ("auto", "Auto"), ("front", "Front"), ("back", "Back"),
         ("front_back", "Front and Back")],
        "auto", "Sampling strategy for emissive surfaces."))
    surface.addParmTemplate(_toggle(
        "use_transparent_shadow", "Transparent Shadows", True,
        "Use transparent shadows for this material if it contains a Transparent BSDF, disabling "
        "will render faster but not give accurate shadows."))
    surface.addParmTemplate(_toggle(
        "use_bump_map_correction", "Bump Map Correction", True,
        "Apply corrections to solve shadow terminator artifacts caused by bump mapping."))

    volume = hou.FolderParmTemplate("cycles_volume", "Volume",
                                    folder_type=hou.folderType.Collapsible)
    volume.addParmTemplate(_menu(
        "volume_sampling_method", "Sampling",
        [("distance", "Distance"), ("equiangular", "Equiangular"),
         ("multiple_importance", "Multiple Importance")],
        "multiple_importance", "Sampling method to use for volumes."))
    volume.addParmTemplate(_menu(
        "volume_interpolation_method", "Interpolation",
        [("linear", "Linear"), ("cubic", "Cubic")],
        "linear", "Interpolation method to use for smoke/fire volumes."))
    volume.addParmTemplate(hou.FloatParmTemplate(
        "cycles_volume_step_rate", "Step Rate", 1, default_value=(1.0,), min=0.001, max=10.0,
        min_is_strict=True,
        help="Scale the distance between volume shader samples when rendering the volume (lower "
             "values give more accurate and detailed results, but also increased render time).",
        tags={"cycles_material_setting": "volume_step_rate"}))

    settings = hou.FolderParmTemplate("cycles_settings", "Settings",
                                      folder_type=hou.folderType.Tabs)
    settings.addParmTemplate(hou.IntParmTemplate(
        "cycles_pass_id", "Pass Index", 1, default_value=(0,), min=0, max=100, min_is_strict=True,
        help="Index number for the Material Index render pass.",
        tags={"cycles_material_setting": "pass_id"}))
    settings.addParmTemplate(surface)
    settings.addParmTemplate(volume)
    return settings


def has_settings(node):
    return any("cycles_material_setting" in p.parmTemplate().tags() for p in node.parms())


def can_add_settings(node):
    """True for a material builder (a VOP subnet) without the settings."""
    return (isinstance(node, hou.VopNode) and node.isNetwork() and not node.isLockedHDA()
            and not has_settings(node))


def add_settings(node):
    """Adds the Settings tab to a material builder, unless it has one."""
    if has_settings(node):
        return False
    group = node.parmTemplateGroup()
    group.append(settings_folder())
    node.setParmTemplateGroup(group)
    return True
