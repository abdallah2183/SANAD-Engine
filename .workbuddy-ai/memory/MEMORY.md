# NOVAForge Engine — Project Memory

## Identity / architecture
- Solo C++23 game engine by Abdal. Namespace `nf::`, public types use `NF` prefix, CLI is `nf`.
- Core principles: RHI abstraction, sparse-set ECS, Render/Game World separation, stable Asset IDs, versioned serialization, async IO/jobs, modular subsystems, dedicated-server path, profiling/CI.
- Design doc: `NOVAForge_Engine_Complete_Design.md`, 335 numbered sections (0–334), 5,742 lines.

## Build / verify
- Preferred: `bash Scripts/build.sh`; it is worktree-safe and auto-detects VS/MSVC/SDK. Vulkan SDK `C:/VulkanSDK/1.4.357.0`.
- Existing build env: `/c/Users/abdal/AppData/Local/Temp/nf_build_env.sh`; build dir `build/debug`.
- Tests: `bash Scripts/run_tests.sh build/debug`. A skip is never a pass. Build and run before calling a state green.
- MSYS gotchas: PATH entries use `/c/...`; CMake program/compiler args use Windows-style paths. INCLUDE/LIB remain `;`-separated.
- Intermittent Windows locks may cause `LNK1168`/`RC1109`; remove the locked output/manifest and rebuild.

## Verified milestones
### Phase 6 / renderer
- Vulkan RHI is abstract (`RHI.hpp` has no Vulkan includes), dynamic-loaded backend, headless device support.
- Stale framebuffer bug fixed: cache keys use monotonic `Texture::creation_serial()`, bounded to 8; validation clean.
- Test framework has honest `NF_SKIP`; GPU absence never reports a false pass.

### Phase 7 COMPLETE (2026-09-13)
- `nf new` → `nf build` → `nf verify` → packaged `NFPlayer` works outside the engine tree.
- Project system: `ProjectDescriptor`, `RunConfigResolver`, cook-all/pruning, CLI, packager, standalone player, editor `--project` + Build, CI end-to-end.
- Decisive proof: relocated package + build shaders hidden + packaged player with no `--project` → exit 0, `meshes=1`, 0 validation errors, 0 RHI leaks.
- Final Phase-7 suite: **263 passed / 0 failed / 1 skipped / 264**.
- Content cleanup: dead `corrupt.nfmesh` and fake `dummy.frag` moved to `Tests/fixtures/`; stale registry entry removed.
- Gotcha: two incompatible mesh formats: Assets writes `NFME` (the cooker accepts it), Rendering writes `NFM1`. Cooker fixtures must use `assets::MeshAsset::from_static_mesh(...)->save_to_file()`.

## Phase 8 — Physics COMPLETE (2026-09-13)
Plan: `Docs/Phase8_Plan.md`. Chosen approach: engine-facing deterministic solver behind `PhysicsWorld`; no Jolt vendoring without Abdal's explicit decision.
- W1: `Quat` inverse/rotate/from_matrix + Vec3 component helpers; 8 tests.
- W2: sphere/box/plane shapes, AABB/support/mass properties; 15 tests.
- W3: deterministic uniform-grid broadphase, brute-force parity tests; 13 tests. Fix: validate huge coordinates in f64 before i32 conversion; per-axis cap avoids overflow.
- W4: manifolds for sphere/sphere, sphere/box, sphere/plane, box/plane, box/box SAT + clipping; 26 tests. Fix: box-box SAT category and axis index are separate (`best_kind` vs `best_i`); reference-face orientation fix (`ref_dir = a_is_ref ? best_axis : -best_axis`).
- W5: solver — sequential impulses, warm starting (position-based matching), Coulomb friction, restitution, Baumgarte via split impulse, island-based union-find sleeping. 18 tests. Fix: static bodies excluded from islands; 12/4 velocity/position iterations.
- W6: `PhysicsWorld`, fixed timestep (1/64s), generation-checked `BodyHandle`, `state_hash` (FNV-1a). 9+5 determinism tests verified across regular/coarse/irregular step patterns.
- W7: ECS integration (`RigidBodyComponent`/`ColliderComponent`), Runtime owns `PhysicsWorld`, scene load rebuilds bodies, fixed-clock stepping writes transforms back. Scene serialization round-trip: 5 tests.
- W8: Editor inspector panels for RigidBody/Collider, `set_rigid_body`/`set_collider` on `EditorApp`, editor acceptance (f==106/108/109), physics scene in template (falling box on static plane), README + Phase8_Plan updated, CI asserts PhysicsTests > 0 and physics world creation in end-to-end.
- **Final Phase-8 suite: 366 passed / 0 failed / 1 skipped / 367.**
- Acceptance proof: `nf new` → `nf build` → packaged `NFPlayer --frames 120 --headless --validation` → exit 0, `physics world created with 2 bodies`, 0 validation errors, 0 leaks.

