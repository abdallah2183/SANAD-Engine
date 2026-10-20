#!/usr/bin/env python3
"""make_character.py - author the SANAD demo character as a rigged glTF.

The character is a low-poly humanoid: a continuous skinned mesh, a
19-bone skeleton, and three clips (idle, walk, run). It is written here
rather than modelled in a DCC tool so that the asset is reproducible, is
reviewable as code, and carries no third-party licence: the file this
script produces is ours, and it is CC0 like the rest of the repository.

    python Tools/Character/make_character.py

writes Games/TreeWorld/Content/Characters/Explorer.gltf and the .bin beside
it. The glTF is the shape the engine's importer accepts: one skinned mesh
with JOINTS_0/WEIGHTS_0, a skin with inverse bind matrices, LINEAR
translation/rotation channels, and a Principled material.

The maths:

* The body is a set of tapered tubes, one per bone, each an eight-sided
  prism from the bone's head to its child's head. The radius per bone is
  what makes it read as a body rather than as a stack of boxes.
* Every vertex is bound to the bone that owns its tube, and to the two
  bones that meet at the tube's ends: the weight falls off linearly over
  the last 30% of the tube, so the surface bends at the joint instead of
  creasing.
* The inverse bind matrices are the rest pose's world transforms,
  inverted. A skeleton in its rest pose therefore skins to the identity,
  which is the property the engine's own tests rely on.
* The clips are rotation channels on the bones (plus a vertical bob on
  the hips), sampled at 15 keys per cycle and interpolated LINEAR.
"""

import base64
import json
import math
import os
import struct
import sys

# --- the rig ----------------------------------------------------------------
#
# name, parent, head position (x, y, z), radius at the head, radius at the
# tail. A bone with no children ends in a cap.
#
# The character stands 1.75 m tall with the feet on the ground.

BONES = [
    # name        parent  head                r_head r_tail
    ("Hips",      None,   (0.00, 0.95, 0.00), 0.17, 0.15),
    ("Spine",     "Hips", (0.00, 1.08, 0.00), 0.15, 0.14),
    ("Chest",     "Spine", (0.00, 1.30, 0.00), 0.16, 0.13),
    ("Neck",      "Chest", (0.00, 1.50, 0.00), 0.06, 0.06),
    ("Head",      "Neck", (0.00, 1.56, 0.00), 0.11, 0.07),
    ("ShoulderL", "Chest", (0.08, 1.46, 0.00), 0.07, 0.06),
    ("UpperArmL", "ShoulderL", (0.19, 1.44, 0.00), 0.06, 0.05),
    ("LowerArmL", "UpperArmL", (0.21, 1.16, 0.00), 0.05, 0.04),
    ("HandL",     "LowerArmL", (0.21, 0.92, 0.00), 0.045, 0.03),
    ("ShoulderR", "Chest", (-0.08, 1.46, 0.00), 0.07, 0.06),
    ("UpperArmR", "ShoulderR", (-0.19, 1.44, 0.00), 0.06, 0.05),
    ("LowerArmR", "UpperArmR", (-0.21, 1.16, 0.00), 0.05, 0.04),
    ("HandR",     "LowerArmR", (-0.21, 0.92, 0.00), 0.045, 0.03),
    ("UpperLegL", "Hips", (0.09, 0.90, 0.00), 0.08, 0.06),
    ("LowerLegL", "UpperLegL", (0.09, 0.50, 0.00), 0.06, 0.05),
    ("FootL",     "LowerLegL", (0.09, 0.09, 0.04), 0.05, 0.04),
    ("UpperLegR", "Hips", (-0.09, 0.90, 0.00), 0.08, 0.06),
    ("LowerLegR", "UpperLegR", (-0.09, 0.50, 0.00), 0.06, 0.05),
    ("FootR",     "LowerLegR", (-0.09, 0.09, 0.04), 0.05, 0.04),
]

SIDES = 8  # an octagonal cross-section: not a box, not a sphere


