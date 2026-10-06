# NOVAForge / SANAD Engine — Project Memory (index)

Detail lives in `memory/YYYY-MM-DD.md`; this file is the load-bearing rules only.

## Identity / workflow
- Solo C++23 Windows engine by Abdal (@abdallah2183), repo `SANAD-Engine`, branch `main`,
  namespace `nf::`, CLI `nf`. **No commit/push without explicit user request.** Never delete
  `.workbuddy-ai`.
- Arabic UI/content is OK. Arabic in code identifiers/comments is a red line unless asked.
- ⚠️ **Never rewrite a source file with PowerShell `Set-Content` / `Get-Content -Raw`** — wrong
  codepage, silently destroys Arabic characters. Caught only by `/WX` C4828. Use the Edit tool.
- Game-Ready Program G1–G10 asks: can a dev ship a PC game without touching engine source?
  Briefs: `.workbuddy-ai/agents/agent-game-ready-program.md`, `agents/model-g*.md`.
- Ownership map + open Requests: `.workbuddy-ai/COORDINATION.md`.

## Build / verify
- Sandbox build: `bash .workbuddy-ai/nfb.sh [--release] [--target X]` (NOT `build_nf.bat` —
  it calls `reg.exe`, which the sandbox blacklists). Binaries: `build/DebugNinja/bin/`.
- Tests: `NF_TEST/NF_CHECK/NF_CHECK_NEAR/NF_SKIP`; register new `.cpp` in `Tests/CMakeLists.txt`
  and confirm the names in the run output (silent registration-drop has shipped 3×). **A skip is
  never a pass.** Always re-run and count — never quote a stale number.
- Last full sweep **2026-10-03: 26 suites, 1800 passed / 0 failed / 2 skipped**
  (ECSTests 1, InputTests 1 — env-gated). EditorTests 261, RHITests 257,
  PhysicsTests 217, AudioTests 119, RuntimeTests 112, ToolTests 52, UITests 42,
  PerfTests 9. Baselines 1435…1796 are stale.
- Editor acceptance: `NOVAForgeEditor.exe --scene content://Scenes/Example.nfscene --frames 125
  --validation` must exit 0 with `automation=OK` and `cube lit pixels = 229149`.
- **Log redirection trap:** PowerShell `*>` drops the logger's stderr WARN lines, so a FAILED
  check is invisible. Use `cmd /c "prog > log 2>&1"` for any run whose failures matter.
- `--headless` never records ImGui frames; UI regressions need a bounded windowed run.

## Renderer — Phase 27 post stack (§206), landed 2026-10-02
- `PostFxParams` = `saturation`/`vignette` first, then nested `bloom`/`grade`/`sharpen`. Every
  stage neutral by default, every neutral value EXACTLY identity (golden pixels depend on it).
- Bloom is a real 4-level chain (`kBloomLevels`); `bloom.{vert,frag}` serves prefilter and 13-tap
  downsample via a push-constant mode flag; the tonemap pass sums the levels with bilinear taps.
  No upsample pass, no additive blending.
- Stage order: the SHADER fetches through the lens warp first, then `sharpen → bloom add →
  exposure → grade → tonemap → gamma → saturation → vignette`. `apply_post_chain` mirrors that
  chain from the sharpen step on — the lens stage is a UV transform applied BEFORE the fetch, so
  it is mirrored by `lens_sample_uv()` instead (this function receives a colour, not a coordinate).
  Tonemap push = **80 B / 20 floats**; layout = 5 bindings.
- ⚠️ **`PipelineDesc::push_constant_size` is a REAL declaration** (it becomes the Vulkan push
  range). Growing the tonemap block without growing it — in BOTH the offscreen and the present
  tonemap pipelines — is a validation error on every frame, which shows up as 19 unrelated
  RHITests failing on `validation_error_count()`. Check it whenever a push block changes.
- **Lens effects (§206, landed 2026-10-03)**: `LensParams{enabled, distortion, chromatic_aberration}`.
  Both are UV-space warps applied BEFORE the HDR fetch, so both neutrals are EXACTLY identity.
  `lens_sample_uv()` is the CPU mirror (separate from `apply_post_chain`, which receives an
  already-fetched colour and has no coordinate to warp). Distortion composes before chroma — the
  order is pinned by a test. The shader has no enabled flag: `enabled` is folded into the two
  coefficients at push time, the way `bloom_on` folds the intensity.