## Phase 9 — Animation + Audio COMPLETE (2026-09-14)
Plan: `Docs/Phase9_Plan.md`. Built the two remaining Year-2 modules from design doc §259.
- **Animation** (W1–W3): `Skeleton`, `AnimationClip` (binary-search sampling, `blend_poses`/`blend_additive`), `AnimationPlayer` (play/pause/stop, loop/ping-pong), `AnimationStateMachine` (states, transitions, cross-fade). Core: `Quat::slerp`/`nlerp` added. **Mat4 convention**: row-vector (`v * M`), so TRS = `S * R * T`, parent composition = `local * parent_world`. Cross-fade: `check_transitions()` at the beginning of `update()` so fade applies same-frame. 38 tests.
- **Audio** (W4–W5): `AudioEngine` (attenuation: None/Linear/Inverse/Exponential, equal-power stereo pan, `AudioBus` mixer, `AudioListener`), `AudioDevice` abstract + `NullAudioDevice` for headless/CI. 27 tests.
- **Integration** (W6): Scene serialization (`Animation:`/`Audio:` lines in `RuntimeSceneLoader.cpp`), README/CI/run_tests updated, `Phase9_Plan.md` complete.
- **Integration pass (second pass, same day)** — the first pass shipped the module layer only and wired nothing into `Runtime::update()`. Closed: `Runtime::update` now runs `step_physics` → `step_animation` → `step_audio` → `propagate_transforms`; `make_procedural_clip()` / `make_tone_buffer()` supply real motion and samples without an import pipeline; `AnimationComponent` carries an authored base offset with `Runtime::rebase_animation()` so animating does not snap an entity to the rig origin; editor gained `set_animation`/`set_audio` + two inspector panels; template scene entity 6 is animated and audible; `Application.cpp` exits 1 if the animated entity never moves. 5 new runtime tests fail when the stepping is removed (Rule 0). Detail: `Docs/Phase9_Verification_2026-09-14.md` §7.
- **Final Phase-9 suite: 439 passed / 0 failed / 1 skipped / 440 across 11 suites** (Core 44, Jobs 4, ECS 25+1skip, Assets 45, RHI 69, Physics 86, Animation 38, Audio 27, Runtime 33, Editor 54, Tools 14). (First pass was 420/0/1 — treat the count as a fingerprint, not a constant.)
- Design doc Year-2 plan (Physics + Animation + Audio + Tools) fully delivered. Phases 5–9 all complete.

## Phase 10 — Scripting foundation + Save System COMPLETE (2026-09-14)
Plan: `Docs/Phase10_Plan.md`. Design doc §70 + §71–74 + §249 (S0–S1).
- **W1 Reflection** (`Engine/Core`): `PropertyInfo` / `ClassInfo` / `EnumInfo`, `ReflectionRegistry`, macros `NF_CLASS` / `NF_PROPERTY` / `NF_PROPERTY_ENUM` / `NF_CLASS_END(_DERIVED)` / `NF_ENUM_BEGIN|VALUE|END`. `NF_CLASS` opens a static function body — a **complete-class context** — which is what makes `offsetof(Type, member)` legal in-class and keeps a declaration next to its member. No codegen. Text form prints floats at **9 significant digits** (shortest exact f32 round-trip). 15 tests.
- **W2 Gameplay modules** (new `Engine/Gameplay`, depends only on NFCore): `GameplayModule` lifecycle + `GameplayModuleRegistry` + `NF_GAMEPLAY_MODULE` + `GameplayContext` + `GameplayModuleComponent`. `Runtime::update` = physics → animation → audio → **step_gameplay** → propagate. A module's reflected state is a **separate plain struct** (offsetof is unusable on a polymorphic class). `initialized` is tracked **per module**, not per set. 18 tests.
- **W3 Inspector**: `ReflectedObjectView` in `NFEditorCore` holds all the policy (testable, window-free); `Panels.cpp` is a decision-free walk. `EditorApp::attach_gameplay_module` / `detach_gameplay_module`. 11 tests.
- **W4 Save system**: slots under `saves://` (`scene.nfscene` + `modules.txt` + `meta.txt`), staging + swap with **rollback**, async save (snapshot on the calling thread, write on a worker), interval autosave, chained `register_migration`. `ProjectDescriptor::make_default` gained `saves://` → `Saves`. 16 tests.
- **W5 Scene serialization**: `Module:` line with escaped `props=`; the escaping lives in `gameplay::encode_properties`, shared with the save format so the two cannot drift. 6 tests.
- **W6**: `Game` toolbar popup (slot field, Save/Load, autosave interval, slot browser); autosave ticks off `UiFrameStats::dt_seconds` in `ui_frame`, not from `Runtime::update`. `OrbitCameraModule` sample in `NFEditorCore` so EditorTests can drive it headlessly — 8 tests. CI gained non-zero guards for GameplayTests + SaveTests and a direct `CoreTests.exe reflection` run.
- **Final Phase-10 suite: 515 passed / 0 failed / 1 skipped across 13 suites** (Core 59, Jobs 5, ECS 25+1skip, Assets 45, RHI 69, Physics 86, Animation 38, Audio 27, Gameplay 24, Save 16, Runtime 33, Editor 74, Tools 14). Was 439/0/1 across 11.
- Phases 7–9 committed first as `13e79da` (106 files, parent `421511f`).
- Deferred on purpose: migrating the existing hand-written components (`RigidBody`, `Collider`, `Animation`, `Audio`) onto the generic reflected path — it would change the on-disk format of every existing scene, and two serializers for one component is two sources of truth.

