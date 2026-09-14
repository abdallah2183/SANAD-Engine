# Phase 10 — Scripting + Save System

**Status**: planned 2026-09-14
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

### W2 — Gameplay module system (Engine/Runtime → new Engine/Gameplay)
- `GameplayModule` base: virtual `on_init`, `on_update(f32)`, `on_shutdown`, `on_scene_load`, `on_scene_unload`. Has a `name()` and an `id()`.
- `GameplayModuleRegistry`: static registry (name → factory function). `NF_GAMEPLAY_MODULE(name)` macro for registration.
- `Runtime` owns a `std::vector<unique_ptr<GameplayModule>>`. `init()` instantiates registered modules; `update()` calls `on_update`; `shutdown()` calls `on_shutdown`.
- Modules can access: `world()` (ECS), `physics()`, `audio()`, `input()`, `scene()`.
- `GameplayModuleComponent` ECS struct: holds module id + serialized state (key-value map of reflected properties).
- Tests: module lifecycle, registry, update order, scene load/unload hooks, state save/restore.

### W3 — Inspector integration (Editor)
- `InspectorPanel` discovers `ClassInfo` for a component type. If found, auto-generates widgets for `EditAnywhere` properties. Falls back to the existing hand-written panels for types that haven't opted in.
- Per-type widget dispatch: `f32` → drag, `i32` → drag, `bool` → checkbox, `std::string` → input text, `Vec3` → 3-component drag, `Quat` → euler drag (degrees), `EntityRef` → entity picker.
- "Add Gameplay Module" dropdown in the inspector — lists all registered modules, adds a `GameplayModuleComponent`.
- Tests: reflected panel renders correct property types, enum dropdown shows all variants, category headers group properties, non-EditAnywhere properties are hidden.

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

### W5 — Scene serialization extension (Engine/Runtime)
- Extend `RuntimeSceneLoader` to write/read `Module:` lines (module id + serialized state as inline key=value pairs).
- Extend `RuntimeSceneLoader` to write/read reflected properties on existing components (e.g., a `RigidBodyComponent` that opts into reflection will have its properties serialized automatically alongside the hand-written format).
- Tests: module round-trip, reflected property round-trip, mixed hand-written + reflected.

### W6 — Editor, CI, docs, README
- Editor: "Save Game" and "Load Game" menu items. Autosave toggle in settings. Save slot browser.
- Editor: sample gameplay module (`OrbitCameraModule`) that demonstrates the API — orbits a target entity, reads input actions for rotation/zoom.
- CI: assert GameplayTests and SaveTests pass.
- README: Phase 10, updated module list, test count.
- `Docs/Phase10_Plan.md`: mark complete.

## Acceptance Criteria

1. A gameplay module can be authored in C++ with `NF_GAMEPLAY_MODULE("name")`, registered, and its `on_update` is called every frame.
2. A component with `NF_PROPERTY` metadata has its properties visible and editable in the inspector without hand-written panel code.
3. `save_game("test")` writes a slot directory; `load_game("test")` restores the exact entity/component/module state.
4. Async save completes without blocking the main thread; the scene is playable during save.
5. Save version mismatch triggers the migration path (even if the migration is a no-op stub for now).
6. Full test suite green: all existing 420 tests + new GameplayTests + SaveTests + ReflectionTests.

## Risks

- **Reflection macro complexity**: C++ macro-based reflection is fragile across compilers. The macros must work under MSVC `/W4 /WX`. Mitigation: keep macros simple, test on MSVC first (it's the only target right now), avoid non-standard extensions.
- **Save format churn**: the text-based save format will evolve. Mitigation: version field from day one; migration stubs registered but not yet needed.
- **Module update order**: gameplay modules may depend on each other. Mitigation: explicit `update_priority` field (f32, default 0, lower first); document that cross-module calls should use late-bound queries, not direct pointers.
- **Editor inspector regression**: the existing hand-written panels must keep working for types that don't opt into reflection. Mitigation: fallback path in `InspectorPanel`, tested.

## Non-goals (revisited)

- No C#/Lua VM. No hot reload. No visual scripting. No binary save. No cloud. No replication. No networking.
- These are all v0.3+ scope per the design doc.
