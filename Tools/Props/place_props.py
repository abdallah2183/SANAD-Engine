"""Place the authored props in the scene.

The forest was laid out for the primitives it used to be: a cube trunk
scaled to 0.3 x 2.2 x 0.3 and three sphere canopies stacked above it.
The authored meshes carry their own size (the trunk is 3.3 m tall, the
canopy's crowns sit at 3.0-4.2 m), so the placements have to follow the
meshes instead of scaling them.

    python Tools/Props/place_props.py

* The trunk sits on the ground with no scale: the mesh IS the tree.
* The canopy is ONE entity per tree, at the trunk's position: the mesh
  already carries the three crowns.
* A rock sits on the ground too.
"""

import io
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SCENE = os.path.join(ROOT, "Games", "TreeWorld", "Content", "Scenes", "Main.nfscene")


def transform_line(block, position, scale):
    return ("  Transform: local(%g,%g,%g) world(%g,%g,%g) rot(0,0,0) scale(%g,%g,%g) "
            "parent(4294967295:0)" % (position[0], position[1], position[2],
                                      position[0], position[1], position[2],
                                      scale[0], scale[1], scale[2]))


def main():
    with io.open(SCENE, "r", encoding="utf-8", newline="") as f:
        text = f.read()

    blocks = text.split("---\n")
    out = []
    trunk_position = {}
    for block in blocks:
        m = re.search(r"(?m)^\s*Name:\s*(\S+)", block)
        name = m.group(1) if m else ""

        # The middle and the top canopy are gone: the authored canopy mesh
        # carries all three crowns, so three of them would be three trees.
        if re.match(r"^Tree\d+Canopy(Mid|Top)$", name):
            continue

        if re.match(r"^Tree\d+Trunk$", name):
            p = re.search(r"(?m)^\s*Transform:.*?local\(([-\d.e]+),([-\d.e]+),([-\d.e]+)\)",
                          block)
            x, z = float(p.group(1)), float(p.group(3))
            trunk_position[name.replace("Trunk", "")] = (x, z)
            block = re.sub(r"(?m)^\s*Transform:.*$",
                           transform_line(block, (x, 0.0, z), (1.0, 1.0, 1.0)), block, count=1)
        elif re.match(r"^Tree\d+CanopyLow$", name):
            key = name.replace("CanopyLow", "")
            x, z = trunk_position.get(key, (0.0, 0.0))
            block = re.sub(r"(?m)^\s*Transform:.*$",
                           transform_line(block, (x, 0.0, z), (1.0, 1.0, 1.0)), block, count=1)
        elif name.startswith("Rock"):
            p = re.search(r"(?m)^\s*Transform:.*?local\(([-\d.e]+),([-\d.e]+),([-\d.e]+)\)",
                          block)
            x, z = float(p.group(1)), float(p.group(3))
            block = re.sub(r"(?m)^\s*Transform:.*$",
                           transform_line(block, (x, 0.0, z), (1.0, 1.0, 1.0)), block, count=1)
        out.append(block)

    scene = "---\n".join(out)
    # The entity count follows the blocks: the middle and top canopies are
    # gone.
    count = len([b for b in out if re.search(r"(?m)^entity:\s*\S+", b)])
    scene = re.sub(r"(?m)^entity_count:\s*\d+", "entity_count: %d" % count, scene, count=1)
    with io.open(SCENE, "w", encoding="utf-8", newline="\n") as f:
        f.write(scene)
    print("scene: %d entities, the props placed on the ground" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