## Phase 11 — Foundation: layering, debt, world streaming COMPLETE (2026-09-14)
Plan: `Docs/Phase11_Plan.md`. No new feature area — three things removed that were actively wrong, one seam landed.
- **W1** `Engine/Assets` no longer depends on `NFRendering`. `AssetManager` was doing two jobs (load/parse/cache **and** GPU-upload meshes); it is now CPU-pure (VFS + registry + parse + cache, links only `NFCore NFJobs`). The `StaticMesh` conversion moved to `NFRendering/MeshUpload`; `Runtime` owns the device and the upload. The second `.nfmesh` format (`rendering::mesh_asset`, magic `NFM1`) is deleted — `NFME`/`assets::MeshAsset` is the only one.
- **W2** `Engine/Rendering` no longer depends on `NFEcs`/`NFScene`. The inversion was one function (`extract_render_objects`); the ECS bridge moved to `NF/Runtime/SceneExtraction.hpp`. `Rendering` links only `NFCore NFRHI NFJobs`, so the renderer builds standalone — which is what the dedicated-server path needs.
- **W3** The six dead RHI handle types are deleted (`BufferHandle`, `TextureHandle`, `PipelineHandle`, `ShaderModuleHandle`, `RenderPassHandle`, `FramebufferHandle`); zero references, replaced by an explanatory comment at `RHI.hpp:400`.
- **W4** ECS benchmarks assert a ceiling (10,000 ns/entity, ~12× the measured Debug baseline) instead of printing numbers nobody reads.
- **W5** World-streaming seam: `scene::StreamingVolume` (grid: `ChunkCoord`, `chunk_at`, `distance_to_chunk`, `chunks_in_radius`) + `runtime::WorldStreamer` (range, hysteresis, per-update load cap). Distance is to the chunk **AABB**, not its centre; `chunk_at` **floors**; `chunks_in_radius` returns **nearest-first** (order is load-bearing wherever a cap exists). 19 tests.
- **W6** Rule 0 executed for W1–W5 (each defect reintroduced, the matching test/guard confirmed to fail, then reverted). `Scripts/check_layering.sh` runs in CI right after the build; the CI per-suite guards became one `require_suite` loop over the runner's list plus a "a suite did not build" check. README + plan marked complete.
- **Final Phase-11 suite: 534 passed / 0 failed / 1 skipped across 14 suites** (Core 59, Jobs 5, ECS 25+1skip, Assets 45, RHI 69, Physics 86, Animation 38, Audio 27, Gameplay 24, Save 16, **Streaming 19**, Runtime 33, Editor 74, Tools 14).
- Commits on `workbuddy/main-9da20c25`: `5b63eb5` (W2–W5), `7fbe65f` (W1), `0b2e769` (W6), `b6b5546` (repair-script fix).
- **Fixed a pre-existing defect found by W6 verification:** the editor's headless acceptance was **failing** (`Physics: body moved under gravity FAILED — world_y = 0`, editor exit 1, which would make the CI acceptance step red). Bodies are built from the scene only on load, so a `RigidBody`/`Collider` added through the inspector never became a body and Play simulated the on-disk scene. `EditorApp::play()` now calls `Runtime::rebuild_physics_from_scene()` before simulating. Not a Phase 11 regression — the physics rebuild/step are byte-identical to Phase 10.

