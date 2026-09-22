# NOVAForge / SANAD Engine — Project Memory

## Identity
- Solo C++23 engine by Abdal (@abdallah2183). Repo `SANAD-Engine`, branch `main`, v0.1.0. Namespace `nf::`, types `NF`-prefixed, CLI `nf`.
- Docs in sync: design doc, `Docs/PhaseN_Plan.md`, `ROADMAP.md`, READMEs (en/ar), `website/index.html` + root `index.html`.
- **Game-Ready Program:** tracks G1–G10, one model each; criterion: "can a dev finish & ship a PC game without touching engine source". Ownership `.workbuddy-ai/agents/agent-game-ready-program.md`; briefs `agents/model-g*.md`. No commit without lead order; Arabic in code = red line; PC/Windows only. G9 = editor subset only; G10 = stability gatekeeper (green-or-rejected).

## Build / verify
- **Sandbox: `bash .workbuddy-ai/nfb.sh [--target X]`, NOT `build_nf.bat`** (vcvars→`reg.exe` sandbox-blacklisted → C1083 stddef.h / LNK1104 ole32.lib = uninitialised env, not a source break). Also restores Windows profile vars so `dotnet build` (NFCSharpSandbox) doesn't die in NuGet.targets; needs `env` for `ProgramFiles(x86)`. Outside sandbox: `.\build_nf.bat --target <T>`; binaries `build/DebugNinja/bin/`.
- Tests: `NF_TEST/NF_CHECK/NF_CHECK_NEAR/NF_SKIP`; register in `Tests/CMakeLists.txt`; skip ≠ pass; confirm new test names in run OUTPUT (silent registration drop). Counts drift — re-run and count, never quote from memory. Last green 2026-09-22: 1540/0/2 across 26 suites.
- Sweep in background: `for exe in build/DebugNinja/bin/*Tests.exe; do timeout 240 "$exe" | tail -6; done`. `RHITests` intermittent flake (exit 139), not a regression.
- Shaders: GLSL in `Samples/Basic3D/shaders/` (lighting.frag + forward.frag share brdf.glsl), compiled by build (CMake/NFShaders.cmake — DEPENDS globs *.glsl, so brdf.glsl edits rebuild includer .spv) AND at runtime via ShaderManager (glslc from VULKAN_SDK).

## Hard-won rules (condensed)
- Rule 0: green suite ≠ integration; `Runtime::update()` is ground truth (physics→animation→audio→gameplay→script→propagate).
- Transform: `local_x/y/z`, `world_x/y/z`, `dirty`; write local then propagate. No `position` member.
- Unused API is untested API — grep `Tests/` first. f32 needs 9 sig digits; `.nfscene` setprecision(9).
- Determinism: no unseeded RNG, sort before serializing; assert exact `!=` component-wise. MSVC /W4 /WX: zero-init, no shadowing, explicit casts.
- Jolt: `state()` = `GetPosition()` never COM; `ShapeSettings` ≠ `Shape` (need `.Create()`); `CompoundPart` own shape vocab; pose matrices `rotation * translate` (row-vector) — assert all 3 axes.
- `StaticMesh` ctor has empty LOD 0 — use `lod(0)`. Measurement windows can clip what they measure.
- Arabic loc: display `AV(key)` (shaped), logic `ui::tr(key)`; label audit = ImGui literal sweep; after bulk AV() grep `= {AV(`; no `%s` in shaped strings; stored names `ui::tr`; duplicate keys shadow (first match). Latin allowlist: logs/telemetry/FPS/NOVAForge/`content://` + few hints.
- `Toolbar.cpp` compiles into editor app, not `NFEditorCore` — testable symbols in `NFEditorCore`/inline headers.
- Editor startup order (VFS→Runtime→UI much later). Second `vfs.mount` of same scheme is silent — tests use own scheme (`rt://`).
- EditorApp helpers push no undo (except `create_primitive`); entity id 0 VALID, invisible to GPU picking; delete = immediate+undoable. Known open: gizmo-drag undo on STATIC body.
- `.nfscene`: ONE `#` comment (header); `Example.nfscene` entity order load-bearing (`find_first_mesh` = `front()`).
- After UI changes run editor headless (`NOVAForgeEditor.exe --headless --frames 125 --validation`); 4 pre-existing failures — compare by NAME.
- Git under OneDrive: `bash Scripts/git_repair_ref.sh` after every commit. NEVER `git stash` — a sync race can restore stale refs and clobber objects/packs (2026-09-22: ~14 unpushed commits' metadata lost, tree survived). Push often; A/B via explicit file copies; recovery recipe in 2026-09-22.md.

## Deferred debt
- Hand-written components → reflected path (blocked on format change); CPU-only animation. Open: procedural clouds; `.nfscene` TimeOfDay keys.

## Workflow
- Branch from `main`; `feat/fix/docs(scope):`; **no commit/push without explicit request**. Smallest viable diff; build + suite; docs get real numbers. Never delete `.workbuddy-ai`.

## G8 perf gate (2026-09-22, machine `win11-x64-dev`)
- Scene `Content/Scenes/Perf.nfscene`: 2053 entities / 2050 meshes / 24 crates. Gate: `Tests/PerfTests/perf_baseline.csv` via `perf_gate.sh` + CI workflow; any metric >10% over its row fails. Rows tagged `any` enforced everywhere; `win11-x64-dev` rows only on this box (frame time is machine-dependent, never cross-compared).
- `bash Tests/PerfTests/perf_gate.sh [--record|--self-test]`. Baselines: 2050 mesh entities, 1742 visible, 3484 draw calls, Δmem 31.2 MB, ws 91.5 MB, frame 87.0 ms, update 29.4 ms. Spread ≤2.6% — gate doesn't flake.
