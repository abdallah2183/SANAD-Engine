# NOVAForge / SANAD Engine — Project Memory

## Identity / workflow
- Solo C++23 Windows engine by Abdal (@abdallah2183), repo `SANAD-Engine`, branch `main`, namespace `nf::`, CLI `nf`. No commit/push without explicit user request. Never delete `.workbuddy-ai`.
- Arabic UI/content is OK; Arabic in code identifiers/comments is a red line unless explicitly requested.
- Game-Ready Program G1–G10 asks: can a dev finish/ship a PC game without touching engine source? See `.workbuddy-ai/agents/agent-game-ready-program.md` and `agents/model-g*.md`.

## Build / verify
- In sandbox use `bash .workbuddy-ai/nfb.sh [--target X]`, not `build_nf.bat` (vcvars/reg.exe sandbox issues). Binaries: `build/DebugNinja/bin/`.
- Tests use `NF_TEST/NF_CHECK/NF_CHECK_NEAR/NF_SKIP`; register new tests in `Tests/CMakeLists.txt`; skip != pass; always re-run/count, never quote stale counts.
- Last full sweep 2026-10-02 (measured, not recalled): 26 suites, `1769 passed / 0 failed / 2 skipped` (ECSTests 1, InputTests 1). RuntimeTests 106, EditorTests 252, PerfTests 9, UITests 42, AudioTests 119, RHITests 241.
- Earlier recorded baselines (1435 / 1571 / 1589 / 1647 / 1703 / 1709 / 1717 / 1718 / 1723 / 1724 / 1733) are stale — re-run, never quote them. `COORDINATION.md` carries many stale numbers.

## Post-processing stack — Phase 27, §206 (landed 2026-10-02)
- `PostFxParams` (`Renderer3D.hpp`) is now the whole §206 stack: `bloom` / `grade` / `sharpen` nested stages plus `saturation`/`vignette` first (so `PostFxParams{1.15f, 0.28f}` keeps its meaning). Every stage is neutral by default and every neutral value is EXACTLY identity — the golden pixels depend on it.
- **Bloom is a real 4-level chain** (`kBloomLevels`), not a single-shader glow. `bloom.{vert,frag}` serves both the prefilter and the 13-tap downsample via a push-constant mode flag; the chain stops at the smallest level and the TONEMAP pass sums the levels with bilinear taps (a bilinear fetch of a half-size level IS the 2×2 tent filter an upsample pass would apply). No upsample pass, no additive blending.
- `enabled=false` records no bloom pass at all, and the tonemap shader's `bloom_intensity > 0` guard means the bloom bindings are never read; the HDR view is bound in their place so every binding stays a valid image.
- **Stage order (shader AND `apply_post_chain`):** `sharpen → bloom add → exposure → grade → tonemap → gamma → saturation → vignette`. §206 lists exposure before grading and bloom after; the bloom add is before exposure because the prefilter reads the raw HDR target, so its threshold is in pre-exposure units.
- `apply_post_chain` is a CPU mirror of the WHOLE tonemap fragment body, so the ordering is testable. `apply_postfx`/`tonemap` remain the sub-step mirrors.
- Tonemap push block is **64 bytes (16 floats)** and `m_tonemap_layout` has **5 bindings** (HDR + 4 bloom levels). `present_texture` must write all five even though `present.frag` reads only binding 0.
- Scene data: `PostProcessComponent` + a `PostProcess:` line (parse/write/clone). Written only when the component exists. `Runtime::extract_post_process()` pushes it, called from both render paths beside `extract_sky()`; a scene with no component leaves renderer state ALONE (does not reset it). Editor: `SetPostProcessCommand`, `make_post_process_command`, `EditorApp::set_post_process`, Inspector panel, Add-component entry.
- `Runtime::load_scene` does NOT push the block — the push happens on the render path, like the sky and the lights. A test that asserts renderer state must therefore record a frame first (`render_offscreen`).
- Deferred on purpose: DOF, motion blur, lens effects, colour-grading LUT, and `exposure`/tonemap-mode as scene data. See `Docs/Phase27_Plan.md`.