## Lessons that cost real time
- **A green suite says nothing about integration.** `Runtime::update()` is the ground truth for what runs in a shipped frame; anything unreachable from it is dead in a game no matter how many unit tests pass. See the `novaforge-verify` skill, Step 3c.
- **A content assertion only covers the subsystem it names.** CI asserted `physics world created`, which stayed true while animation/audio were never stepped — the falling box is physics-driven.
- **A helper with no test call sites is not "covered by the suite".** `JobGroup::add` bumped its pending counter and enqueued the bare task without handing the counter over, so `JobGroup::wait()` spun forever. `JobGroup` appeared in zero tests, which is exactly why it survived. Grep for the helper's name in `Tests/` before assuming it works.
- **Dead code hides defects.** `Quat::from_euler` returned the zero quaternion for `(0,0,0)` and nothing called it. When a public API is unused, check whether it is correct before building on it — and prefer deleting a broken duplicate over repairing it into a second convention.
- **`Scripts/build.sh` only knows `build/debug` and `build/release`.** Rebuilding a custom dir like `build/verify` needs the MSVC env set up by hand; see the wrapper in the `novaforge-verify` skill, Step 1.
- **MSYS mangles paths passed as arguments to `nf.exe`.** `/c/Users/...` becomes a rooted `\c\Users\...` and the project lands at `C:\c\Users\...`. Always pass `C:/Users/...`.
- **`grep "a\|b"` under this MSYS shell gave false negatives** on `Main.nfscene` (reported no `Animation:` line while the file had one). Confirm with `Read` before concluding a file lacks something.
- **The main `.git` lives inside OneDrive, and OneDrive deletes new ref files under `.git/refs/`.** A commit can print a success hash and then leave the branch unborn. The commit object survives; recover the sha from the reflog and recreate the ref with the full 40 characters. Re-check `git rev-parse HEAD` after every commit. Detail in the `novaforge-verify` skill.
- **`f32` needs 9 significant digits to round-trip through text.** `%.6g` looks fine and silently loses the low bits; the loss shows up as drift after a save/load, not as a failure.
- **"Does HEAD resolve?" is not enough to detect a lost ref here.** Once a branch has been packed, deleting the new loose ref leaves the *previous* tip in `packed-refs`, so HEAD keeps resolving — to the old commit — and a fresh commit looks applied while being unreferenced. `Scripts/git_repair_ref.sh` now compares HEAD against the **reflog tip** (the reflog is written first and is the only record of the truth) and reports `STALE`. This bit the W6 commit.
- **A file the engine writes only on load cannot see an edit made while it is open.** Physics bodies were built in `Runtime::load_scene`; the inspector writes components into the live ECS, so a `RigidBody` added in the editor had no body and Play simulated the on-disk scene. When a comment says "the runtime rebuilds this on the next play/step", grep for the call — nothing did.
- **A generated file checked into git must be deterministic.** `Content/AssetRegistry.nfreg` is rewritten by the cooker; it is stable across repeated runs, so a cook no longer dirties the tree. Verify with two consecutive cooks before assuming a diff is noise.

## Open architectural debt
- ~~Two `Mat4`s with the same name and different layouts~~ — RESOLVED (working
  tree): one type, one owner (`nf::Vec3`/`nf::Mat4`, NFCore). The renderer's
  column-major pair is deleted; `Camera`/`RenderObject` and all uploads use the
  Core types (uploads stay plain memcpy by the flat-array identity). Fixed en
  route, all previously masked: Core `look_at` rotation untransposed for tilted
  views (+ an `m[2][2]`/`m[1][2]` typo), `perspective` collapsing depth to
  ~[1,2], `orthographic` mapping far to -1, GL-style `[-1,1]` renderer
  projection replaced by Vulkan `[0,1]` (pinned near→0/far→1), frustum
  re-derived for row-vector clip (columns, near = col 2). `compose_trs_mat4`
  with a byte-equivalence test vs the legacy bytes.
- ~~`Runtime::load_scene` replaces the world~~ — MERGE LANDED (working tree):
  `merge_scene_into_world` + canonical `copy_scene_entity` (shared with prefab
  cloning, which now delegates; live body handles reset on copy) next to the
  scene loader, and Runtime wiring (`enable_streaming`/`step_streaming`:
  merge → mesh kick → physics rebuild, destroy-subtree unload). New tests:
  3 merge + 1 end-to-end wiring.
- Audio backend swap (MiniAudio behind `AudioDevice` seam) — planned, not yet implemented.
- Animation lacks a skinning GPU pipeline (CPU-only sampling); no animation asset cooker yet.

## Working tree convention
- Phases 7–11 are merged into `main` (fast-forward to `b2a0853`, then a
  `PRODUCT.md` doc fix on top). The old `workbuddy/*` session branches are
  superseded; branch new work from `main`.
- **After any commit here, run `bash Scripts/git_repair_ref.sh`.** OneDrive deletes new ref files under the main `.git/refs/`; the commit object survives but the branch can end up unborn. The script is worktree- and branch-agnostic and detects the stale-packed-ref case.
- Never delete `.workbuddy-ai`.
