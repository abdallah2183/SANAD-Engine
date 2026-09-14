# Phase 10 — Scripting + Save System

**Status**: **COMPLETE 2026-09-14** — W1–W6 delivered; suite 515 passed / 0 failed / 1 skipped across 13 suites (was 439/0/1 across 11)
**Design doc reference**: §70 (Save System), §71–74 (Scripting Architecture, Reflection, Serialization, Versioned Serialization), §249 (Scripting Roadmap S0–S4)

## Motivation

The design doc (§252) places Scripting and Save in v0.2 alongside Physics/Audio/Animation. Phases 5–9 delivered everything else in v0.2 (editor, prefab, physics, audio, animation, project/build/ship). The two remaining v0.2 items are:

- **Scripting** (§249 S0–S1): C++ gameplay module hooks + reflection metadata. Without this, gameplay logic is ad-hoc and not data-driven.
- **Save System** (§70): Save slots, autosave, versioned serialization, migration, async save, cloud hooks.

The design doc's S0 is "C++ gameplay module" — a structured way to author gameplay in C++ with lifecycle hooks (init, update, shutdown). S1 is "Reflection" — compile-time/runtime metadata so properties can be discovered, edited in the inspector, and serialized without hand-written boilerplate. S2–S4 (C#/Lua, hot reload, visual scripting) are explicitly later.

## Scope