## The `uv1` water/terrain "collision" — investigated and CLOSED (2026-10-01)
- COORDINATION.md item 8 called this "needs a **lead** decision". It does not, and the code already answers it. Both headers say the SAME thing in so many words:
  - `Engine/Water/include/NF/Water/Water.hpp:147-150`: "uv1 carries `(foam, reflection)` … so water rides the same vertex layout the renderer already uploads, **and the one `uv1` declaration the terrain needs is the one the water needs too**."
  - `Engine/Rendering/include/NF/Rendering/Terrain.hpp:100-106`: "The splat the mesh builder writes into `uv1` … `uv1 = (slot, blend)`. A shader samples the array at `slot` and `slot + 1` and mixes them by `blend`, which **serves any number of layers in the two attributes the mesh already uploads**."
- **So there is no collision: there is one `uv1` attribute with two consumers, and they never meet.** A water mesh and a terrain mesh are different draws; each reads its own `uv1` with its own shader path. Sharing a vertex layout is the point — it is what avoids a second vertex format in the renderer.
- Both are `StaticMesh::Vertex` (position, normal, tangent, uv0, uv1) — one layout, one upload, two meanings. Widening to a third channel (uv2/uv3) to "resolve" a non-conflict would add a second vertex format and buy nothing.
- The real open item was never the protocol: it is that **the renderer does not yet sample `uv1` at all** (`Samples/WaterDemo/main.cpp:20-25`: "Until the renderer samples `uv1` (requested in COORDINATION.md, R1) the surface renders as a solid displaced plane… the data is correct and tested, it is the paint that is pending"). Both splat and foam/reflection are authored and tested on the CPU; neither is painted. That is shader work in the deferred pass, and it is optional per-design (the memory budget it buys is the reason to weigh it), so it stays unstarted rather than mislabelled as a protocol conflict.
- **No code changed for this item.** Closing it with evidence is the deliverable.

## Editor automation: the last two failures — BOTH FIXED (2026-10-01)
Both are now green: `NOVAForgeEditor.exe --frames 125 --validation` exits **0**.

### 1. "Viewport drag undoes cleanly" — the harness compared two different entities
- Symptom: `dragged to 0.7849, undo -> 0.0000, drag began at 0.5000`.
- **Cause:** the baseline came from `find_first_mesh()` (entity **2**) while `viewport_press()` re-picks whatever is under the cursor and got entity **3**. The undo faithfully restored the *picked* entity's start (0.0); the check compared it against the *other* entity's 0.5. Two different objects, so it could never pass.
- The editor was correct the whole time: `GizmoDrag::begin/commit` and `SetTransformsCommand::undo` all store and restore the right value.
- **Fix (`Editor/src/main.cpp`):** sample `automation_drag_start_x` from `app.selection().primary()` *after* the press, not from the pre-press mesh.
- Two wrong conclusions of mine are recorded here so they are not repeated: I first blamed the shared undo stack, then the editor's capture. Both were disproved by measurement. **The trace that settled it** was a temporary log in `CommandStack::undo` + `SetTransformsCommand::undo` + `GizmoDrag::commit()` printing `depth`, `label` and `before_x`.
- Draining the undo stack was tried and rejected: it turns this green but breaks `Save/Load structure round-trip`. It was never needed.

### 2. "Drag mesh to viewport" — a REAL product bug: tooling directories listed as project assets
- Symptom: `Only mesh assets with a valid AssetId can be dropped into the viewport`, with the offending entry printed as `project://.kilo/worktrees/bald-bosworth/Content/Meshes/cube.nfmesh`.
- **Cause:** `list_project_assets()` (Editor/src/AssetBrowser.cpp) excluded tooling dirs by a hardcoded blocklist — `/dist`, `/.git`, `/build`, `/Cache`. Any *other* dot-directory (`.kilo`, `.vs`, `.idea`) was scanned and listed as authored project content. It sorts ahead of the real `Content/`, so a drag grabbed the tooling copy, which carries no AssetId. **This would have shipped tool/VCS files inside a packaged game.**
- **Fix:** structural instead of enumerated — skip any path component beginning with `.` and call `disable_recursion_pending()` so the scan does not even walk it.
- **Ordering trap that made the first fix attempt silently do nothing:** the dot-directory test must come BEFORE `is_regular_file()`. A directory entry is not a regular file, so the pre-existing test `continue`s first and the iterator still descends. The filter looked right, compiled clean, and changed nothing.
- **This failure had been invisible all along** because PowerShell `*>` redirection drops the logger's stderr WARN lines while INFO lines still appear, so every earlier "automation sweep" I ran was reading a log that structurally could not show a FAILED check. **Use `cmd /c "prog > log 2>&1"` for any run whose failures matter.**

