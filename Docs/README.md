# SANAD Engine — documentation index

This folder holds three different kinds of document, and they are not
interchangeable. This file says which is which, because the phase numbers in
the file names look like a sequence and are not one.

## The three kinds

| Kind | Where | What it is |
|:--|:--|:--|
| **Design** | [`SANAD_Engine_Complete_Design.md`](SANAD_Engine_Complete_Design.md) (209 KB) | The complete engine design, in Arabic, numbered by section (`§14`, `§59`, `§71`, `§81`, `§206`, …). The code refers to these section numbers, so they are stable references. It describes the shape of the whole engine, not the state of any one release. |
| **Roadmap** | [`../ROADMAP.md`](../ROADMAP.md) (85 KB, repo root) | The phase-by-phase history, in Arabic: what each phase was for, what it shipped, what it deferred. It lives at the repo root because it is about the project, not about one subsystem. |
| **Plan** | `Phase*_Plan.md` (this folder) | A per-milestone work plan: the motivation, the scope, the work items, the acceptance criteria — written *before* the phase was implemented. It is a historical record of intent, not a description of the current code. |

A plan document is the one that goes stale. When the code and a plan disagree,
**the code is right** and the plan is a snapshot of what was decided at the
time. Do not treat a plan as a specification to implement; read it to find out
why something was built the way it was.

## The phase documents that exist

| Document | Phase | Subject |
|:--|:--|:--|
| [`Phase7_Plan.md`](Phase7_Plan.md) | 7 | Project, build, standalone run — the `nf` CLI and the packaged player. |
| [`Phase8_Plan.md`](Phase8_Plan.md) | 8 | Physics and deterministic simulation (the own solver that Jolt later replaced in part). |
| [`Phase9_Plan.md`](Phase9_Plan.md) | 9 | Animation and audio, built together. |
| [`Phase9_Verification_2026-09-14.md`](Phase9_Verification_2026-09-14.md) | 9 | A dated verification pass over phase 9. |
| [`Phase10_Plan.md`](Phase10_Plan.md) | 10 | Scripting and the save system (Lua sandbox, save slots). |
| [`Phase11_Plan.md`](Phase11_Plan.md) | 11 | Foundation: break the layering inversions and pay down the debt (the layering rules CI enforces today). |
| [`Phase12_Plan.md`](Phase12_Plan.md) | 12 | Complete world streaming. |
| [`Phase13_Plan.md`](Phase13_Plan.md) | 13 | Directional shadows and the procedural sky. |
| [`Phase27_Plan.md`](Phase27_Plan.md) | 27 | The post-processing stack (design §206). |
| [`Phase28_Plan.md`](Phase28_Plan.md) | 28 | Navigation in the frame, the scene and the editor (NavMesh authoring). |
| [`Phase29_Plan.md`](Phase29_Plan.md) | 29 | Jolt vehicles in the frame, the scene and the editor (Step 11). |

### Why the numbers jump

The plan file names are **historical labels, not a sequence to renumber**:

- **Phases 1–6** predate the plan-document convention. Their scope lives in
  the design document and in the roadmap's earlier sections.
- **Phases 14–26** were planned and tracked in [`../ROADMAP.md`](../ROADMAP.md)
  — which lists every one of them — but their individual plan files were not
  kept in the tree. The gap in the file names is a gap in what was archived,
  **not a gap in the work**: those phases shipped.
- **Phases 27–29** are the recent ones, and each has a plan file because each
  was a large enough milestone to warrant one.

So: for *what a phase did*, read the roadmap. For *why it was designed that
way*, read the plan file when one exists. For *how the engine is shaped*,
read the design document.

## Guides (user-facing)

| Document | About |
|:--|:--|
| [`AI_Game_Guide.md`](AI_Game_Guide.md) | The executable recipe for driving the engine with an AI agent: create a `.nfproj`, edit a `.nfscene` and a Lua script, then `nf run`. |
| [`Getting_Started_Ar.md`](Getting_Started_Ar.md) | Arabic getting-started guide. |
| [`Tutorial_Ar.md`](Tutorial_Ar.md) | Arabic 20-minute tutorial. |
| [`Lua_Scripting_Ar.md`](Lua_Scripting_Ar.md) | Arabic Lua scripting reference. |
| [`Asset_Import.md`](Asset_Import.md) | Importing glTF/GLB/OBJ/STL/PLY and `.nfmesh` assets. |
| [`Blender_Pipeline.md`](Blender_Pipeline.md) | The shipped Blender add-on and its exporter settings. |
| [`GPU_Profiling.md`](GPU_Profiling.md) | Real Vulkan timestamp queries: the two renderer toggles, runtime switch, GPU-aware dynamic resolution (fresh-sample gate, CPU fallback, wall-time cooldown), query layout, one-frame latency, overhead, and tests. |
| [`Frame_Pacing.md`](Frame_Pacing.md) | Rolling-window frame telemetry: raw wall time, average/1% low/jitter, the CPU/GPU bound rule, editor rows, `NFPlayer --frame-stats`, and limits. |
| [`Combat_Ballistics.md`](Combat_Ballistics.md) | Time-of-flight projectiles (speed, gravity, range-derived lifetime, swept segments), factions with friendly-fire filtering, faction-aware auto-targeting, projectile lead for moving targets, and per-frame impact events for effects: scene keys, runtime rules, editor sections, tests, and limits. |
| [`Combat_Damage.md`](Combat_Damage.md) | Layered damage (hit zones, armor, penetration, per-type resistance), radial blasts, frame-local shot events for tracers/muzzle flashes, and damage-over-time statuses (burn/bleed/corrode): formulas, scene keys, event fields, editor fields, tests, and declared limits. |
| [`Gamepad_Checklist.md`](Gamepad_Checklist.md) | Gamepad support checklist. |
| [`Demo_Release.md`](Demo_Release.md) | What the 0.2.0 demo release is, and what it is not — including that it is **Windows only** (no Linux/macOS build yet). |

## Reviews (dated snapshots)

| Document | About |
|:--|:--|
| [`Engine_Evaluation_2026-09-13.md`](Engine_Evaluation_2026-09-13.md) | Engine evaluation, 2026-09-13. |
| [`Full_Project_Review_2026-09-13.md`](Full_Project_Review_2026-09-13.md) | Full-project review, 2026-09-13. |

Dated reviews are point-in-time assessments. They are kept because they
record what was true on that date; they are not a to-do list.

## Media

`images/` (README and documentation stills), `media/` (the demo video),
`launcher/` and `assets/` (the Arabic launcher shell and its assets).
`index.html` renders this documentation set as a browsable site.
