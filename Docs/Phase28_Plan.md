# Phase 28 — Navigation in the frame, the scene and the editor

The last open item of Phase 22 was `NavMesh/Recast`. The voxel pipeline landed
with its own suite (`AITests`: 19 tests over voxelisation, walkable
classification, regions, polygon cover, links, pathing and determinism) — and
nothing reached it: no scene could name a volume, no frame stepped an agent, no
editor authored either. This phase closes that gap the same way Phase 25 closed
the particle and cloth gap: **the scene declares, the frame steps, the editor
authors** — no game code touches the navmesh API.

## The two new scene lines

```
NavMesh: area(12,8,12) cell=0.5 cell_height=0.25 slope=45 climb=0.5
         headroom=2 min_area=2 agent_radius=0 jump_distance=4 jump_height=1.5
         max_verts=6 enabled=true
NavAgent: speed=3 goal(10,0,10) arrive=0.25 enabled=true
```

* **Placement is the entity's Transform, not a field.** The origin IS the
  volume's minimum corner; the sizes run along +X/+Y/+Z from it. A level author
  drags the volume like any other object, and the floor it bakes is
  `Transform.y` — no second copy of the corner exists to drift from the gizmo.
* Every key is optional and an impossible one rejects the whole line with a
  warning naming the key (the contract every component line in `.nfscene` keeps).
* The lines are written only when the component exists, so a scene that never
  heard of navigation round-trips byte for byte.

## What the bake reads from the scene — and what it refuses to invent

The volume's own floor height is the terrain sampler. There is no Terrain
component in `.nfscene`, so a sampler that invented hills would describe a world
the physics does not simulate. A richer sampler is a caller's extension — `build`
takes any `HeightSampler` — not a scene-format change.

The obstacles are the scene's **static box colliders**:

| Refused | Why |
| :--- | :--- |
| A dynamic body | Simulated by the engine solver every frame; an obstacle that moves needs a re-bake to mean anything. |
| A sphere/plane | The voxeliser is AABB-based, and a sphere baked as its box is a wall wider than the thing an author placed and can see. |
| An entity carrying a NavAgent | An agent is not its own wall — its collider would carve a hole exactly where it stands. |

## The frame

`build_scene_navmesh()` runs on scene adopt; `step_navmesh()` runs after
`step_script` and `step_ai_world`, before `step_particles` — an agent sees this
frame's simulation and AI plan, and writes its Transform before propagation, so
the walk renders where it ends up this frame. `dt = 0` (the editor's edit freeze)
holds the crowd posed, exactly like every other mover.

Re-pathing is triggered by four things, and only four: no path, the goal moved,
the mesh was re-baked, or the agent left its corridor (an editor drag, a script
teleport). Walking along a path never reads as "left the corridor" — the check is
a distance-to-segment against the leg being walked, not a distance to the goal.

## Three defects the wiring exposed in `NavMesh` itself

1. **`find_path` pinned the raw endpoints onto the smoothed chain without a
   line-of-sight check.** The straight line between the raw start and the raw
   goal can thread a hole the corridor walked around — the detour test's wall
   was crossed outright. The chain is now built from **portals**: the midpoints of
   the shared edges between consecutive polygons, both ends of an off-mesh link,
   and the raw endpoints. Every surviving leg is an edge that was actually tested
   against the mesh, which is what makes "straight" and "on the mesh" the same
   property rather than two competing ones.
2. **`locate` could not see a point ON a polygon's boundary.** The crossing test
   is built from strict comparisons, so a point on an edge — and every portal of
   a path is exactly such a point — fell through to "outside". A one-millimetre
   boundary slop (documented as a slop, not a walkable extension) fixes it.
3. **`raycast` refused a corner-touching transition.** A rectangle cover meets at
   corners wherever two edges cross, and a corridor that rounds such a corner
   passes through exactly that shared point — which is on the mesh. Point contact
   is now accepted alongside edge contact.

## The editor

`attach/set/detach_navmesh` and `attach/set/detach_nav_agent` in `EditorApp`,
validated at the door with the same bounds the loader enforces (a range that
differed between the two would let the UI write a component the file then
refuses), two inspector sections, two Add-Component menu entries, EN/AR keys, and
live port stats (`NavMesh polygons`, `N agents at goal`) that read the bake rather
than the components — a component count cannot tell an unbaked volume from a
working one.

## Result (measured 2026-10-10)

`AITests` 129/129 (20 navmesh), `RuntimeTests` 120/120 (`test_runtime_nav_agents.cpp`
×9), `EditorTests` 265/265 (`test_nav_editor.cpp` ×4) — clean `/W4 /WX` build.
Full sweep: **26 suites, 1849 passed / 1 failed / 2 skipped**.

The one failure, `forward_pass_renders_an_opaque_surface_like_the_deferred_path`,
is pre-existing at `HEAD`: the transparency pass samples the blurred SSAO at the
fragment's screen position and reads 1.0 there, while the lighting pass reads the
occluded value at the same coordinate of the same texture — a divergence that
predates this phase (the SSAO stage landed after the test's threshold was
calibrated). What this phase DID fix in that area:

* the forward pass sampled the SSAO buffer with the **surface's own uv**, so a
  pane covering a quarter of the screen read the occlusion of whatever happened
  to sit at that quarter of the texture — it was lit by a stranger's shadow;
* the transparency pass did not declare the blurred SSAO as a graph read, a
  read-after-write hazard the graph was free to schedule the wrong way round.

Both are covered by `transparent_pane_blends_over_opaque_geometry`, which now
passes (it failed before).

## Deferred on purpose

* A height sampler from real terrain: there is no Terrain component to sample.
* Per-agent crowd steering on the navmesh: `AIWorld`'s budget decides *which*
  actors think; this phase decided *where they walk*. Composing them is design,
  not wiring.
* Live path debugging in the viewport: the mesh is a per-scene artefact baked on
  adopt; a debug draw is a renderer feature, not a scene one.