### In scope (S0 + S1 + Save)
1. **Gameplay module system** — `GameplayModule` base class with `on_init`, `on_update`, `on_shutdown`, `on_scene_load`, `on_scene_unload`. Registered modules are owned by `Runtime`. A module can query ECS, spawn/despawn entities, read input actions, drive physics/audio/animation. This is the C++ scripting layer — no external VM, no code generation beyond reflection.
2. **Reflection metadata** — `PROPERTY(...)`, `CLASS(...)` macros that generate `PropertyInfo` / `ClassInfo` records. No codegen step required (unlike Unreal's UHT); the macros expand to inline metadata + a `static_meta()` method. Supports: `EditAnywhere`, `Replicated` (forward-looking, no-ops for now), `SerializeField`, `Category`. Type info covers `f32`, `i32`, `bool`, `std::string`, `Vec3`, `Quat`, entity refs, and arrays of these.
3. **Inspector integration** — the editor's `InspectorPanel` can discover reflected properties on any component that opts in, and render appropriate widgets (float drag, int drag, bool checkbox, vec3 drag, string input, enum dropdown). This replaces the per-component hand-written panels for reflected types.
4. **Save system** — `SaveSystem` with: named save slots (a slot = a directory under `saves://`), autosave trigger, async save (dispatched on the job system), versioned save format (engine version + schema version + migration hooks), save/load of the full scene state + gameplay state. Save format is a directory of line-based text files (consistent with the engine's `.nfscene` / `.nfproj` conventions — no binary format yet, no JSON parser exists).
5. **Scene serialization extension** — serialize gameplay module state (registered modules + their reflected properties) as part of the scene file. Load restores modules + state.

### Explicitly out of scope
- **S2**: C# or Lua VM bindings — future phase.
- **S3**: Hot reload of scripting — future phase.
- **S4**: Visual scripting — future phase.
- **Binary save format** — text now, binary later when the text format is proven.
- **Cloud save hooks** — interface defined, not implemented.
- **Save migration for old engine versions** — the version field exists; migration is a stub.
- **Replication** — the `Replicated` property tag exists in metadata but does nothing yet (networking is v0.4).

## Work Breakdown

### W1 — Reflection core (Engine/Core)
- `PropertyInfo` struct: name, type enum, offset, flags (EditAnywhere, SerializeField, Replicated, Category string).
- `ClassInfo` struct: name, array of `PropertyInfo`, parent `ClassInfo*`, `static_meta()` function pointer.
- `NF_PROPERTY(name, ...)`, `NF_CLASS(...)` macros — expand to inline metadata + `static_meta()`.
- `ReflectionRegistry` singleton: register `ClassInfo` at static-init, query by name.
- Type enum: `Float`, `Int`, `Bool`, `String`, `Vec3`, `Quat`, `EntityRef`, `Enum`, `Array`.
- `enum` reflection: `NF_ENUM(name, underlying)` macro that registers variants.
- Tests: property discovery, offset correctness, enum variants, category filtering, parent inheritance.

**As built.** The macros are `NF_CLASS` / `NF_PROPERTY` / `NF_PROPERTY_ENUM` / `NF_CLASS_END` (plus
`NF_CLASS_END_DERIVED` for inheritance) and `NF_ENUM_BEGIN` / `NF_ENUM_VALUE` / `NF_ENUM_END`.
`NF_CLASS` opens a static function whose body holds the property table, which puts the entries in a
**complete-class context** — that is what makes `offsetof(Type, member)` legal inside the class body,
and it is why a declaration is one macro line next to the member rather than a separate out-of-line
block. `nf_class_meta()` returns a pointer (matching `ReflectionRegistry::find_class`) and the static
registrar calls it, so including the header is enough to make a type discoverable.

Two things are deliberately absent:
- **No `clear()` on the registry.** Registration is one-shot during static initialisation and cannot
  be re-run, so a reset would permanently disable reflection for the rest of the process — a test that
  used it would silently break every test after it.
- **No second euler convention.** `Quat::from_euler` existed, was never called, and was wrong: it
  returned the zero quaternion for `(0,0,0)`, which is not a rotation. It was **removed** rather than
  repaired, because `NF/Scene/Transform.hpp` already has a tested forward/inverse pair
  (`quat_from_euler_xyz_degrees` / `euler_xyz_degrees_from_quat`). Two conventions is how this project
  ended up with two incompatible `.nfmesh` formats.

The text marshaller (`property_to_string` / `property_from_string`) prints floats at **9 significant
digits**, the shortest width that round-trips every `f32` exactly. `%.6g` was the first attempt and it
lost the low bits, which would have shown up as drift after a save/load cycle. `EntityRef` is a weak
`u32` id and prints as `none` when unset. An unregistered enum variant prints as its number so the
value still round-trips.

15 tests in `Tests/CoreTests/test_reflection.cpp` (the module lives in NFCore, so the tests do too —
there is no separate `ReflectionTests` executable).

### W2 — Gameplay module system (Engine/Runtime → new Engine/Gameplay)
- `GameplayModule` base: virtual `on_init`, `on_update(f32)`, `on_shutdown`, `on_scene_load`, `on_scene_unload`. Has a `name()` and an `id()`.
- `GameplayModuleRegistry`: static registry (name → factory function). `NF_GAMEPLAY_MODULE(name)` macro for registration.
- `Runtime` owns a `std::vector<unique_ptr<GameplayModule>>`. `init()` instantiates registered modules; `update()` calls `on_update`; `shutdown()` calls `on_shutdown`.
- Modules can access: `world()` (ECS), `physics()`, `audio()`, `input()`, `scene()`.
- `GameplayModuleComponent` ECS struct: holds module id + serialized state (key-value map of reflected properties).
- Tests: module lifecycle, registry, update order, scene load/unload hooks, state save/restore.

**As built.** `Engine/Gameplay` (`NFGameplay`) depends only on `NFCore`; the Runtime owns modules, so
there is no dependency back into `Engine/Runtime`. Modules reach the engine through
`GameplayContext` (world, scene, physics, audio, input, dt, frame, scene_version), and every pointer
in it may be null — a headless run has no input source and a scene may have no physics.

Three decisions worth recording:
- **A module's reflected state is a separate plain struct**, not the module class. `offsetof` is not
  usable on a non-standard-layout type, and a `GameplayModule` has virtuals. `GameplayModule::state()`
  returns a `GameplayStateBinding { instance, meta }` pointing at that struct.
- **`initialized` is tracked per module, not per set.** A single "have we initialised yet" flag would
  silently skip `on_init` for a module added after the first scene load — which is exactly the
  "registered but never driven" failure this phase exists to prevent.
- **Gameplay runs after physics/animation/audio and before transform propagation.** A module therefore
  observes where the frame actually put things, and an entity it repositions renders at the new place
  in the same frame. A module that wants to steer *physics* writes velocities, which take effect on the
  next fixed step.

18 tests in `Tests/GameplayTests/test_gameplay.cpp`, including five that drive a probe module through
`Runtime::update()` — the layer that decides whether a module runs at all.

### W3 — Inspector integration (Editor)
- `InspectorPanel` discovers `ClassInfo` for a component type. If found, auto-generates widgets for `EditAnywhere` properties. Falls back to the existing hand-written panels for types that haven't opted in.
- Per-type widget dispatch: `f32` → drag, `i32` → drag, `bool` → checkbox, `std::string` → input text, `Vec3` → 3-component drag, `Quat` → euler drag (degrees), `EntityRef` → entity picker.
- "Add Gameplay Module" dropdown in the inspector — lists all registered modules, adds a `GameplayModuleComponent`.
- Tests: reflected panel renders correct property types, enum dropdown shows all variants, category headers group properties, non-EditAnywhere properties are hidden.

**As built.** Split in two, because `NFEditorCore` is deliberately window-free and RHI-free:
- `NF/Editor/ReflectedInspector.hpp` — `ReflectedObjectView`, which turns a reflected object into
  labelled fields with a widget kind each plus category groups. All the *policy* lives here and is
  covered by EditorTests.
- `Editor/src/ui/Panels.cpp` — a direct walk of that model. No decisions, nothing worth testing that is
  not already tested one layer down.

`Quat` edits in degrees through the scene module's existing conversion pair rather than a new one.
`EntityRef` is an explicit id box, not a click-to-pick control: a half-wired picker that silently
clears the reference is worse than an honest id field. `Unsupported` types render read-only rather than
disappearing, so a missing widget is visible instead of being mistaken for a missing property.

The inspector edits the **live module's** state, not the component's map — during a session the module
owns its state and the component is the saved snapshot, so editing the map would show a value the
module would immediately overwrite.

11 tests: 8 in `Tests/EditorTests/test_reflected_inspector.cpp`, 3 in
`Tests/EditorTests/test_gameplay_editor.cpp` (the attach/detach path through `EditorApp`).

### W4 — Save system (Engine/Runtime)
- `SaveSystem` class: `save_game(slot_name)`, `load_game(slot_name)`, `list_saves()`, `delete_save(slot_name)`, `has_save(slot_name)`.
- Save slot = a directory under `saves://<slot_name>/` containing:
  - `scene.nfscene` — the scene state (entities, components, transforms).
  - `modules.txt` — gameplay module state (one block per module, key=value lines).
  - `meta.txt` — engine version, schema version, save timestamp, scene name.
- Async save: `save_game_async(slot_name)` dispatches to the job system; returns a `JobHandle`. Save runs on a worker thread, writes to a temp dir, atomically renames on completion.
- Autosave: `set_autosave(interval_seconds, slot_prefix)`. Timer accumulates in `update()`.
- Versioned: `meta.txt` has `engine_version=X.Y.Z` and `schema_version=N`. `load_game` checks versions and calls registered migration functions if they differ.
- `save://` VFS mount (added to `ProjectDescriptor` defaults).
- Tests: save/load round-trip (entities, transforms, components, module state), async save completion, autosave trigger, list/delete, version mismatch → migration stub.

**As built, with three deviations from the sketch:**
1. **The async payload is built on the calling thread.** Serializing the live scene on a worker would
   race with the frame loop. The snapshot is cheap; the disk write is what goes to the worker. There is
   no `JobHandle` type in this codebase — `JobGroup` is the existing primitive, and
   `async_save_pending()` / `wait_for_async_save()` expose it.
2. **`modules.txt` uses one encoded `props:` line per module**, not one key per line. The values are
   user data and can contain newlines, which would let a value forge a new entry. It reuses
   `gameplay::encode_properties`, the same encoding the scene format uses, so a value means the same
   thing in both files.
3. **No `saves://`-only mount helper was needed in the end** — `ProjectDescriptor::make_default` gained
   `saves://` → `Saves`, and `SaveSystem::ensure_mount()` derives a location from the content mount
   when the descriptor did not declare one. Defaulting to the process's working directory would scatter
   saves wherever the editor happened to be launched from.

`save_game` builds into `<slot>.staging` and swaps it in with **rollback** (the old slot is moved aside
rather than deleted), so a failure between the first byte and the publish cannot cost the player the
save they already had. `load_game` reuses the scene loader rather than a second parser, and installs
the result through `Runtime::adopt_scene()`.

The migration path is real and chained (`register_migration(from, fn)` walks one version at a time). No
production migration is registered, because there is only one schema version — registering a no-op for
a version that never shipped would be dead code. The path is exercised by a test that registers one. A
save from a **newer** engine is refused outright rather than half-read.

16 tests in `Tests/SaveTests/test_save.cpp`.

**This work uncovered a pre-existing hang.** `JobGroup::add` incremented its pending counter and then
enqueued the bare task, never handing the counter to the job — and a worker only decrements a counter it
was given. `JobGroup::wait()` therefore spun forever. `JobGroup` had no test anywhere in the suite,
which is why it survived. Fixed, and pinned by `test_job_group_wait_returns`, which polls with a
deadline *before* calling `wait()` so a regression fails in ten seconds instead of hanging CI.

### W5 — Scene serialization extension (Engine/Runtime)
- Extend `RuntimeSceneLoader` to write/read `Module:` lines (module id + serialized state as inline key=value pairs).
- Extend `RuntimeSceneLoader` to write/read reflected properties on existing components (e.g., a `RigidBodyComponent` that opts into reflection will have its properties serialized automatically alongside the hand-written format).
- Tests: module round-trip, reflected property round-trip, mixed hand-written + reflected.

**As built, and one bullet deliberately deferred.** The `Module:` line is implemented:

```
  Module: name=OrbitCamera enabled=true props=radius=6|yaw_degrees=0|label=hello\swords
```

The scene format is a run of space-separated `key=value` tokens and `field_value` stops at the first
space, so module values are escaped (`\` → `\\`, `|` → `\p`, space → `\s`, newlines as `\n`). Without
that, `label=hello world` truncates to `hello` silently. The escaping lives in
`gameplay::encode_properties`, shared with the save system, so the two formats cannot drift.

**Deferred, explicitly:** migrating the existing hand-written components (`RigidBodyComponent`,
`ColliderComponent`, `AnimationComponent`, `AudioComponent`) onto the generic reflected path. The
mechanism exists and is used — the `Module:` line *is* reflected-property serialization — but pointing
it at those components would change the on-disk format of every existing scene, and having both a
hand-written block and a generic block for the same component is the duplication that produces two
sources of truth. That is a deliberate format migration for a later phase, not an oversight.

6 tests in `Tests/GameplayTests/test_gameplay_serialization.cpp`, including one that asserts the exact
on-disk line shape so a future format change has to be deliberate.

### W6 — Editor, CI, docs, README
- Editor: "Save Game" and "Load Game" menu items. Autosave toggle in settings. Save slot browser.
- Editor: sample gameplay module (`OrbitCameraModule`) that demonstrates the API — orbits a target entity, reads input actions for rotation/zoom.
- CI: assert GameplayTests and SaveTests pass.
- README: Phase 10, updated module list, test count.
- `Docs/Phase10_Plan.md`: mark complete.

**As built.** A `Game` toolbar popup carries the slot field, Save/Load, the autosave interval, and the
slot list with each slot's scene name, schema version and timestamp. Autosave ticks off the frame clock
in `ui_frame` (via a new `UiFrameStats::dt_seconds`) rather than from `Runtime::update`, which keeps the
Runtime unaware of save slots.

`OrbitCameraModule` lives in `NFEditorCore`, not in the editor executable, so EditorTests can drive it
headlessly — a sample that cannot be run is documentation, not a demonstration. It resolves its pivot,
applies input, clamps pitch and radius, and reports `placements()` / `last_target()` /
`last_camera_position()` as observables. It never uses the camera entity as its own pivot: reading a
position out of the transform it writes would feed last frame's result back in and drift.

CI gained non-zero guards for `GameplayTests` and `SaveTests`, plus a direct run of
`CoreTests.exe reflection` — the runner only prints per-suite totals, and the reflection tests are the
one guard against a property table that points at the wrong offset (a defect that compiles, links and
silently corrupts data). All four guards were validated against a real run: the reflection filter
reports 15 with the filter and 0 with a wrong one.

8 tests in `Tests/EditorTests/test_orbit_camera_module.cpp`.

## Acceptance Criteria

1. A gameplay module can be authored in C++ with `NF_GAMEPLAY_MODULE("name")`, registered, and its `on_update` is called every frame.
2. A component with `NF_PROPERTY` metadata has its properties visible and editable in the inspector without hand-written panel code.
3. `save_game("test")` writes a slot directory; `load_game("test")` restores the exact entity/component/module state.
4. Async save completes without blocking the main thread; the scene is playable during save.
5. Save version mismatch triggers the migration path (even if the migration is a no-op stub for now).
6. Full test suite green: all existing 420 tests + new GameplayTests + SaveTests + ReflectionTests.

### §5 — Acceptance, executed

Suite (build/verify, MSVC 14.51.36231, `/W4 /WX`, zero warnings):

```
PASS  CoreTests      Passed: 59 | Failed: 0 | Skipped: 0 | Total: 59
PASS  JobTests       Passed: 5  | Failed: 0 | Skipped: 0 | Total: 5
PASS  ECSTests       Passed: 25 | Failed: 0 | Skipped: 1 | Total: 26
PASS  AssetTests     Passed: 45 | Failed: 0 | Skipped: 0 | Total: 45
PASS  RHITests       Passed: 69 | Failed: 0 | Skipped: 0 | Total: 69
PASS  PhysicsTests   Passed: 86 | Failed: 0 | Skipped: 0 | Total: 86
PASS  AnimationTests Passed: 38 | Failed: 0 | Skipped: 0 | Total: 38
PASS  AudioTests     Passed: 27 | Failed: 0 | Skipped: 0 | Total: 27
PASS  GameplayTests  Passed: 24 | Failed: 0 | Skipped: 0 | Total: 24
PASS  SaveTests      Passed: 16 | Failed: 0 | Skipped: 0 | Total: 16
PASS  RuntimeTests   Passed: 33 | Failed: 0 | Skipped: 0 | Total: 33
PASS  EditorTests    Passed: 74 | Failed: 0 | Skipped: 0 | Total: 74
PASS  ToolTests      Passed: 14 | Failed: 0 | Skipped: 0 | Total: 14
TOTAL  passed=515 failed=0 skipped=1
RESULT: PASS (1 skipped — see above)
```

Criterion 6 said "all existing 420 tests"; the baseline had already grown to 439 after Phase 9's
integration pass, and is now 515. Reflection tests are criterion 1–2's coverage and live in CoreTests,
not in an executable of their own.

Phase 7 chain re-run end-to-end (`nf new` → `nf build` → `nf verify` → packaged player, no `--project`):

```
Cook report: cooked 3, skipped 0, failed 0, pruned 0, total 3
Packaged 17 file(s) (17 manifest entries), 10 shader(s)
Verify: total 3, ok 3, missing 0, bad_format 0

Runtime: physics world created with 2 bodies
Rendered 120 frames (meshes=1)
Animated entities: 1 (max transform deviation over 120 frames: 179.83662)
Audio sources mixed (peak): 1 (0.19999997)
Validation errors: 0
Alive RHI objects before shutdown: 0
=== NOVAForge Runtime exited cleanly ===          exit 0
```

Editor headless (`--project`, 30 frames, validation on): all automation checks OK, 0 validation errors,
0 leaked RHI objects, exit 0.

### Rule 0 — the negative tests, executed

Commented out `step_gameplay(dt)` in `Runtime::update` and rebuilt:

```
GameplayTests  22 passed / 2 failed / 24 total        exit 1
  runtime_instantiates_and_steps_registered_modules — probe->update_calls != 5
  runtime_gameplay_context_carries_the_scene_world  — probe->updates != 1
```

The other three runtime tests call `rt.step_gameplay(...)` directly, so they test `step_gameplay`
itself rather than the call from `update()` — the split is deliberate, and the two failures above are
the ones that catch the regression.

Reverted the `JobGroup::add` fix and rebuilt:

```
test_job_group_wait_returns — FAILED: group.pending() != 0   (after the 10 s bounded poll)
```

i.e. the test fails instead of hanging. Restored; suite back to 515/0/1.

## Risks

- **Reflection macro complexity**: C++ macro-based reflection is fragile across compilers. The macros must work under MSVC `/W4 /WX`. Mitigation: keep macros simple, test on MSVC first (it's the only target right now), avoid non-standard extensions.
- **Save format churn**: the text-based save format will evolve. Mitigation: version field from day one; migration stubs registered but not yet needed.
- **Module update order**: gameplay modules may depend on each other. Mitigation: explicit `update_priority` field (f32, default 0, lower first); document that cross-module calls should use late-bound queries, not direct pointers.
- **Editor inspector regression**: the existing hand-written panels must keep working for types that don't opt into reflection. Mitigation: fallback path in `InspectorPanel`, tested.

## Non-goals (revisited)

- No C#/Lua VM. No hot reload. No visual scripting. No binary save. No cloud. No replication. No networking.
- These are all v0.3+ scope per the design doc.
