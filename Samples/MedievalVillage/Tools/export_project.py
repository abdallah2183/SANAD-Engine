#!/usr/bin/env python3
"""export_project.py — MedievalVillage sample -> editor project (.nfproj).

Builds a real NOVAForge editor project from the exact village the game plays:
  1. dumps the layout headlessly from NFSampleMedievalVillage.exe
     (--export-layout: layout.txt + ground.nfmesh, no window, no GPU),
  2. scaffolds a project with nf.exe,
  3. copies the kit's cooked meshes/textures + writes .nfmat materials,
  4. cooks everything (NFAssetCooker assigns the asset IDs),
  5. writes Content/Scenes/Main.nfscene from the dump + registry IDs,
  6. cooks again and packages with `nf build` to prove it is shippable.

Usage:
  python3 export_project.py [--out <dir>] [--force]

Defaults to <Documents>/MedievalVillage. Open the result by dragging
MedievalVillage.nfproj onto the NOVAForge launcher/editor window.

What you get in the editor is the whole village explorable in Play mode
(fly with RMB+WASD, physics preview). The crate quest itself (pickup,
carry, timer, day cycle) is C++ logic in Samples/MedievalVillage/main.cpp
and stays in NFSampleMedievalVillage.exe — the editor's Play is a scene
preview, not the game binary.
"""

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ENGINE_ROOT = Path(__file__).resolve().parents[3]
BIN = ENGINE_ROOT / "build" / "DebugNinja" / "bin"
NF = BIN / "nf.exe"
COOKER = BIN / "NFAssetCooker.exe"
SAMPLE = BIN / "NFSampleMedievalVillage.exe"
KIT_CONTENT = ENGINE_ROOT / "Samples" / "MedievalVillage" / "Content" / "Medieval"

# Tags mirror PlacementTag in Samples/MedievalVillage/Village.hpp.
TAG_STRUCTURE, TAG_DECOR, TAG_CRATE, TAG_WAGON, TAG_DOOR, TAG_FENCE = range(6)
COLLIDABLE = {TAG_STRUCTURE, TAG_CRATE, TAG_WAGON, TAG_DOOR, TAG_FENCE}

# Roughness per texture stem — kept in sync with KitAssets.cpp.
ROUGHNESS = {
    "T_RoundTiles_BaseColor": 0.55,
    "T_WoodTrim_BaseColor": 0.62,
    "T_RockTrim_BaseColor": 0.78,
    "T_Brick_BaseColor": 0.85,
    "T_RedBrick_BaseColor": 0.85,
    "T_UnevenBrick_BaseColor": 0.88,
    "T_Plaster_BaseColor": 0.94,
}
DEFAULT_ROUGHNESS = 0.90


def run(cmd, cwd=None):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors="replace")
    if p.returncode != 0:
        print(p.stdout[-3000:], flush=True)
        print(p.stderr[-3000:], flush=True)
        raise SystemExit(f"command failed ({p.returncode}): {cmd[0]}")
    return p


def parse_manifest(path):
    pieces = {}  # name -> dict(mesh_rel, tex, min, max)
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line.startswith("piece "):
            continue
        parts = line.split()
        name = parts[1]
        fields = {}
        for tok in parts[2:]:
            if "=" in tok:
                k, v = tok.split("=", 1)
                fields[k] = v
        mn = tuple(float(x) for x in fields["min"].split(","))
        mx = tuple(float(x) for x in fields["max"].split(","))
        pieces[name] = {"mesh_rel": fields["mesh"], "tex": fields["tex"], "min": mn, "max": mx}
    return pieces


def parse_layout(path):
    spawn = wagon = None
    lamps, placed = [], []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        t = line.split()
        if t[0] == "spawn":
            spawn = tuple(float(x) for x in t[1:4])
        elif t[0] == "wagon":
            wagon = tuple(float(x) for x in t[1:4])
        elif t[0] == "lamp":
            lamps.append(tuple(float(x) for x in t[1:4]))
        elif t[0] == "placed":
            name = t[1]
            x, y, z, roty, sx, sy, sz = (float(v) for v in t[2:9])
            placed.append({"piece": name, "pos": (x, y, z), "rot": roty,
                           "scale": (sx, sy, sz), "tag": int(t[9])})
    return spawn, wagon, lamps, placed


