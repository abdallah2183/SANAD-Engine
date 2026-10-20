#!/usr/bin/env python3
"""make_props.py - author the SANAD demo props: a tree and a rock.

The forest used to be a cube and three spheres. It is now geometry: a
tapered trunk with three branches, a canopy of three overlapping crowns,
and a faceted rock.

    python Tools/Props/make_props.py

writes Games/TreeWorld/Content/Meshes/TreeTrunk.gltf,
TreeCanopy.gltf and Rock.gltf (each with its .bin beside it).

Like the character, the props are written here rather than modelled in a
DCC tool, so the assets are reproducible, reviewable as code, and carry
no third-party licence: they are ours, and they are CC0 like the rest of
the repository.

The shapes:

* The trunk is an eight-sided prism that tapers and leans, with three
  branches. Eight sides is enough to read as a trunk from a distance and
  costs almost nothing.
* The canopy is three overlapping crowns at different heights, each an
  icosahedron flattened a little, so the silhouette is not a ball.
* The rock is a sphere whose vertices are pushed in and out by a fixed
  hash, flat-shaded, so no two faces read the same.
"""

import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Character"))
import make_character as mk  # noqa: E402  (the matrix and buffer helpers)


def hash_noise(*values):
    """A deterministic pseudo-random number in [-1, 1]."""
    h = 2166136261
    for v in values:
        h = (h ^ (int(abs(v) * 1000.0) & 0xFFFFFFFF)) & 0xFFFFFFFF
        h = (h * 16777619) & 0xFFFFFFFF
    return ((h >> 8) & 0xFFFF) / 32767.5 - 1.0


class Mesh:
    """Positions, normals, uvs and indices."""

    def __init__(self):
        self.positions = []
        self.normals = []
        self.uvs = []
        self.indices = []

    def vertex(self, p, n, uv):
        self.positions.append(p)
        self.normals.append(n)
        self.uvs.append(uv)
        return len(self.positions) - 1

    def triangle(self, a, b, c):
        self.indices.extend([a, b, c])


def add_tube(mesh, start, end, r_start, r_end, sides=8, twist=0.0):
    """A tapered prism from `start` to `end`."""
    axis = (end[0] - start[0], end[1] - start[1], end[2] - start[2])
    length = math.sqrt(sum(c * c for c in axis))
    if length < 1e-6:
        return
    axis = (axis[0] / length, axis[1] / length, axis[2] / length)
    helper = (0.0, 1.0, 0.0) if abs(axis[1]) < 0.9 else (1.0, 0.0, 0.0)
    u = (axis[1] * helper[2] - axis[2] * helper[1],
         axis[2] * helper[0] - axis[0] * helper[2],
         axis[0] * helper[1] - axis[1] * helper[0])
    un = math.sqrt(sum(c * c for c in u))
    u = (u[0] / un, u[1] / un, u[2] / un)
    v = (axis[1] * u[2] - axis[2] * u[1],
         axis[2] * u[0] - axis[0] * u[2],
         axis[0] * u[1] - axis[1] * u[0])

    rings = []
    for r in range(2):
        t = float(r)
        centre = (start[0] + (end[0] - start[0]) * t,
                  start[1] + (end[1] - start[1]) * t,
                  start[2] + (end[2] - start[2]) * t)
        radius = r_start + (r_end - r_start) * t
        ring = []
        for s in range(sides):
            a = 2.0 * math.pi * s / sides + twist
            ca, sa = math.cos(a), math.sin(a)
            nx = u[0] * ca + v[0] * sa
            ny = u[1] * ca + v[1] * sa
            nz = u[2] * ca + v[2] * sa
            p = (centre[0] + nx * radius, centre[1] + ny * radius, centre[2] + nz * radius)
            ring.append(mesh.vertex(p, (nx, ny, nz), (s / sides, t)))
        rings.append(ring)
    for s in range(sides):
        s2 = (s + 1) % sides
        mesh.triangle(rings[0][s], rings[0][s2], rings[1][s2])
        mesh.triangle(rings[0][s], rings[1][s2], rings[1][s])