- **Depth of field (§206, landed 2026-10-03)**: `DofParams{enabled, focus_distance, focus_range,
  max_radius}`. Runs INSIDE the tonemap pass — no new pipeline, render pass or graph texture —
  and reads the depth target + the frame block, both added to the **tonemap descriptor layout
  (now 7 bindings)**. `dof_coc()` is the CPU mirror. **`max_radius` 0 is the off switch**
  (`enabled` is folded into it at push time), so the shader has no flag and the golden pixels
  are untouched. `focus_range` is the distance over which the blur ramps to full width, NOT the
  half-width of a sharp band — the confusion is 0 only AT `focus_distance`.
- ⚠️ **The tonemap layout is shared by FOUR pipelines** (tonemap offscreen/present, present
  passthrough offscreen/present) and there are **TWO descriptor-set write sites** — the tonemap
  pass and `present_texture`. Both must write every binding, and binding 5 is a UNIFORM BUFFER:
  the present site's image loop cannot cover it, so it is special-cased. An unwritten binding is
  not a valid set; writing an image view into a uniform-buffer binding is a validation error.
- The bloom prefilter reads the RAW HDR target, so the glow is generated from the un-defocused
  image. Deliberate: routing it through a defocused copy needs a second HDR target for a
  difference invisible once the glow is already a wide blur.
- **Motion blur (§206, landed 2026-10-03)**: `MotionBlurParams{enabled, intensity, max_length}`.
  CAMERA motion blur from **depth reprojection** — the world position (from depth) is projected
  with the PREVIOUS view-projection, so no velocity buffer and no extra gbuffer attachment are
  needed. A surface's own movement is not in the depth buffer, so a fast object under a still
  camera does not smear; per-object motion needs a renderer change. `motion_smear()` /
  `motion_prev_uv()` are the CPU mirrors; **`intensity` 0 is the off switch**.
- ⚠️ **`FrameUniforms::prev_view_proj` sits at offset 80, in the MIDDLE of the block**, so the
  tonemap pass can declare a std140 PREFIX (invViewProj + camPos + prevViewProj = 144 B) instead
  of redeclaring forty floats. Every member after it moved by one mat4 **on both sides** —
  `Renderer3D.hpp`'s `static_assert(offsetof(...))` block and `brdf.glsl`'s `FrameUniforms`.
  Those asserts are the safety net: they fail loudly on a one-sided edit.
- **`kMotionTaps` / `kDofTaps`** must match `tonemap.frag`'s constants.
- `apply_post_chain` mirrors the whole tonemap fragment body so the ORDER is testable.
- Scene data: `PostProcessComponent` + optional `PostProcess:` line; `extract_post_process()`
  pushes on the render path beside `extract_sky()`; no component ⇒ renderer state untouched.
- Deferred on purpose: **NOTHING — §206 IS COMPLETE as of 2026-10-03.** Exposure, colour
  grading (parametric AND LUT), bloom, DOF, motion blur, lens effects, tonemapping,
  sharpening, saturation and vignette are all implemented, all reachable from a scene, and
  exposure + the tonemap operator are scene data too. See `Docs/Phase27_Plan.md`.
- **Exposure and the tonemap operator are OPT-IN scene data.** `PostProcessComponent::exposure`
  (0 = not authored) and `::tonemap` (-1 = not authored) are the sentinels; `extract_post_process`
  applies them only when set, so a scene that says nothing leaves the renderer's setting alone —
  which is what keeps the editor's golden pixels intact. They are NOT part of `PostFxParams`
  (the renderer holds them separately), so they are applied after `set_postfx`. The writer emits
  `exposure=`/`tonemap=` only when authored. `Runtime::set_tonemap_mode` is the narrow wrapper
  (like `set_exposure`); the operator is a closed set, so an unknown value is REFUSED.
- ⚠️ Localization: a value that is legitimately IDENTICAL in both languages (a proper noun like
  the operator names ACES / Reinhard) must be added to `same_in_both_languages()` in
  `Tests/UITests/test_localization.cpp`, or `localization_every_key_has_both_languages` and
  `localization_arabic_values_carry_real_arabic` both fail. `latin_allowed_in_arabic()` is the
  OTHER list — for Arabic values that merely contain a Latin token.
