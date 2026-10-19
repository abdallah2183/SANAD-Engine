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

- 63 entities, authored as plain text in `Content/Scenes/Main.nfscene`.
- 8 trees (trunk + three canopy layers), 3 rocks, bushes, a stone path, five
  emissive fireflies, and a six-part primitive character.
- Daylight sky, cascaded sun shadows, distance fog, ACES tonemap, bloom,
  colour grading, vignette.
- Procedural bark and leaf textures under `Content/Textures`; all materials are
  text `.nfmat` files.

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