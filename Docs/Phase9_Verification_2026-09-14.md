# Phase 9 (Animation + Audio) — Independent Verification

**Verified:** 2026-09-14
**Verifier:** Nova (independent rebuild, not a re-read of the phase's own claims)
**Tree:** `C:/Users/abdal/WorkBuddy/Worktrees/NOVAForge Engine/main-e34f0fa2` (uncommitted Phase 7–9 work)
**Verdict at time of verification:** **Module layer green and trustworthy. Integration layer missing. Phase 9 is not complete.**

> **Status update — 2026-09-14, later the same day.** The integration gap described below has been
> closed; see [§7](#7-closure-2026-09-14). The findings in §2 are preserved as written because they
> are the record of what the phase actually shipped on its first pass, and because §2.6 (the suite
> could not detect any of it) is the reason the new tests are shaped the way they are. **Phase 9 is
> now complete**: 439/0/1, and the packaged player drives an animated, audible entity.

---

## 1. What was reproduced

Rebuilt from scratch into a throwaway `build/verify` (no reuse of the existing `build/debug`):

```
CMake configure → OK   (MSVC 14.51.36231 / VS 18 Community, Windows SDK 10.0.26100.0, Vulkan 1.4.357.0)
Build           → 229/229 targets, 0 errors   (/W4 /WX, per CMake/NFCompilerFlags.cmake)
```

Full suite via `bash Scripts/run_tests.sh build/verify`:

```
PASS  CoreTests      Passed: 44  | Failed: 0 | Skipped: 0
PASS  JobTests       Passed:  4  | Failed: 0 | Skipped: 0
PASS  ECSTests       Passed: 25  | Failed: 0 | Skipped: 1
PASS  AssetTests     Passed: 45  | Failed: 0 | Skipped: 0
PASS  RHITests       Passed: 69  | Failed: 0 | Skipped: 0
PASS  PhysicsTests   Passed: 86  | Failed: 0 | Skipped: 0
PASS  AnimationTests Passed: 31  | Failed: 0 | Skipped: 0
PASS  AudioTests     Passed: 20  | Failed: 0 | Skipped: 0
PASS  RuntimeTests   Passed: 28  | Failed: 0 | Skipped: 0
PASS  EditorTests    Passed: 54  | Failed: 0 | Skipped: 0
PASS  ToolTests      Passed: 14  | Failed: 0 | Skipped: 0
TOTAL passed=420 failed=0 skipped=1
```

**The `420 / 0 / 1` claim in `Docs/Phase9_Plan.md` §4 and the README is honest.** Reproduced
exactly, from a clean tree.

Phase 7 chain still green — packaged player, run from inside `dist/` with no `--project`:

```
nf new → nf build → dist/NFPlayer.exe --frames 120 --validation
Rendered 120 frames (meshes=1) · Validation errors: 0 · Alive RHI objects before shutdown: 0 · exit 0
```

The animation and audio suites are also *honest* tests: 31 + 20 real `NF_CHECK`/`NF_CHECK_NEAR`
assertions, zero silent early-returns, no `require_gpu()` escapes. The bug they caught during
development (Mat4 row-vector TRS order, equal-power panning at 0.707) were real bugs, correctly
diagnosed and fixed. The module code itself is good work.

---

## 2. Why the phase is still not done

`Runtime::update(dt)` is the definition of what runs in a shipped game. It is exactly:

```cpp
void Runtime::update(float dt) {
    m_manager.update();
    sync_meshes_from_assets();
    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) return;
    step_physics(dt);
    auto& world = m_scene_data_ptr->scene->world();
    scene::propagate_transforms(world);
}
```

Four steps. No animation step. No audio step. Everything below follows from that.

### 2.1 Animation never runs

`AnimationComponent` in a scene is inert. Nothing advances `AnimationPlayer`, nothing evaluates the
state machine, no pose is ever computed. `grep -rn "AnimationPlayer\|AnimationComponent"` outside
`Engine/Animation/` returns only `RuntimeSceneLoader.cpp` (load/save) and the tests.

The component's own header documents the missing behaviour:

```cpp
/// ECS component for skeletal animation. ... The runtime steps
/// the player and/or state machine each frame and writes the world-space pose
/// to the entity's Transform hierarchy.
struct AnimationComponent {
    ...
    std::vector<LocalPose> last_local_pose;
    std::vector<WorldPose> last_world_pose;   // "for the render path to consume"
};
```

`last_local_pose` and `last_world_pose` are **written by nobody in the entire repository**. They are
declared output fields with no producer. A scene can carry animation data end to end and produce
zero motion.

### 2.2 No skinning, no render hookup

`Docs/Phase9_Plan.md` §7 says "CPU skinning only; the render path gets the final pose as a mesh
override." Nothing outside `Engine/Animation/` mentions skeleton, bone, pose, or a mesh override.
Even if a pose were computed, there is no path from it to the renderer.

### 2.3 Audio never runs

Identical: no `AudioComponent` step in `Runtime::update`. The only device implementation is
`NullAudioDevice`, so audio today is a pure math library (`compute_attenuation`,
`compute_3d_pan_gain`) plus an ECS struct. The plan lists the MiniAudio backend as out of scope,
which is fair — but combined with the missing runtime step, no audio code executes in a game.

### 2.4 No editor panels

`grep -rn "Animation\|Audio" Editor/ --include=*.cpp --include=*.hpp` → **zero hits.**
`Editor/src/ui/Panels.cpp` covers Transform, Mesh, Camera, Directional Light, RigidBody, Collider,
PrefabLink. `Docs/Phase9_Plan.md` §2 promises "Editor inspector panels for both components."
Compare Phase 8, which does have `#include <NF/Physics/Components.hpp>` and live RigidBody/Collider
panels — the pattern to follow already exists in the file.

### 2.5 The phase's own §5 acceptance is unimplemented

`Docs/Phase9_Plan.md` §5 states:

```bash
nf new Demo --name Demo          # the template scene gains an animated entity
nf build --project Demo/Demo.nfproj
cd Demo/dist && ./NFPlayer --frames 120 --validation
# expect: exit 0, the animated entity's transforms have changed, 0 validation errors, 0 leaks
```

Run literally, the chain passes — but only because it still tests Phase 8. The scaffolded scene is
byte-for-byte the Phase 8 template:

```
5 entities — Camera, Directional Light, Mesh, static RigidBody+Plane, dynamic RigidBody+Box
grep "Animation\|Audio" Main.nfscene  →  no matches
```

And `Tools/Player/main.cpp` has no animation assertion of any kind. The acceptance cannot fail,
because the thing it is supposed to check was never added.

### 2.6 The suite cannot catch any of this

`Tests/RuntimeTests/test_runtime_scene.cpp` asserts save/load **round-trip** of component fields
(`clip_name`, `speed`, `buffer_name`, `volume`, `looping`). It never asserts that anything advances
them. So a green 420 and a completely unwired animation system are perfectly compatible — which is
exactly the situation.

---

## 3. What this is not

- Not a fake-green. Nothing here is a test that lies; the tests are real and pass for the right
  reasons. The suite simply measures the module layer, and the module layer is finished.
- Not a design failure. The plan's architecture (pure sampling, seam-based backend, headless-safe
  device) is sound and the modules match it.
- It is a **stopping point**: the phase was closed on unit tests before the wiring described in its
  own §2 and §5 was done.

---

## 4. Required to close Phase 9

In dependency order:

1. **`Runtime::step_animation(dt)`** — advance each `AnimationComponent`'s player (or state machine
   when `use_state_machine`), write `last_local_pose` / `last_world_pose`, and apply the root bone
   to the entity's `scene::Transform` so `propagate_transforms` picks it up. Call it from
   `Runtime::update` after `step_physics`.