def add_blob(mesh, centre, radius, squash=1.0, seed=0, facets=1):
    """A faceted ball: an icosahedron whose vertices are pushed in and out."""
    t = (1.0 + 5.0 ** 0.5) / 2.0
    verts = [
        (-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0),
        (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
        (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1),
    ]
    faces = [
        (0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11),
        (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6), (7, 1, 8),
        (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9),
        (4, 9, 5), (2, 4, 11), (6, 2, 10), (8, 6, 7), (9, 8, 1),
    ]
    # One subdivision: an icosahedron is too coarse to read as a crown.
    if facets > 1:
        new_faces = []
        cache = {}

        def midpoint(a, b):
            key = (min(a, b), max(a, b))
            if key in cache:
                return cache[key]
            pa, pb = verts[a], verts[b]
            m = [(pa[0] + pb[0]) * 0.5, (pa[1] + pb[1]) * 0.5, (pa[2] + pb[2]) * 0.5]
            verts.append(m)
            cache[key] = len(verts) - 1
            return cache[key]

        for a, b, c in faces:
            ab = midpoint(a, b)
            bc = midpoint(b, c)
            ca = midpoint(c, a)
            new_faces.extend([(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)])
        faces = new_faces

    base = len(mesh.positions)
    for i, v in enumerate(verts):
        n = math.sqrt(sum(c * c for c in v))
        unit = (v[0] / n, v[1] / n, v[2] / n)
        r = radius * (1.0 + 0.22 * hash_noise(seed, i))
        p = (centre[0] + unit[0] * r,
             centre[1] + unit[1] * r * squash,
             centre[2] + unit[2] * r)
        mesh.vertex(p, unit, (0.5 + 0.5 * unit[0], 0.5 + 0.5 * unit[2]))
    for a, b, c in faces:
        mesh.triangle(base + a, base + b, base + c)


def build_trunk():
    mesh = Mesh()
    # The trunk leans a little: a perfectly vertical trunk reads as a pipe.
    add_tube(mesh, (0.0, 0.0, 0.0), (0.10, 1.6, -0.06), 0.30, 0.19)
    add_tube(mesh, (0.10, 1.6, -0.06), (0.16, 3.1, -0.10), 0.19, 0.10)
    # Three branches, at different heights and different directions.
    add_tube(mesh, (0.11, 1.9, -0.07), (0.78, 2.45, 0.18), 0.10, 0.05)
    add_tube(mesh, (0.12, 2.3, -0.08), (-0.52, 2.85, -0.30), 0.09, 0.045)
    add_tube(mesh, (0.13, 2.7, -0.09), (0.48, 3.25, -0.62), 0.08, 0.04)
    return mesh


def build_canopy():
    mesh = Mesh()
    # Three crowns at different heights: a single ball reads as a lollipop.
    add_blob(mesh, (0.10, 3.55, -0.08), 1.05, squash=0.82, seed=1)
    add_blob(mesh, (-0.62, 3.05, -0.42), 0.78, squash=0.85, seed=2)
    add_blob(mesh, (0.78, 3.05, 0.30), 0.72, squash=0.85, seed=3)
    add_blob(mesh, (0.25, 4.15, -0.35), 0.62, squash=0.80, seed=4)
    return mesh


def build_rock():
    mesh = Mesh()
    add_blob(mesh, (0.0, 0.30, 0.0), 0.52, squash=0.72, seed=7, facets=1)
    add_blob(mesh, (0.30, 0.18, 0.22), 0.32, squash=0.70, seed=8, facets=1)
    return mesh


def write_gltf(path, name, mesh, base_color, roughness):
    buf = mk.Buffer()
    pos_acc = buf.accessor(buf.view(mk.f32s([c for p in mesh.positions for c in p])), 5126,
                            len(mesh.positions), "VEC3")
    nrm_acc = buf.accessor(buf.view(mk.f32s([c for n in mesh.normals for c in n])), 5126,
                            len(mesh.normals), "VEC3")
    uv_acc = buf.accessor(buf.view(mk.f32s([c for t in mesh.uvs for c in t])), 5126,
                           len(mesh.uvs), "VEC2")
    idx_acc = buf.accessor(buf.view(mk.u16s(mesh.indices)), 5123, len(mesh.indices), "SCALAR")

    gltf = {
        "asset": {"version": "2.0", "generator": "SANAD make_props.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{
            "name": name,
            "primitives": [{
                "attributes": {"POSITION": pos_acc, "NORMAL": nrm_acc, "TEXCOORD_0": uv_acc},
                "indices": idx_acc,
                "material": 0,
                "mode": 4,
            }],
        }],
        "materials": [{
            "name": name + "Material",
            "pbrMetallicRoughness": {"baseColorFactor": list(base_color) + [1.0],
                                     "metallicFactor": 0.0,
                                     "roughnessFactor": roughness},
        }],
        "buffers": [{"byteLength": len(buf.data), "uri": os.path.basename(path).replace(".gltf", ".bin")}],
        "bufferViews": buf.views,
        "accessors": buf.accessors,
    }

    bin_path = path.replace(".gltf", ".bin")
    with open(bin_path, "wb") as f:
        f.write(buf.data)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(gltf, f, separators=(",", ":"))
        f.write("\n")
    print("wrote %s (%d verts, %d tris)" % (path, len(mesh.positions), len(mesh.indices) // 3))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.normpath(os.path.join(here, "..", "..", "Games", "TreeWorld", "Content",
                                             "Meshes"))
    os.makedirs(out_dir, exist_ok=True)
    write_gltf(os.path.join(out_dir, "TreeTrunk.gltf"), "TreeTrunk", build_trunk(),
               (0.42, 0.30, 0.20), 0.85)
    write_gltf(os.path.join(out_dir, "TreeCanopy.gltf"), "TreeCanopy", build_canopy(),
               (0.18, 0.34, 0.16), 0.90)
    write_gltf(os.path.join(out_dir, "Rock.gltf"), "Rock", build_rock(),
               (0.42, 0.42, 0.44), 0.80)


if __name__ == "__main__":
    sys.exit(main())