- **Colour-grading LUT (§206, landed 2026-10-03)**: a `kLutSize^3` cube (16³) stored as a 2D
  STRIP — 256x16 — because the RHI's sampled views are 2D; the shader does the slice blend.
  The texture arrives through `Renderer3D::set_color_lut(view)` (NOT owned; the runtime passes
  the view from its path-keyed texture cache) and is bound at **tonemap binding 7** (the layout
  is now **8 bindings**). `PostFxParams::lut_strength` 0 is exactly identity.
  ⚠️ **The strength is FOLDED TO 0 at push time when no LUT is bound** — the slot then holds the
  1x1 white texture, so an authored strength with no LUT would grade the whole frame toward
  white (the same trap `bloom_on` avoids). The runtime resolves `lut_path` once per CHANGE
  through `ensure_texture`, and refuses an image whose size is not exactly 256x16.
  `apply_color_lut()` is the CPU mirror; it uses a NEAREST texel, so only a constant LUT (and
  strength 0) compare exactly against the shader — the tests are built around those.

## Emission colour fix (uncommitted, 2026-10-02)
- `.nfmat emission.rgb` was parsed/saved/round-tripped and **never read**: `lighting.frag` used
  `albedo * emission_strength`, so a dark base colour + bright emission rendered black.
- Fix: new gbuffer attachment **3** (emissive radiance, `emission.rgb * strength`), lighting
  binding **7**, `forward.frag` reads `matParams.misc.rgb`. `PBRMaterialParams::pack()` resolves
  an all-zero emission colour to `base_color` — so it is a strict generalisation: every material
  that set only a strength renders bit-identically. `emission: 0 0 0` + strength>0 glows with the
  base colour (use strength=0 for "no glow").
- ⚠️ This changes brightness for any scene with `emission_strength > 0` ⇒ needs a golden-pixel
  re-shoot of the editor acceptance, not a drive-by edit.

## Scene / assets / runtime facts
- **Names are NOT unique, and nothing may assume they are.** Runtime synthesizes
  a name from the entity KIND for every entity with no `Name:` line, so a scene
  with two meshes has two entities called `"Mesh"`. `scenes_equal_structure` must
  pair duplicates positionally (fixed 2026-10-03); the gallery/launcher and
  `Panels.cpp` also work around the same collision.
- **The New Project gallery must offer every template `nf new` accepts, and name
  none it does not.** The table is `kLauncherTemplateCards` in
  `NF/Editor/ProjectLauncher.hpp` (header-inline so EditorTests can pin it) and
  `EditorTests/test_project_launcher.cpp` pins both directions. This defect class
  has shipped twice — check it before touching the table.
- **`Runtime::renderer()` is const by design.** Renderer settings the editor may
  write get a narrow `Runtime` wrapper instead (see `set_exposure`). Exposure is a
  display preference, not scene data — never persisted per scene, because the
  editor's golden pixel count is pinned to its default.
- `.nfscene` supports **19** component lines — re-derive by grepping
  `line.rfind("  <Name>:",0)==0` in `RuntimeSceneLoader.cpp` (the count has gone stale twice):
  Name, Transform, Camera, Light, Sky, TimeOfDay, PostProcess, Mesh, RigidBody, Collider,
  Animation, Audio, Destructible, Particles, Cloth, Character, Script, Module, Prefab.
  `id = block index`; after `entity_count` each block needs `---` and `entity: <id>:0`.
- **Always give every entity a `Name:` line.** `Runtime.cpp:385-410` synthesizes a name from the
  entity KIND, so two meshes are both `"Mesh"` — invisible until it breaks the editor's
  save/load structure check.
- Mesh import has one entry point `nf::assets::import_mesh_file` (extension wins, content sniff
  fallback, contradiction fails). `.nfmat` is the only material route; `.nfmesh` is geometry only.
- Scene loader creates only directional lights. Material has one albedo texture; normal/
  roughness/ORM maps are not supported directly.
- Scene collider is centered on entity origin; many kit meshes do not pivot at visual center.
- Transform components use `local_x/y/z`, `world_x/y/z`, `dirty`; write local then propagate.
  Entity id 0 is valid but invisible to GPU picking.