### 3. `scenes_equal_structure` (Editor/src/PlayMode.cpp) — hardened
Transforms compare with a relative epsilon instead of `!=`, because a save/load round trip goes through the 9-digit scene text and a gizmo-dragged transform can land one ULP away. Failure message prints both tuples.

## Editor launcher templates (closed 2026-10-01)
- The New Project gallery showed 6 cards but only "Blank Scene" was enabled, and `create_project_in_shell()` **ignored `selected_template` entirely** — it always scaffolded `NF_TEMPLATE_DIR`. Four real templates existed on disk and worked via `nf new --template <name>`, so the gallery was advertising less than the engine could do.
- `TemplateInfo` now carries `template_name`; Platformer2D and ThirdPerson are enabled and resolve through the CLI's own `resolve_project_template()` allow-list, so gallery and CLI cannot drift. Placeholders stay disabled and fall back to **Default** (never to "no template", which yields a Content/ with no Main.nfscene and an editor that exits on open).
- `kTemplates` must be declared **above** `create_project_in_shell()`; the table used to sit further down the file.
- ⚠️ **Never rewrite a source file with PowerShell `Set-Content` / `Get-Content -Raw` here.** It writes the wrong codepage and silently destroys every Arabic character in the file (ProjectLauncher.cpp lost all 23 of them; caught by `/WX` C4828 and fixed with `git checkout --` + re-applying via the editor tool). Use the editor tool.

## TimeOfDay is scene data now (2026-10-01)
- `TimeOfDayComponent` (`RuntimeSceneTypes.hpp`) + a `TimeOfDay:` line in `.nfscene`: `hours=`, `day_length=`, `enabled=`, `drive_light=`. The clock is the component's `time_hours`, rewritten each `update()` — the component is the single source of truth, so a save mid-cycle round-trips the exact hour.
- `Runtime::advance_time_of_day()` runs in `update()` after `step_animation`, NOT in render: advancing at render time would make the hour depend on how often the frame was drawn rather than how long it took.
- `find_time_of_day()` is shared by `extract_light` and `extract_sky` so sun and sky can never be driven by two different hours. It returns the *enabled* component; `enabled=false` falls back to the authored Sky palette.
- `hours` wraps (25.0 → 1.0) rather than rejecting the line; non-finite values warn and keep the default. `day_length<=0` is "frozen", a legitimate fixed golden hour.
- Line is written ONLY when the component exists — a scene that has never heard of it round-trips byte-identical, so no existing level silently gains a day/night cycle on first save.
- An enabled cycle overrides the sky palette entirely, not just the sun: driving the sun across a noon-blue authored sky is the result that reads as broken rather than merely wrong.
- **Editor path added:** `SetTimeOfDayCommand` + `make_time_of_day_command()` follow the exact `SetSkyCommand` pattern (validated factory, undo/redo). `EditorApp::set_time_of_day()` and an Inspector panel sit beside the Sky one. New Arabic keys: `add_day_night`, `day_length`, `drive_light`, `hour`, `no_day_night`. Undo of the edit that first created a cycle **removes** the component, so the scene round-trips with no TimeOfDay line.