2. **`Runtime::step_audio(dt)`** — advance sources on the `AudioBus`, update the listener from the
   active camera, mix through `AudioDevice`. `NullAudioDevice` keeps CI green.
3. **Editor panels** for `AnimationComponent` and `AudioComponent`, following the existing physics
   panel pattern in `Panels.cpp` (clip dropdown, speed, loop mode, play/pause, state-machine
   toggle; buffer, volume, pitch, looping, spatial, autoplay).
4. **Template scene gains an animated entity** (`Templates/Default/Content/Scenes/Main.nfscene`)
   with an `Animation:` block, plus a minimal clip asset so it has something to sample.
5. **Player acceptance assertion** — `Tools/Player/main.cpp` must verify the animated entity's
   transform actually changed between first and last frame, and fail the run if not. Same shape as
   the existing `meshes=1` / `Automation: Scene geometry drawn OK` guards: the observable must be
   the *motion*, not the component's presence.
6. **A runtime-level test** that fails when `step_animation` is removed — per the project's own
   Rule 0 convention, confirm the test FAILS with the fix reverted. A round-trip test will not do;
   it must assert a transform changed after `Runtime::update`.

Only after that is a Phase 9 acceptance run meaningful.

---

## 5. Note on repository state

The main checkout (`C:/Users/abdal/OneDrive/Desktop/NOVAForge Engine`) is still at **Phase 6**
(`421511f`). All of Phases 7–9 — including the new `Engine/Animation/`, `Engine/Audio/`,
`Engine/Physics/`, `Templates/`, `Tools/ProjectTool/` — exist only as **uncommitted changes** in the
worktree: 88 changed/untracked paths, 6 commits total on `main`. The work is undefended against a
bad `git checkout` or a OneDrive sync event. Committing the green state before doing more work is
the cheapest risk reduction available.

---

## 6. Recommendation on Phase 10

**Do not start Phase 10 (Scripting + Save, `Docs/Phase10_Plan.md`) yet.** Two concrete reasons:

- Phase 10 W3 is "Inspector integration — the editor's `InspectorPanel` discovers `ClassInfo` and
  renders widgets." Adding a generic reflected-property panel on top of an inspector that is missing
  two of the five component types will either collide with the missing panels or bake the gap in.
  Fix the two hand-written panels first; then the reflection work has a working reference to match.