def bone_index(name):
    for i, b in enumerate(BONES):
        if b[0] == name:
            return i
    raise KeyError(name)


def children_of(name):
    return [b[0] for b in BONES if b[1] == name]


def matrix_from_trs(t, r, s):
    """A ROW-MAJOR 4x4 matrix, the way the engine stores them.

    The engine's Mat4 is row-major (m[row][col]) and it multiplies a
    vector from the left (v * M). glTF stores its matrices column-major,
    so the conversion happens once, where the inverse bind matrices are
    written out -- not here, where the rig is built.

    Getting this wrong is silent: the rig still loads, the skin still
    binds, and the character is bent out of the world.
    """
    x, y, z, w = r
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    rot = [
        [1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy)],
        [2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx)],
        [2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy)],
    ]
    out = [0.0] * 16
    for row in range(3):
        for col in range(3):
            out[row * 4 + col] = rot[row][col] * s[col]
    out[0 * 4 + 3] = t[0]
    out[1 * 4 + 3] = t[1]
    out[2 * 4 + 3] = t[2]
    out[3 * 4 + 3] = 1.0
    return out


def mat_mul(a, b):
    """The row-major matrix product."""
    out = [0.0] * 16
    for row in range(4):
        for col in range(4):
            acc = 0.0
            for k in range(4):
                acc += a[row * 4 + k] * b[k * 4 + col]
            out[row * 4 + col] = acc
    return out


def mat_inverse(m):
    """The inverse of a row-major 4x4 matrix."""
    a = [[m[row * 4 + col] for col in range(4)] for row in range(4)]
    inv = [[0.0] * 4 for _ in range(4)]
    for i in range(4):
        for j in range(4):
            minor = [[a[r][c] for c in range(4) if c != j] for r in range(4) if r != i]
            det = (minor[0][0] * minor[1][1] * minor[2][2]
                   - minor[0][0] * minor[1][2] * minor[2][1]
                   - minor[0][1] * minor[1][0] * minor[2][2]
                   + minor[0][1] * minor[1][2] * minor[2][0]
                   + minor[0][2] * minor[1][0] * minor[2][1]
                   - minor[0][2] * minor[1][1] * minor[2][0])
            sign = 1 if (i + j) % 2 == 0 else -1
            inv[j][i] = sign * det
    out = [0.0] * 16
    for row in range(4):
        for col in range(4):
            out[row * 4 + col] = inv[row][col]
    return out


def to_column_major(m):
    """A row-major matrix as glTF's column-major array."""
    return [m[col * 4 + row] for col in range(4) for row in range(4)]


def rest_world_matrices():
    """The world transform of every bone in the rest pose."""
    world = {}
    for name, parent, head, _, _ in BONES:
        # A glTF node's translation is LOCAL to its parent, so the local
        # translation is the offset from the parent's head. Writing the
        # absolute position here would make the rest pose accumulate the
        # positions -- and the inverse bind matrices, which are the inverse
        # of the rest pose, would bend the character out of the world.
        local_x, local_y, local_z = head
        if parent is not None:
            px, py, pz = BONES[bone_index(parent)][2]
            local_x, local_y, local_z = head[0] - px, head[1] - py, head[2] - pz
        local = matrix_from_trs((local_x, local_y, local_z), (0, 0, 0, 1), (1, 1, 1))
        world[name] = local if parent is None else mat_mul(world[parent], local)
    return world


def quat_axis_angle(axis, degrees):
    a = math.radians(degrees) * 0.5
    s = math.sin(a)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(a))


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def quat_norm(q):
    n = math.sqrt(q[0] ** 2 + q[1] ** 2 + q[2] ** 2 + q[3] ** 2)
    if n < 1e-9:
        return (0.0, 0.0, 0.0, 1.0)
    return (q[0] / n, q[1] / n, q[2] / n, q[3] / n)


# --- the geometry ------------------------------------------------------------