## The "distinct meshes" 5x claim — MEASURED, and it does not reproduce (2026-10-01)
- A field report claimed frame time scales with DISTINCT MESHES, not entity count: 522 entities / 56 meshes = 4.3 fps vs the same 522 entities / 1 mesh = 21.4 fps (5x). It said so itself: "the numbers above are the reproduction, not the diagnosis", and blamed the 32-slot descriptor allocator at `Renderer3D.cpp:407` as a *hypothesis*.
- `perf_frame_time_tracks_distinct_meshes_not_entity_count` now builds that A/B in code (240 entities, same material, same triangles, 1 mesh vs 64 meshes) so it can be re-run on any machine instead of existing as prose.
- **Measured here: 3.20 ms (1 mesh) vs 4.67 ms (64 meshes) = ~1.46x, not 5x.** The penalty is real but small and roughly linear in mesh count.
- Do NOT quote "5x slower" in any perf claim. It was one machine, one scene, and does not hold. The 1.46x figure is the reproducible number.
- Baseline rows `mesh_law_shared_ms` / `mesh_law_distinct_ms` recorded in `Tests/PerfTests/perf_baseline.csv` at 25% tolerance (timings are noisier than the deterministic counters).
- On this host `build_nf.bat` is invoked as `& '...\build_nf.bat' --target X` from PowerShell; it must set up vcvars itself.
- Restoring a file with `Copy-Item` can leave ninja thinking "no work to do" — touch `LastWriteTime` before rebuilding.
- `--headless` never records ImGui frames. UI regressions require bounded windowed validation: `NOVAForgeEditor.exe --scene content://Scenes/Example.nfscene --frames 125 --validation`.
- `--screenshot <file.bmp>` writes last composited scene+UI frame; without `--frames` defaults to 90. Convert BMP to PNG with `.workbuddy-ai/tmp/bmp2png.py`.
- `--resize-test` resizes every 25 frames; use for swapchain/lifetime checks. Windowed editor is currently validation-clean (0 errors). Two automation failures remain known/pre-existing: `Viewport drag undoes cleanly`, `Drag mesh to viewport`.

## Editor UI rules
- Theme roles in `Editor/include/NF/Editor/UiTheme.hpp`; ImGui palette/style in `UiBackend.cpp`. Keep accent family synced. One UI font size for Latin+Arabic.
- Default dock layout (`Panels.cpp`): left Outliner/FileSystem, center Viewport, right Inspector (+ scene panels), bottom Console/Tools. Persisted layout only for human sessions; scripted runs are deterministic. `DockBuilderFinish()` needs explicit dirty marking for persisted defaults.
- ImGui widget ID = label. Any repeated visible label in a window needs `##suffix` — and **the suffix must be UNIQUE, not merely present**: an ID is the text AFTER `##`, so fourteen widgets all ending in `"##pp"` share one ID. `Scripts/audit_imgui_ids.py` does NOT catch that (it flags *bare* labels only), and ImGui's own duplicate detector is hover-based so no automated run sees it either. Run `python Scripts/audit_imgui_ids.py`. Runtime ImGui duplicate detector is hover/render dependent; force relevant panels/tabs open before trusting a clean run.
- Arabic localization in `Engine/UI/src/Localization.cpp`; keys must be duplicate-free (`UITests::localization_keys_are_unique`). Use `AV(key)` for displayed shaped labels and `ui::tr(key)` for logic/stored names.
- 2026-09-26 UI overhaul added `UiWidgets.hpp`, `ScenePanels.hpp/cpp`, Window menu, and docked panels: Lighting, Environment, Camera, Render/Performance, World. Panels must bind to real runtime/editor state, not placeholders.

## Input record/replay from the CLI (landed 2026-10-01)
- `NFPlayer --input-log <file>` records; `--replay <file>` replays; `--replay-strict` fails a run that served no frames. Plain filesystem paths (not `saves://`) — they name a file on the machine that ran the game.
- Validation lives in `prepare_input_replay()` (RunConfigResolver.hpp) so it is unit-testable without a window/device. It refuses: both flags at once, an unreadable file, an empty file, and a malformed log — each with its own message.
- **The trap that shipped a silent no-op:** loading the log into a probe-scope local and then handing `play_input_log()` a *second, still-empty* `InputLog`. Exit code 0, "replayed 0 frames", entity never moved. The real `replay_log` is now declared at function scope. Whenever a value is validated early and consumed late, the *same* object must cross both — a second load is a silent-empty bug waiting to happen.
- Verified end to end on a real `nf new` project: record 30 frames → cook → replay 60 frames → `(loaded)`, exit 0.
- `nf run` forwards `--replay <file>`, `--input-log <file>`, `--replay-strict` to the player (Tools/BuildTool/main.cpp allowlist), and the shipped `README.txt` documents them. Verified on a path containing spaces (`C:\...\My Game\My Game.nfproj`) — exit 0. The old COORDINATION.md "rc=1 quoting bug" no longer reproduces; the quoting goes through `quote_windows_argument` + `CreateProcessW` with no `cmd.exe` in the path.
- **Trap in that allowlist:** a flag that takes a value MUST consume it with `next()`. Without it the value falls through to the positional branch and is read as the project name. Symptom was `"--frames" "--replay" "path"` — `--frames` had lost its number, and the replay path became a stray positional argument. Every value-taking flag needs its own `next()`; `--headless`/`--validation`/`--replay-strict` take none.