- Phase 10's acceptance criterion 6 is "all existing 420 tests + new suites green." That baseline
  would enshrine an unwired animation and audio system as the floor.

Suggested sequence: commit Phase 7–9 → finish Phase 9 integration (§4 above) → re-run the §5
acceptance → then Phase 10.

---

## 7. Closure (2026-09-14)

Every item in §4 was implemented and verified. The repository-state warning in §5 still stands.

### 7.1 The six gaps, closed

| §2 gap | Resolution |
|---|---|
| 2.1 Animation never runs | `Runtime::update()` now calls `step_animation(dt)` after `step_physics(dt)`. It samples each `AnimationComponent`, applies the posed bone delta to the entity's `Transform` relative to an authored base offset, and returns the count of entities it drove. |
| 2.2 No skinning, no render hookup | Out of scope by the phase's own §7 ("no GPU skinning"), but the *observable* half is fixed: the entity's `Transform` is genuinely written, so the render path sees the animated placement. `last_local_pose` / `last_world_pose` are now written by `step_animation` rather than documented as written by nobody. |
| 2.3 Audio never runs | `step_audio(dt)` mixes every autoplaying `AudioComponent` through `AudioBus` and exposes the output peak. The device is still `NullAudioDevice` (headless/CI), which is the phase's stated design — but the mixer is now on the frame path, so a real device is a backend swap and nothing more. |
| 2.4 No editor panels | `EditorApp::set_animation` / `set_audio` plus two `CollapsingHeader` panels in `Panels.cpp`, following the Phase 8 `InspectorCache` pattern. `set_transform` calls `rebase_animation()` so a hand-edit is not immediately overwritten by the next frame. |
| 2.5 §5 acceptance unimplemented | The template scene gains entity 6 (mesh + `Animation:` + `Audio:`). `Application.cpp` tracks the max deviation of the animated entity's transform from its first frame and **fails the run with exit 1** if it never moved. Executed output is in `Docs/Phase9_Plan.md` §5. |
| 2.6 The suite cannot catch any of this | `Tests/RuntimeTests/test_runtime_anim_audio.cpp` — 5 tests that assert observable motion and signal after `Runtime::update()`, plus `make_procedural_clip` tests (7) and `make_tone_buffer` tests (7) at module level. |

### 7.2 Rule 0 applied

The new runtime tests were confirmed to fail with the fix reverted. `step_animation(dt)` and
`step_audio(dt)` were commented out and the tree rebuilt:

```
RuntimeTests: 29 passed / 4 failed / 0 skipped (33 total)   ← was 33/0/0
Full suite:   435 passed / 4 failed / 1 skipped             ← was 439/0/1
RESULT: FAIL
```

The four failures are `runtime_steps_animation_and_moves_entity`,
`runtime_paused_animation_does_not_move`, `runtime_audio_mixes_generated_tone`, and
`runtime_scene_round_trip_keeps_animation_moving`. The fifth new test,
`runtime_audio_silent_without_sources`, passes in both states **by design** — it is the control that
distinguishes "the mixer is working" from "the mixer emits a constant", and a control that changed
behaviour with the fix would not be a control.

The packaged player was checked the same way and correctly refused to exit 0:

```
Animated entities: 1 (max transform deviation over 120 frames: 0)
ERROR ... no driven transform moved
Exiting: the scene has an animated entity that never moved.      EXIT=1
```

### 7.3 Final state

```
Build:      clean under /W4 /WX, zero warnings
Suite:      439 passed / 0 failed / 1 skipped / 440  (was 420/0/1)
              AnimationTests 31 → 38   (+7 procedural clip)
              AudioTests     20 → 27   (+7 tone buffer)
              RuntimeTests   28 → 33   (+5 runtime integration)
Acceptance: nf new → nf build → packaged NFPlayer --frames 120 --validation
              Animated entities: 1 (max transform deviation over 120 frames: 174.93932)
              Audio sources mixed (peak): 1 (0.19999997)      # volume 0.4 x amplitude 0.5
              Validation errors: 0
              Alive RHI objects before shutdown: 0
              exit 0
CI:         added a RuntimeTests non-zero guard, and end-to-end assertions that the
            animated transform moved and the mixer produced non-zero signal — the
            existing physics assertion would have stayed green through this regression.
```

### 7.4 Still outstanding

- **Phases 7–9 remain uncommitted** (§5). The main checkout is still at Phase 6. This is now the
  single largest risk to the work, and it grew by this pass.
- `nf new` reports the project path with backslashes in its "Next:" hint
  (`...\Demo\Demo.nfproj`) while accepting forward slashes. Cosmetic, but it is the kind of thing
  that leads a user to paste a mangled path into an MSYS shell.
- `Runtime::step_animation` logs "entity N has no skeleton data; using a single-bone root rig" at
  INFO. Correct behaviour, but it will be noisy once scenes have many animated entities.

