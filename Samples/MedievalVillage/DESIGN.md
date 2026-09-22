# Medieval Village: Harvest Run — design notes

A third-person collection game built on NOVAForge and the Medieval Village
MegaKit[Standard]. The player brings six supply crates to the wagon in the
village square, three at a time, before dusk. This file records *why* it is
built the way it is; the header comment in `main.cpp` records *what* it
exercises.

## Asset pipeline

The kit ships 176 glTF pieces; `Tools/import_kit.sh` converts the curated set
into the engine's cooked `.nfmesh` format and writes `MedievalKit.manifest`:

```
piece <name> mesh=<rel> tex=<BaseColor stem> min=x,y,z max=x,y,z tris=<n>
```

Decisions worth keeping:

- **Bounds live in the manifest.** Kit pieces are not the size their names
  suggest (`Roof_RoundTiles_6x6` is 5.5 m wide), so the layout is driven by
  what the importer measured, never by a guess in a layout table.
- **Texture choice = piece identity, not dominant triangle.**
  `Wall_Plaster_Straight` is 80/86 wood-trim triangles, but it is a plaster
  wall — the kit names every piece after the material that defines it, so the
  importer matches material names against the piece name first and falls back
  to the dominant one. Two overrides (metal has no BaseColor map; two doors
  prefer wood).
- **One material instance per texture**, not per piece: 72 pieces share 8
  materials, which keeps the draw-call count and the material library small.

## Village layout (`Village.cpp`)

- **Anchor on measured bounds, not the origin.** Pieces are placed so their
  bounding-box centre in X/Z lands on the target and their bbox *minimum* Y
  lands on the ground. This makes the layout independent of each exporter's
  pivot choice (the plaster wall's origin is 11 cm off its own centre).
- **Rotation convention is load-bearing.** `rotate_y` here matches
  `scene::compose_trs_mat4` (`R = Ry*Rx*Rz`, row-vector application). The
  layout maths and the renderer must agree or every wall is placed for one
  rotation and drawn with another.
- Four houses face a plaza; plaster in front, brick behind, so the village
  reads as a place with a history. Floors are sunk by their own thickness so
  the *surface* is exactly the physics ground plane at y = 0. Vines are
  anchored by their bbox top (their local box runs below the origin).

## Physics

Real `physics::CharacterController` on a `physics::PhysicsWorld`; every
Structure/Fence/Wagon/Crate/Door gets a static box collider generated from its
placed world AABB. Doors drop their collider while moving so a swinging plank
cannot shove the player. The player visual is a sphere of exactly the
controller's radius — what you see is what collides.

## UI

`NF/UI` splits retained widgets from draw backends, but a *shipped game* had
no backend. `UiOverlay` is that backend: `ui::GameFlow/Hud/Menu` →
`ui::DisplayList` → ImGui draw list → RHI. ImGui provides glyph rasterisation
and batched 2D submission only; the screens and state machine are the engine's.
Arabic DisplayList text is logical-order UTF-8, so shaping happens in the
backend (`ui::shape_arabic`) and the Amiri glyphs are merged into the same
atlas as the Latin font.

## Autoplay verification

`--autoplay` drives the same wish vector the keys would — no teleports — so a
run exercises the real controller end to end. Because the village has walls
and no pathfinder, the bot uses:

- **Whisker steering** (`steer_around`): probe points 0.7 m and 1.5 m ahead of
  each candidate direction against the blocking pieces' world AABBs (Y-filtered
  so roofs don't steer), preferring the direction closest to the one wanted.
  Same-side sweeps so house walls are *rounded*, not zigzagged against.
- **E from 2.0 m, not 1.1 m**: a crate's own collider stops the player ~1.2 m
  from its centre on a diagonal approach; waiting to close further means
  leaning on the crate forever without ever pressing E.
- **Crate blacklist** after 8 s of no progress, with a full reset if everything
  ends up blacklisted.
- **Menu driving**: the flow boots to `MainMenu`; autoplay sends `Confirm` on
  MainMenu/GameOver the same way a keypress would. Without this an automated
  run sits on the main menu forever.

A successful run is `frames > 0 && geometry drawn && 0 RHI validation errors &&
0 kit failures && UI frames > 0 && (reached_goal in autoplay)`.

## Shutdown ordering

No explicit `device->shutdown()` at the end of `run_game`: the device is
declared *first* among GPU users, so it is destroyed *last* at function exit,
after the kit's textures, the mesh library and the frame resources have
released their Vulkan objects. Calling `shutdown()` while those locals are
still alive produces a several-hundred-object leak report.