def build_mesh():
    """The skinned mesh: one tapered tube per bone, plus the head cap.

    Returns (positions, normals, uvs, joints, weights, indices).
    """
    positions = []
    normals = []
    uvs = []
    joints = []
    weights = []
    indices = []

    def add_vertex(p, n, uv, joint_ids, joint_weights):
        positions.append(p)
        normals.append(n)
        uvs.append(uv)
        # Four joint slots, as glTF requires.
        ids = list(joint_ids) + [0] * (4 - len(joint_ids))
        ws = list(joint_weights) + [0.0] * (4 - len(joint_weights))
        joints.append(ids)
        weights.append(ws)

    for bi, (name, parent, head, r_head, r_tail) in enumerate(BONES):
        kids = children_of(name)
        if not kids:
            continue
        # The tube runs from this bone's head to its first child's head. A
        # bone with several children (the chest) gets one tube per child.
        for kid in kids:
            ki = bone_index(kid)
            tail = BONES[ki][2]
            direction = (tail[0] - head[0], tail[1] - head[1], tail[2] - head[2])
            length = math.sqrt(sum(c * c for c in direction))
            if length < 1e-6:
                continue
            axis = (direction[0] / length, direction[1] / length, direction[2] / length)
            # Two perpendicular vectors for the cross-section.
            helper = (0.0, 1.0, 0.0) if abs(axis[1]) < 0.9 else (1.0, 0.0, 0.0)
            u = (axis[1] * helper[2] - axis[2] * helper[1],
                 axis[2] * helper[0] - axis[0] * helper[2],
                 axis[0] * helper[1] - axis[1] * helper[0])
            un = math.sqrt(sum(c * c for c in u))
            u = (u[0] / un, u[1] / un, u[2] / un)
            v = (axis[1] * u[2] - axis[2] * u[1],
                 axis[2] * u[0] - axis[0] * u[2],
                 axis[0] * u[1] - axis[1] * u[0])

            rings = 3
            ring_base = []
            for r in range(rings):
                t = r / (rings - 1)
                centre = (head[0] + direction[0] * t, head[1] + direction[1] * t,
                          head[2] + direction[2] * t)
                radius = r_head + (r_tail - r_head) * t
                # The weight falls off towards the ends, so the surface bends
                # at the joints instead of creasing.
                blend = 0.35
                w_self = 1.0
                others = []
                if r == 0 and parent is not None:
                    w_self = 1.0 - blend
                    others.append((bone_index(parent), blend))
                if r == rings - 1:
                    w_self = 1.0 - blend
                    others.append((ki, blend))
                ids = [bi] + [o[0] for o in others]
                ws = [w_self] + [o[1] for o in others]
                ring = []
                for s in range(SIDES):
                    a = 2.0 * math.pi * s / SIDES
                    ca, sa = math.cos(a), math.sin(a)
                    nx = u[0] * ca + v[0] * sa
                    ny = u[1] * ca + v[1] * sa
                    nz = u[2] * ca + v[2] * sa
                    p = (centre[0] + nx * radius, centre[1] + ny * radius, centre[2] + nz * radius)
                    add_vertex(p, (nx, ny, nz), (s / SIDES, t), ids, ws)
                    ring.append(len(positions) - 1)
                ring_base.append(ring)

            for r in range(rings - 1):
                for s in range(SIDES):
                    s2 = (s + 1) % SIDES
                    a = ring_base[r][s]
                    b = ring_base[r][s2]
                    c = ring_base[r + 1][s2]
                    d = ring_base[r + 1][s]
                    indices.extend([a, b, c, a, c, d])

    # The head: an octahedron-ish cap, so the character has a head and not
    # a stick.
    head_pos = BONES[bone_index("Head")][2]
    head_bone = bone_index("Head")
    head_r = 0.115
    for s in range(SIDES):
        a = 2.0 * math.pi * s / SIDES
        ca, sa = math.cos(a), math.sin(a)
        p = (head_pos[0] + ca * head_r, head_pos[1], head_pos[2] + sa * head_r)
        add_vertex(p, (ca * 0.4, 0.9, sa * 0.4), (s / SIDES, 1.0), [head_bone], [1.0])
    top = len(positions)
    add_vertex((head_pos[0], head_pos[1] + 0.13, head_pos[2]), (0, 1, 0), (0.5, 1.0),
               [head_bone], [1.0])
    for s in range(SIDES):
        s2 = (s + 1) % SIDES
        indices.extend([s, s2, top])

    return positions, normals, uvs, joints, weights, indices


