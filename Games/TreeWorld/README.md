# TreeWorld — a playable SANAD Engine demo

A small third-person game made **with SANAD Engine** (not HTML): walk a low-poly
explorer through a misty forest and collect five fireflies.

![TreeWorld](Screenshot.png)

## Run

From the engine repository root:

```bat
build\Release\bin\nf.exe build --project Games\TreeWorld\TreeWorld.nfproj
build\Release\bin\nf.exe run   --project Games\TreeWorld\TreeWorld.nfproj
```

Headless check:

```bat
build\Release\bin\nf.exe run --project Games\TreeWorld\TreeWorld.nfproj --frames 120 --headless --validation
```

## Controls

| Input | Action |
|:---|:---|
| `WASD` / arrow keys | walk |
| touch a firefly | collect it (print score) |
| collect all five | `ALL FIREFLIES FOUND` |

## What is in the scene

- 40 entities, authored as plain text in `Content/Scenes/Main.nfscene`.
- 8 trees (a leaning tapered trunk + a canopy of four crowns), 3 rocks,
  bushes, a stone path and five emissive fireflies.
- **One rigged character.** The explorer is a single entity with a rig: a
  glTF that carries a skin, 19 bones and three clips (idle, walk, run). The
  engine skins it and plays the clip that matches how fast it is moving.
- **A camera that follows.** `Camera: ... follow=Player pivot=0,1.45,0
  distance=5.2 yaw=180 pitch=-13 smoothing=9 radius=0.35` — the camera
  orbits the character, eases after it, and pulls in front of a tree
  instead of ending up inside one.
- Daylight sky, cascaded sun shadows, distance fog, ACES tonemap, bloom,
  colour grading, vignette.
- Procedural bark and leaf textures under `Content/Textures`; all materials are
  text `.nfmat` files.

## Where the assets come from

The character and the props are **authored as code**, not modelled in a DCC
tool, so they are reproducible, reviewable as code, and carry no
third-party licence:

- `Tools/Character/make_character.py` writes
  `Content/Characters/Explorer.gltf` (and its `.bin`).
- `Tools/Props/make_props.py` writes `Content/Meshes/TreeTrunk.gltf`,
  `TreeCanopy.gltf` and `Rock.gltf`.

Re-run them after changing the shape of a bone, a tree or a rock:

```bat
python Tools\Character\make_character.py
python Tools\Props\make_props.py
```

## Gameplay script

`Content/Scripts/explorer.lua` uses the native Lua API:

- `nf.axis("move_x"/"move_z")` for input,
- `nf.entity_pos` / `nf.set_entity_pos` to move the body and place the head,
  hair, arms and legs around it,
- `nf.find_entity` + `nf.set_entity_scale` to collect fireflies,
- `print` for the score/win log.

The character is six sibling entities rather than a parent chain, because the
scene loader's hierarchy guard is strict; the script keeps the parts together.

## Style note

This is deliberately **low-poly**: the engine ships primitive meshes and
procedural textures, not a scanned forest asset pack. The target was a coherent,
playable, atmospheric game made entirely from engine-authored data. A photoreal
forest needs imported meshes/textures, which the same asset pipeline supports.