## Audio facts (the G5 bridge landed 2026-10-01)
- `Runtime::step_audio` mixes through ONE `audio::AudioScene` (`begin_block -> mix_emitter -> finalize`), not a bare `AudioBus`. Buses, Settings volumes, music/ambience and reverb are reachable from a shipped game.
- A game reaches the mixer with `rt.audio_scene().settings()` — no runtime replacement. `GameplayContext::audio` is an `audio::AudioScene*` (was `AudioBus*`).
- `AudioComponent` carries `sample_cursor` AND `sample_position` (f64). Both must be persisted across blocks; the integer alone truncates a rate-converted buffer and drifts. `Runtime` keeps persistent `Emitter`s in `m_audio_emitters` keyed by entity — a per-frame temporary would reset the occlusion filter and click.
- `pitch` was decorative until 2026-10-01: authored/serialized/inspector-editable but `AudioBus::mix_source` never read it. It now multiplies the resampling step; non-finite/non-positive falls back to 1.0.
- Scene `Audio:` line carries `bus=` and `occluded=`; an unknown bus name is a warning, not a silent default.
- **Game settings bridge (closed 2026-10-01):** `Engine/Audio/include/NF/Audio/GameSettingsBridge.hpp` — `GameAudioSettings` + `load_game_audio_settings()` / `save_game_audio_settings()`. A game's settings screen now reaches the live mixer with `settings.apply_to(rt.audio_scene().settings())` and persists to a file. Fixes the documented default mismatch: `SettingsData` (Engine/UI) defaulted music 1.0 vs the mixer's 0.8, so pushing screen values at boot silently turned the music up.
- **Settings-file parsing rule, and why the asymmetry is deliberate:** the KEY is trimmed on both sides (indenting keys is a common, unambiguous human edit); the VALUE is passed through verbatim because `AudioVolumeSettings::apply_setting` rejects a value with surrounding characters so `"0.50x"` cannot silently become 0.5. A padded `audio.volume.music = 0.25` is therefore IGNORED (and warned about), not misread. Two tests pin both halves — do not "fix" the value side.

## Scene / assets / runtime facts
- `.nfscene` supports **19** component lines (re-derived 2026-10-02 by grepping `line.rfind("  <Name>:",0)==0` in `RuntimeSceneLoader.cpp` — derive it that way, the count has gone stale twice): `Name`, `Transform`, `Camera`, `Light`, `Sky`, `TimeOfDay`, `PostProcess`, `Mesh`, `RigidBody`, `Collider`, `Animation`, `Audio`, `Destructible`, `Particles`, `Cloth`, `Character`, `Script`, `Module`, `Prefab`. `id = block index`; after `entity_count`, each block needs `---` and `entity: <id>:0`.
- Scene collider is centered on entity origin; many kit meshes do not pivot at visual center. Collider-only fixes may need separate centered entities, but destructibles need their own collider.
- Scene loader creates only directional lights. Material asset has one albedo texture; normal/roughness/ORM maps are not supported directly.
- Mesh import has one read entry point: `nf::assets::import_mesh_file`; extension wins, content sniffing fallback, contradiction fails. `.nfmat` is the only material route; `.nfmesh` is geometry only.
- Transform components use `local_x/y/z`, `world_x/y/z`, `dirty`; write local then propagate. Entity id 0 is valid but invisible to GPU picking.

## Performance / gotchas
- Frame time scales with distinct meshes, not raw entity count (measured 2026-09-23: 522 entities/56 meshes -> 4.3 fps; same entities/1 mesh -> 21.4 fps). Quote entity capacity only with distinct mesh count.
- `RHITests::shader_async_compile_via_job_system` race fixed; avoid fixed sleeps for GPU/external compiler tests.
- Git under OneDrive: after commits run `bash Scripts/git_repair_ref.sh`; avoid `git stash`; push often.
