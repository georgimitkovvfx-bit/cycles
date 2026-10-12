#!/usr/bin/env hython
"""
Generate the Cycles Script VOP, like Blender's Script node.

Usage:
    hython script2hda.py --hda-install-dir <output_dir>

The node has a shader file parameter and a "Load Shader" button; loading turns it into a VOP
type with the ports of that OSL shader (see script_node.py, its PythonModule).
"""

import argparse
import os
import sys

import hou

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import script_node  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hda-install-dir", required=True, help="Directory to write the HDA to")
    args = parser.parse_args()

    import shaderhda

    name = script_node.BASE_TYPE
    hda_path = os.path.join(args.hda_install_dir, "cycles-script.hda").replace("\\", "/")
    os.makedirs(args.hda_install_dir, exist_ok=True)
    # Start from scratch, also when an earlier install of the node is loaded.
    vop_type = hou.vopNodeTypeCategory().nodeTypes().get(name)
    if vop_type:
        for definition in vop_type.allInstalledDefinitions():
            hou.hda.uninstallFile(definition.libraryFilePath(), change_oplibraries_file=False)
    if os.path.exists(hda_path):
        os.remove(hda_path)

    inputs = [shaderhda.ShaderParameter("filepath", "string", script_node._filepath_template(),
                                        None, False, shaderhda.ShaderParameterExtraInfos())]
    sh = shaderhda.Shader(name)
    sh.setShaderName(script_node.SHADER_NAME)
    sh.setHdaLabel("Cycles Script")
    sh.setVopnetMask("cycles")
    sh.setRenderMask("cycles")
    sh.setShaderType("generic")
    sh.setToolKeywords(["cycles", "osl"])
    sh.setTabSubMenus(["Cycles/Script"])
    sh.signatureSet().addSignature(shaderhda.ShaderSignature(
        name, "Cycles Script", script_node.SHADER_NAME, inputs, []))
    sh.signatureSet().setBestDefaultSignature()
    sh.addHDAToFile(hda_path, False)

    hou.hda.installFile(hda_path, change_oplibraries_file=False)
    definition = [d for d in hou.hda.definitionsInFile(hda_path) if d.nodeTypeName() == name][0]
    definition.setIcon("$CYCLES_ICON")
    group = definition.parmTemplateGroup()
    group.insertAfter("filepath", script_node._load_button())
    definition.setParmTemplateGroup(group)
    with open(script_node.__file__, "r") as f:
        definition.addSection("PythonModule", f.read())
    definition.setExtraFileOption("PythonModule/IsPython", True)
    definition.save(hda_path)
    print("[script2hda] Wrote %s" % hda_path)


if __name__ == "__main__":
    main()