# --- the clips ---------------------------------------------------------------
#
# Each clip is a list of (bone, path, keys) where keys is a list of
# (time, value). Rotations are quaternions (x, y, z, w).

def clip_idle():
    """A slow breathing sway: the chest rises, the arms drift."""
    keys = []
    duration = 4.0
    steps = 9
    for i in range(steps):
        t = duration * i / (steps - 1)
        phase = 2.0 * math.pi * t / duration
        keys.append(("Chest", "rotation",
                     (t, quat_norm(quat_axis_angle((0.0, 0.0, 1.0), 2.0 * math.sin(phase))))))
        keys.append(("Spine", "rotation",
                     (t, quat_norm(quat_axis_angle((0.0, 0.0, 1.0), -1.2 * math.sin(phase))))))
        keys.append(("Hips", "translation",
                     (t, (0.0, 0.95 + 0.012 * math.sin(phase), 0.0))))
        keys.append(("UpperArmL", "rotation",
                     (t, quat_norm(quat_axis_angle((0.0, 0.0, 1.0), 4.0 * math.sin(phase))))))
        keys.append(("UpperArmR", "rotation",
                     (t, quat_norm(quat_axis_angle((0.0, 0.0, 1.0), -4.0 * math.sin(phase))))))
        keys.append(("Head", "rotation",
                     (t, quat_norm(quat_axis_angle((1.0, 0.0, 0.0), 3.0 * math.sin(phase))))))
    return "Idle", duration, keys


def _gait(duration, leg_swing, arm_swing, bob, knee_bend):
    """A walk/run cycle: the legs and arms swing in opposition."""
    keys = []
    steps = 13
    for i in range(steps):
        t = duration * i / (steps - 1)
        phase = 2.0 * math.pi * t / duration
        s = math.sin(phase)
        c = math.cos(phase)
        # Hips: the bob is twice the stride frequency (down on each step).
        keys.append(("Hips", "translation", (t, (0.0, 0.95 + bob * (1.0 - c) * 0.5, 0.0))))
        keys.append(("Hips", "rotation",
                     (t, quat_norm(quat_axis_angle((0.0, 1.0, 0.0), 3.0 * s)))))
        keys.append(("Spine", "rotation",
                     (t, quat_norm(quat_axis_angle((0.0, 0.0, 1.0), -2.0 * s)))))
        keys.append(("Chest", "rotation",
                     (t, quat_norm(quat_axis_angle((0.0, 0.0, 1.0), 4.0 * s)))))
        # The legs: the left swings forward with the right.
        keys.append(("UpperLegL", "rotation",
                     (t, quat_norm(quat_axis_angle((1.0, 0.0, 0.0), leg_swing * s)))))
        keys.append(("UpperLegR", "rotation",
                     (t, quat_norm(quat_axis_angle((1.0, 0.0, 0.0), -leg_swing * s)))))
        # The knee bends only backwards, and most when the leg is behind.
        keys.append(("LowerLegL", "rotation",
                     (t, quat_norm(quat_axis_angle((1.0, 0.0, 0.0), knee_bend * max(0.0, -s))))))
        keys.append(("LowerLegR", "rotation",
                     (t, quat_norm(quat_axis_angle((1.0, 0.0, 0.0), knee_bend * max(0.0, s))))))
        # The arms swing in opposition to the legs.
        keys.append(("UpperArmL", "rotation",
                     (t, quat_norm(quat_axis_angle((1.0, 0.0, 0.0), -arm_swing * s)))))
        keys.append(("UpperArmR", "rotation",
                     (t, quat_norm(quat_axis_angle((1.0, 0.0, 0.0), arm_swing * s)))))
        keys.append(("LowerArmL", "rotation",
                     (t, quat_norm(quat_axis_angle((1.0, 0.0, 0.0), -12.0 - 10.0 * max(0.0, s))))))
        keys.append(("LowerArmR", "rotation",
                     (t, quat_norm(quat_axis_angle((1.0, 0.0, 0.0), -12.0 - 10.0 * max(0.0, -s))))))
        keys.append(("Head", "rotation",
                     (t, quat_norm(quat_axis_angle((1.0, 0.0, 0.0), -2.0 * s)))))
    return duration, keys