def rotate_y(v, deg):
    r = math.radians(deg)
    c, s = math.cos(r), math.sin(r)
    # Same convention as scene::compose_trs_mat4 / Village::rotate_y.
    return (v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c)


def placed_aabb(bounds, placed):
    mn, mx = bounds["min"], bounds["max"]
    out_min = [math.inf] * 3
    out_max = [-math.inf] * 3
    for i in range(8):
        lx = mx[0] if (i & 1) else mn[0]
        ly = mx[1] if (i & 2) else mn[1]
        lz = mx[2] if (i & 4) else mn[2]
        sc = placed["scale"]
        w = rotate_y((lx * sc[0], ly * sc[1], lz * sc[2]), placed["rot"])
        p = placed["pos"]
        w = (w[0] + p[0], w[1] + p[1], w[2] + p[2])
        for a in range(3):
            out_min[a] = min(out_min[a], w[a])
            out_max[a] = max(out_max[a], w[a])
    return tuple(out_min), tuple(out_max)


def parse_registry(path):
    mapping = {}  # logical -> id
    cur_id = cur_logical = None
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        s = line.strip()
        if s == "---":
            if cur_id and cur_logical:
                mapping[cur_logical] = cur_id
            cur_id = cur_logical = None
        elif s.startswith("id:"):
            cur_id = s[3:].strip()
        elif s.startswith("logical:"):
            cur_logical = s[8:].strip()
    if cur_id and cur_logical:
        mapping[cur_logical] = cur_id
    return mapping


