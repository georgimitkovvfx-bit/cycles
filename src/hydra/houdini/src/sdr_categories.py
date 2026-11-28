"""
Menu categories of the Cycles shader nodes for the shader registry (sdrCycles), as the Houdini package's Tab menu
shows them: Blender's Add menu ("Cycles/Shader", "Cycles/Utilities/Math"), from blender_layout.tab_menu.

    python3 sdr_categories.py <shader_nodes.json> ../blender_node_layouts.json > ../../resources/sdr/categories.json

shader_nodes.json is written by shaders_node_dump (the Houdini build). Run it again when Cycles' nodes change:
src/hydra/test's test_shader_registry fails when a node has no category.
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import blender_layout  # noqa: E402


def main() -> int:
    nodes = json.loads(Path(sys.argv[1]).read_text())["nodes"]
    layouts = json.loads(Path(sys.argv[2]).read_text())
    categories = {node["name"]: blender_layout.tab_menu(node, layouts) for node in nodes}
    print(json.dumps(dict(sorted(categories.items())), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