def clip_walk():
    duration, keys = _gait(1.0, 26.0, 18.0, 0.035, 42.0)
    return "Walk", duration, keys


def clip_run():
    duration, keys = _gait(0.58, 42.0, 30.0, 0.075, 70.0)
    return "Run", duration, keys


# --- the writer --------------------------------------------------------------


class Buffer:
    def __init__(self):
        self.data = bytearray()
        self.views = []
        self.accessors = []

    def view(self, payload):
        offset = len(self.data)
        self.data.extend(payload)
        self.views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(payload)})
        return len(self.views) - 1

    def accessor(self, view, component_type, count, type_name, normalized=False):
        acc = {"bufferView": view, "componentType": component_type, "count": count,
               "type": type_name}
        if normalized:
            acc["normalized"] = True
        self.accessors.append(acc)
        return len(self.accessors) - 1


def f32s(values):
    return struct.pack("<%df" % len(values), *values)


def u16s(values):
    return struct.pack("<%dH" % len(values), *values)


def main():
    positions, normals, uvs, joints, weights, indices = build_mesh()
    world = rest_world_matrices()
    # The inverse bind matrices are the inverse of the rest pose, in the
    # ENGINE's row-major layout; the file stores them column-major, so the
    # conversion happens here.
    inverse_bind = [to_column_major(mat_inverse(world[name])) for name, _, _, _, _ in BONES]

    buf = Buffer()

    pos_acc = buf.accessor(buf.view(f32s([c for p in positions for c in p])), 5126,
                            len(positions), "VEC3")
    nrm_acc = buf.accessor(buf.view(f32s([c for n in normals for c in n])), 5126,
                            len(normals), "VEC3")
    uv_acc = buf.accessor(buf.view(f32s([c for t in uvs for c in t])), 5126,
                           len(uvs), "VEC2")
    joint_acc = buf.accessor(buf.view(u16s([j for row in joints for j in row])), 5123,
                             len(joints), "VEC4")
    # Weights as normalised u16, the way Blender writes them.
    weight_values = []
    for row in weights:
        total = sum(row) or 1.0
        for w in row:
            weight_values.append(int(round(65535.0 * w / total)))
    weight_acc = buf.accessor(buf.view(u16s(weight_values)), 5123, len(weights), "VEC4",
                              normalized=True)
    index_acc = buf.accessor(buf.view(u16s(indices)), 5123, len(indices), "SCALAR")
    ibm_acc = buf.accessor(buf.view(f32s([c for m in inverse_bind for c in m])), 5126,
                            len(BONES), "MAT4")

    animations = []
    for name, duration, keys in (clip_idle(), clip_walk(), clip_run()):
        samplers = []
        channels = []
        # One sampler per (bone, path): the keys are merged per channel.
        merged = {}
        for bone, path, key in keys:
            merged.setdefault((bone, path), []).append(key)
        for (bone, path), key_list in merged.items():
            key_list.sort(key=lambda k: k[0])
            times = [k[0] for k in key_list]
            values = []
            for k in key_list:
                v = k[1]
                values.extend(v if isinstance(v, tuple) else (v,))
            if path == "rotation":
                values_acc = buf.accessor(buf.view(f32s(values)), 5126, len(times), "VEC4")
            elif path == "translation":
                values_acc = buf.accessor(buf.view(f32s(values)), 5126, len(times), "VEC3")
            else:
                values_acc = buf.accessor(buf.view(f32s(values)), 5126, len(times), "VEC3")
            times_acc = buf.accessor(buf.view(f32s(times)), 5126, len(times), "SCALAR")
            samplers.append({"input": times_acc, "output": values_acc,
                             "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1,
                             "target": {"node": bone_index(bone), "path": path}})
        animations.append({"name": name, "samplers": samplers, "channels": channels})

    nodes = []
    for i, (name, parent, head, _, _) in enumerate(BONES):
        node = {"name": name}
        # The translation is LOCAL to the parent (a glTF node's transform is
        # relative to its parent), so it is the offset from the parent's head.
        lx, ly, lz = head
        if parent is not None:
            px, py, pz = BONES[bone_index(parent)][2]
            lx, ly, lz = head[0] - px, head[1] - py, head[2] - pz
        node["translation"] = [lx, ly, lz]
        if parent is not None:
            node["children"] = []
        nodes.append(node)
    for i, (name, parent, head, _, _) in enumerate(BONES):
        if parent is not None:
            nodes[bone_index(parent)].setdefault("children", []).append(i)

    # The mesh node carries the skin. It is a SCENE ROOT beside the joint
    # hierarchy, not a child of the hips: a skinned mesh's own transform must
    # stay the identity (the skinning does the placing), and a scene root may
    # not have a parent -- the parser refuses a document whose root is
    # parented.
    mesh_node = len(nodes)
    nodes.append({"name": "CharacterMesh", "mesh": 0, "skin": 0})

    gltf = {
        "asset": {"version": "2.0", "generator": "SANAD make_character.py"},
        "scene": 0,
        "scenes": [{"nodes": [mesh_node, bone_index("Hips")]}],
        "nodes": nodes,
        "meshes": [{
            "name": "Explorer",
            "primitives": [{
                "attributes": {"POSITION": pos_acc, "NORMAL": nrm_acc, "TEXCOORD_0": uv_acc,
                               "JOINTS_0": joint_acc, "WEIGHTS_0": weight_acc},
                "indices": index_acc,
                "material": 0,
                "mode": 4,
            }],
        }],
        "materials": [{
            "name": "ExplorerSkin",
            "pbrMetallicRoughness": {"baseColorFactor": [0.82, 0.62, 0.48, 1.0],
                                     "metallicFactor": 0.0, "roughnessFactor": 0.65},
        }],
        "skins": [{
            "name": "ExplorerRig",
            "skeleton": bone_index("Hips"),
            "joints": list(range(len(BONES))),
            "inverseBindMatrices": ibm_acc,
        }],
        "animations": animations,
        "buffers": [{"byteLength": len(buf.data),
                     "uri": "Explorer.bin"}],
        "bufferViews": buf.views,
        "accessors": buf.accessors,
    }

    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.normpath(os.path.join(here, "..", "..", "Games", "TreeWorld",
                                             "Content", "Characters"))
    os.makedirs(out_dir, exist_ok=True)
    gltf_path = os.path.join(out_dir, "Explorer.gltf")
    bin_path = os.path.join(out_dir, "Explorer.bin")
    with open(bin_path, "wb") as f:
        f.write(buf.data)
    with open(gltf_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(gltf, f, separators=(",", ":"))
        f.write("\n")
    print("wrote %s (%d vertices, %d triangles, %d bones)" %
          (gltf_path, len(positions), len(indices) // 3, len(BONES)))
    print("wrote %s (%d bytes)" % (bin_path, len(buf.data)))


if __name__ == "__main__":
    sys.exit(main())