def write_materials(mat_dir, tex_stems):
    mat_dir.mkdir(parents=True, exist_ok=True)

    def write(name, base, metallic, rough, albedo=None):
        lines = ["# NOVAForge Material v1", f"name: {name}",
                 f"base_color: {base[0]} {base[1]} {base[2]} {base[3]}",
                 f"metallic: {metallic}", f"roughness: {rough}", "ao: 1",
                 "emission: 0 0 0", "emission_strength: 0"]
        if albedo:
            lines.append(f"albedo: {albedo}")
        lines.append("mip: linear")
        (mat_dir / f"{name}.nfmat").write_text("\n".join(lines) + "\n", encoding="utf-8")

    for stem in sorted(tex_stems):
        write(stem, (1, 1, 1, 1), 0.0, ROUGHNESS.get(stem, DEFAULT_ROUGHNESS),
              f"content://Textures/{stem}.png")
    write("ironwork", (0.55, 0.56, 0.60, 1), 0.85, 0.42,
          "content://Textures/T_RockTrim_BaseColor.png")
    write("Ground", (0.60, 0.56, 0.49, 1), 0.0, 0.96,
          "content://Textures/T_UnevenBrick_BaseColor.png")
    write("Tunic", (0.45, 0.22, 0.12, 1), 0.0, 0.80)
    write("Skin", (0.92, 0.72, 0.55, 1), 0.0, 0.60)
    write("Hood", (0.25, 0.18, 0.12, 1), 0.0, 0.85)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    for tool in (NF, COOKER, SAMPLE):
        if not tool.exists():
            raise SystemExit(f"missing tool: {tool} (run build_nf.bat first)")

    out = Path(args.out) if args.out else (Path(os.path.expanduser("~")) / "Documents" / "MedievalVillage")
    if out.exists():
        if not args.force:
            raise SystemExit(f"project already exists at '{out}' (pass --force to rebuild)")
        shutil.rmtree(out)

    tmp = Path(tempfile.mkdtemp(prefix="mv_layout_"))
    run([str(SAMPLE), "--export-layout", str(tmp)])
    spawn, wagon, lamps, placed = parse_layout(tmp / "layout.txt")
    manifest = parse_manifest(KIT_CONTENT / "MedievalKit.manifest")

    # 1. Scaffold.
    run([str(NF), "new", str(out), "--name", "MedievalVillage"])
    nfproj = out / "MedievalVillage.nfproj"
    content = out / "Content"
    for sub in ("Meshes", "Materials", "Scenes", "Textures"):
        (content / sub).mkdir(parents=True, exist_ok=True)
    # Fresh template scene/registry would fight ours; meshes/materials from the
    # template (cube/sphere) are kept for the player-start marker.
    for stale in (content / "Scenes").glob("*.nfscene"):
        stale.unlink()

    # 2. Kit assets + ground grid mesh.
    for mesh_file in (KIT_CONTENT / "Meshes").glob("*.nfmesh"):
        shutil.copy2(mesh_file, content / "Meshes" / mesh_file.name)
    shutil.copy2(tmp / "ground.nfmesh", content / "Meshes" / "ground.nfmesh")
    if not (content / "Meshes" / "sphere.nfmesh").exists():
        # The Default template ships only a cube; the player-start marker wants
        # a head, so borrow the sphere from the ThirdPerson template.
        sphere_src = (ENGINE_ROOT / "Templates" / "ThirdPerson" / "Content" / "Meshes" /
                      "sphere.nfmesh")
        if sphere_src.exists():
            shutil.copy2(sphere_src, content / "Meshes" / "sphere.nfmesh")
    tex_stems = set()
    for piece in manifest.values():
        tex_stems.add(piece["tex"])
        src = KIT_CONTENT / "Textures" / (piece["tex"] + ".png")
        dst = content / "Textures" / (piece["tex"] + ".png")
        if not dst.exists():
            shutil.copy2(src, dst)

    # 3. Materials.
    write_materials(content / "Materials", tex_stems)

    # 4. First cook: meshes/textures/materials get registry IDs.
    run([str(COOKER), "--all", "--project", str(nfproj),
         "--registry", "content://AssetRegistry.nfreg"])
    reg = parse_registry(content / "AssetRegistry.nfreg")

    def mesh_id(logical):
        if logical not in reg:
            raise SystemExit(f"mesh not in registry after cook: {logical}")
        return reg[logical]

    def material_for(piece_name, tex):
        if piece_name in ("Prop_MetalFence_Simple", "Prop_MetalFence_Ornament"):
            return "content://Materials/ironwork"
        return f"content://Materials/{tex}"

    # 5. Scene.
    entities = []

    def add(name, pos, rot=(0, 0, 0), scale=(1, 1, 1), mesh=None, material=None,
            static_body=False, box_half=None, plane=False):
        e = {"name": name, "pos": pos, "rot": rot, "scale": scale}
        if mesh:
            e["mesh"] = mesh
            e["material"] = material
        if static_body:
            e["static"] = True
        if box_half:
            e["box"] = box_half
        if plane:
            e["plane"] = True
        entities.append(e)

    # Establishing shot: high 3/4 view over the whole village. The top-left
    # pick ray leaves slightly upward, so the harness's empty-corner probe
    # sees sky while the centre ray lands on the plaza paths. Close enough
    # that a 1 m test cube still paints four-figure pixels.
    add("MainCamera", (0.0, 8.0, 23.0), rot=(-26.0, 0.0, 0.0),
        mesh=None, material=None)
    entities[-1]["camera"] = (58.0, 1.7777778, 0.1, 400.0)
    add("Sun", (0, 0, 0), mesh=None, material=None)
    entities[-1]["light"] = ((0.75, -0.58, -0.33), (1.0, 0.95, 0.88), 2.1)
    entities[-1]["sky"] = ((0.055, 0.195, 0.600), (0.550, 0.660, 0.800),
                           (0.135, 0.125, 0.110), (0.03, 0.03, 0.07), 1.0, 1.0)
    add("Ground", (0, 0, 0), mesh=mesh_id("content://Meshes/ground.nfmesh"),
        material="content://Materials/Ground", static_body=True, plane=True)

    for i, p in enumerate(placed):
        info = manifest.get(p["piece"])
        if info is None:
            print(f"WARNING: manifest has no {p['piece']}; skipped", flush=True)
            continue
        mesh_logical = "content://Meshes/" + Path(info["mesh_rel"]).name
        add(f"{p['piece']}_{i}", p["pos"], rot=(0.0, p["rot"], 0.0), scale=p["scale"],
            mesh=mesh_id(mesh_logical), material=material_for(p["piece"], info["tex"]))
        if p["tag"] in COLLIDABLE:
            bmin, bmax = placed_aabb(info, p)
            size = tuple(bmax[a] - bmin[a] for a in range(3))
            if min(size) > 0.0:
                center = tuple((bmin[a] + bmax[a]) * 0.5 for a in range(3))
                half = tuple(size[a] * 0.5 for a in range(3))
                add(f"{p['piece']}_{i}_Col", center, static_body=True, box_half=half)

    # Player-start marker: the villager from the game (tunic/skin/hood).
    cube = mesh_id("content://Meshes/cube.nfmesh")
    sphere = mesh_id("content://Meshes/sphere.nfmesh")
    add("PlayerStart_Body", (spawn[0], spawn[1] - 0.55 + 0.45, spawn[2]),
        scale=(0.52, 0.80, 0.34), mesh=cube, material="content://Materials/Tunic")
    add("PlayerStart_Head", (spawn[0], spawn[1] - 0.55 + 1.08, spawn[2]),
        mesh=sphere, material="content://Materials/Skin")
    add("PlayerStart_Hood", (spawn[0], spawn[1] - 0.55 + 1.14, spawn[2]),
        scale=(1.0, 0.72, 1.0), mesh=sphere, material="content://Materials/Hood")

    lines = ["# NOVAForge Scene v1", "version: 1", "name: Main",
             f"entity_count: {len(entities)}"]
    for idx, e in enumerate(entities, start=1):
        x, y, z = e["pos"]
        rx, ry, rz = e["rot"]
        sx, sy, sz = e["scale"]
        lines.append("---")
        lines.append(f"entity: {idx}:0")
        lines.append(f"  Name: {e['name']}")
        lines.append(
            f"  Transform: local({x},{y},{z}) world({x},{y},{z}) "
            f"rot({rx},{ry},{rz}) scale({sx},{sy},{sz}) parent(4294967295:0)")
        if "camera" in e:
            fov, aspect, near, far = e["camera"]
            lines.append(
                f"  Camera: fov={fov} aspect={aspect} near={near} far={far} active=true")
        if "light" in e:
            d, c, inten = e["light"]
            lines.append(
                f"  Light: type=Directional dir({d[0]},{d[1]},{d[2]}) "
                f"color({c[0]},{c[1]},{c[2]}) intensity={inten}")
        if "sky" in e:
            zn, ho, gr, cl, disk, glow = e["sky"]
            lines.append(
                f"  Sky: zenith({zn[0]},{zn[1]},{zn[2]}) "
                f"horizon({ho[0]},{ho[1]},{ho[2]}) ground({gr[0]},{gr[1]},{gr[2]}) "
                f"clear({cl[0]},{cl[1]},{cl[2]}) sun_disk={disk} sun_glow={glow} "
                f"enabled=true")
        if "mesh" in e:
            lines.append(f"  Mesh: asset_id={e['mesh']} material={e['material']}")
        if e.get("static"):
            lines.append(
                "  RigidBody: type=Static mass=0 friction=0.800000 restitution=0.100000 "
                "linear_damping=0.050000 angular_damping=0.050000 allow_sleep=true")
        if "box" in e:
            h = e["box"]
            lines.append(f"  Collider: shape=Box half({h[0]},{h[1]},{h[2]})")
        if e.get("plane"):
            lines.append("  Collider: shape=Plane normal(0,1,0)")
    (content / "Scenes" / "Main.nfscene").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"scene: {len(entities)} entities", flush=True)

    # 6. Cook the scene + package to prove it ships.
    run([str(COOKER), "--all", "--project", str(nfproj),
         "--registry", "content://AssetRegistry.nfreg"])
    run([str(NF), "build", "--project", str(nfproj)])

    shutil.rmtree(tmp, ignore_errors=True)
    print(f"\nDone: {nfproj}", flush=True)
    print("Open it: drag MedievalVillage.nfproj onto the NOVAForge launcher window,", flush=True)
    print("or run: NOVAForgeEditor.exe --project", f'"{nfproj}"', flush=True)


if __name__ == "__main__":
    main()
