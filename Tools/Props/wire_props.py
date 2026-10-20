"""Wire the authored props into the scene.

The props are authored as glTF (Tools/Props/make_props.py), cooked into
the engine's mesh format by NFModelImporter, and then referenced by the
scene the way every other mesh is: by the asset id the cooked mesh
carries.

    python Tools/Props/wire_props.py

reads Content/Meshes/{TreeTrunk,TreeCanopy,Rock}.nfmesh, adds the
registry entries, and points the scene's trees and rocks at the new
meshes.
"""

import io
import os
import re
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CONTENT = os.path.join(ROOT, "Games", "TreeWorld", "Content")
REGISTRY = os.path.join(CONTENT, "AssetRegistry.nfreg")
SCENE = os.path.join(CONTENT, "Scenes", "Main.nfscene")


def mesh_id(path):
    """The asset id a cooked .nfmesh carries (magic + version + 16 bytes)."""
    with open(path, "rb") as f:
        data = f.read(32)
    if data[:4] != b"EMFN":
        raise ValueError("%s is not a mesh asset" % path)
    raw = data[8:24]
    h = "".join("%02x" % b for b in raw)
    return "%s-%s-%s-%s-%s" % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32])


def deterministic_id(name):
    """A stable asset id for a prop, derived from its name.

    The CLI's mesh cook writes the geometry without an id (the editor's
    import assigns one), so the id is written here -- and it is DERIVED from
    the name, so re-running the script does not change every reference in
    the scene.
    """
    import hashlib
    digest = hashlib.sha256(name.encode("utf-8")).digest()[:16]
    digest = bytearray(digest)
    digest[6] = (digest[6] & 0x0F) | 0x40  # version 4
    digest[8] = (digest[8] & 0x3F) | 0x80  # variant
    h = "".join("%02x" % b for b in digest)
    return "%s-%s-%s-%s-%s" % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32]), bytes(digest)


def write_mesh_id(path, raw_id):
    """Patch the asset id into a cooked .nfmesh (magic + version + 16 bytes)."""
    with open(path, "r+b") as f:
        f.seek(8)
        f.write(raw_id)


def read_registry():
    with io.open(REGISTRY, "r", encoding="utf-8") as f:
        text = f.read()
    header, _, body = text.partition("---\n")
    entries = []
    for block in ("---\n" + body).split("---\n"):
        if block.strip():
            entries.append(block)
    return header, entries


def entry_field(entry, key):
    m = re.search(r"(?m)^%s:\s*(.*)$" % re.escape(key), entry)
    return m.group(1).strip() if m else ""


def write_registry(header, entries):
    out = header
    for e in entries:
        # The separator is a LINE: an entry that does not end with a newline
        # would glue "---" onto its last line, and the registry loader reads
        # the separator as a line of its own.
        out += "---\n" + e.rstrip("\n") + "\n"
    if not out.endswith("\n"):
        out += "\n"
    with io.open(REGISTRY, "w", encoding="utf-8", newline="\n") as f:
        f.write(out)


def main():
    props = {
        "TreeTrunk": os.path.join(CONTENT, "Meshes", "TreeTrunk.nfmesh"),
        "TreeCanopy": os.path.join(CONTENT, "Meshes", "TreeCanopy.nfmesh"),
        "Rock": os.path.join(CONTENT, "Meshes", "Rock.nfmesh"),
    }
    ids = {}
    for name, path in props.items():
        if not os.path.exists(path):
            print("missing %s - run NFModelImporter on the glTF first" % path)
            return 1
        current = mesh_id(path)
        if current == "00000000-0000-0000-0000-000000000000":
            # The CLI's cook writes no id; the editor's import does. The id is
            # derived from the name here so the scene's references stay stable
            # across re-runs.
            current, raw = deterministic_id(name)
            write_mesh_id(path, raw)
        ids[name] = current
        print("%-12s %s" % (name, ids[name]))

    # --- the registry ------------------------------------------------------
    header, entries = read_registry()
    existing = {entry_field(e, "logical"): e for e in entries}
    for name, path in props.items():
        logical = "content://Meshes/%s.nfmesh" % name
        cooked = "cache://Meshes/%s.nfmesh" % name
        if logical in existing:
            entry = existing[logical]
            entry = re.sub(r"(?m)^id:.*$", "id: " + ids[name], entry)
            existing[logical] = entry
            continue
        entries.append(
            "id: %s\ntype: Mesh\nlogical: %s\ncooked: %s\nfingerprint: 0\n"
            "format: nfmesh-v1\nversion: 1" % (ids[name], logical, cooked))
        print("registry: added %s" % logical)
    header = re.sub(r"(?m)^count:\s*\d+", "count: %d" % len(entries), header, count=1)
    write_registry(header, entries)

    # --- the scene ---------------------------------------------------------
    with io.open(SCENE, "r", encoding="utf-8", newline="") as f:
        scene = f.read()

    blocks = scene.split("---\n")
    out = []
    for block in blocks:
        name = re.search(r"(?m)^\s*Name:\s*(\S+)", block)
        name = name.group(1) if name else ""
        if name.startswith("Tree") and name.endswith("Trunk"):
            block = re.sub(r"(?m)^(\s*Mesh:\s*asset_id=)\S+",
                           r"\g<1>" + ids["TreeTrunk"], block)
        elif name.startswith("Tree") and "Canopy" in name:
            block = re.sub(r"(?m)^(\s*Mesh:\s*asset_id=)\S+",
                           r"\g<1>" + ids["TreeCanopy"], block)
        elif name.startswith("Rock"):
            block = re.sub(r"(?m)^(\s*Mesh:\s*asset_id=)\S+",
                           r"\g<1>" + ids["Rock"], block)
        out.append(block)
    with io.open(SCENE, "w", encoding="utf-8", newline="\n") as f:
        f.write("---\n".join(out))
    print("scene: trees and rocks now use the authored meshes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