- `uv1` is ONE attribute with two consumers (terrain splat `(slot,blend)`, water
  `(foam,reflection)`) — not a conflict. **The DEFERRED path already paints the splat**:
  the vertex layout supplies uv1 at location 3, `gbuffer.frag` samples `SplatPalette`
  (binding 2) when `uv1 != (0,0)`, and the renderer exposes `set_splat_layer_color`.
  The **forward/transparency** path deliberately stops at uv0 (`forward.vert` has no
  palette), so water foam/reflection is still unpainted. Splat layer colours are reachable
  from C++ only, and terrain is not scene-authorable (no Terrain component in `.nfscene`).

## Editor UI rules
- Theme roles in `Editor/include/NF/Editor/UiTheme.hpp`; palette/style in `UiBackend.cpp`.
  One UI font size for Latin+Arabic.
- ImGui widget ID = label. Any repeated visible label needs `##suffix` — and **the suffix must be
  UNIQUE, not merely present** (the ID is the text AFTER `##`). `Scripts/audit_imgui_ids.py` only
  flags *bare* labels; ImGui's duplicate detector is hover-based, so no automated run sees a
  repeated suffix. Run `python Scripts/audit_imgui_ids.py`.
- Arabic localization in `Engine/UI/src/Localization.cpp`; keys must be duplicate-free
  (`UITests::localization_keys_are_unique`). Use `AV(key)` for displayed shaped labels,
  `ui::tr(key)` for logic/stored names.
- Default dock layout in `Panels.cpp`; persisted layout only for human sessions — scripted runs
  are deterministic.

## Audio
- **Scene audio environment (landed 2026-10-03).** `ReverbZone:` / `Music:` /
  `Ambience:` are ENTITY component lines, not scene-level records — scene v1 has no
  place for a global list (`---` must follow `entity_count`), and a reverb zone's
  position IS the entity's transform. `Runtime::build_scene_audio()` runs on scene
  adopt and **REPLACES** the previous scene's environment (clears zones, stops
  unnamed music/ambience). That is the opposite of the post-process "absent means
  unchanged" rule, on purpose: these are per-scene artefacts, not renderer state a
  caller may have configured directly.
- Music/ambience samples are copied into **runtime-owned** buffers
  (`m_scene_music_buffer` / `m_scene_ambience_buffer`): `MusicSystem` holds the
  buffer pointer across frames and the ECS world can reallocate, so pointing at a
  component's `owned_buffer` would dangle.
- `AmbienceComponent` has **no volume field** on purpose — `set_ambience` takes no
  volume, so one would be parsed, saved and never read.
- ⚠️ Editor UI: three sections (Reverb Zone / Music / Ambience) with add, edit,
  undo/redo and Arabic keys. Music and ambience need a buffer path, so their
  EMPTY state offers the path field — they are deliberately NOT in the
  Add-component menu, because a component naming no file is refused by the
  setter. `EditorApp::set_music/set_ambience` resolve the path through the VFS
  (so an authored track is audible without a save/reload) and carry the decoded
  samples over when the path is unchanged — an edit must not silence a track.
- `Runtime::step_audio` mixes through ONE `audio::AudioScene` (`begin_block → mix_emitter →
  finalize`). A game reaches the mixer with `rt.audio_scene().settings()`.
- `AudioComponent` carries `sample_cursor` AND `sample_position` (f64) — the integer alone drifts.
  `Runtime` keeps persistent `Emitter`s in `m_audio_emitters` keyed by entity.
- Game settings bridge: `Engine/Audio/include/NF/Audio/GameSettingsBridge.hpp`
  (`settings.apply_to(rt.audio_scene().settings())`). In the settings file the KEY is trimmed,
  the VALUE is verbatim (so `"0.50x"` cannot silently become 0.5) — do not "fix" the value side.

## Performance / gotchas
- Frame time scales with DISTINCT MESHES, not entity count — but measured **~1.46×** (240 ent,
  1 vs 64 meshes), NOT the reported 5×. Never quote 5×. Rows `mesh_law_*_ms` in
  `Tests/PerfTests/perf_baseline.csv` at 25% tolerance.
- `nf::clamp` is float-only. `/W4 /WX` is on: warnings are errors.
- Determinism is an explicit contract in AI/2D/physics: no RNG, no clock, ids not pointers,
  `dt` clamped ≥0, insertion order is visit order.
- Git under OneDrive: after commits run `bash Scripts/git_repair_ref.sh`; avoid `git stash`;
  push often.
