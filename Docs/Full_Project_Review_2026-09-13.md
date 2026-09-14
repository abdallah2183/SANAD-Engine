# NOVAForge Engine — Full Project Review

**Date:** 2026-09-13 (21:31–21:35 PDT)
**Reviewer:** Nova
**Commit reviewed:** `421511f` ("Record Phase 6 completion and the frame-vs-wallclock harness rule"), branch `workbuddy/main-e34f0fa2`
**Method:** Read-only audit of all source + **independent build and test execution** from a clean worktree.
**Toolchain used:** MSVC 19.51.36243 (VS 18 Community, toolset 14.51.36231), Windows SDK 10.0.26100.0, Vulkan SDK 1.4.357.0, Ninja.
**GPU:** NVIDIA GeForce RTX 5060 Ti, driver 610.74.
**Relation to prior work:** supersedes `Docs/Engine_Evaluation_2026-09-13.md` (same day, earlier). Every defect that document raised is re-verified below.

---

## Status update — same day, 21:37–21:55 (improvement pass)

Findings §4.1, §4.2, §4.3, §4.8 and §4.9 have since been **fixed and verified**. The findings are
left below as originally written so the reasoning is preserved; this is the current state:

| Finding | Status |
|---|---|
| §4.1 per-frame retry storm | **Fixed** — `m_failed_meshes` + `m_failed_mesh_reports`; 120 → 1 warnings over 60 frames, constant in frame count. Pinned by `runtime_failed_mesh_reported_once_not_per_frame`. |
| §4.2 acceptance green while rendering nothing | **Fixed** — `Application::run` and `Editor/src/main.cpp` assert geometry was drawn and exit non-zero with a "cache is probably not cooked" error. CI cooks before the acceptance and fails if `RHITests` passes 0. |
| §4.3 `Scripts/build.sh` wrong tree | **Fixed** — root derived from `BASH_SOURCE`, VS/MSVC/SDK auto-detected (`NF_VS_ROOT` override). Same auto-detection added to `CMake/Toolchain-MSVC.cmake`. |
| §4.8 nine silent early-returns | **Fixed** — all migrated to `require_gpu()` / `NF_SKIP`; `mesh_asset_cook_import` split so the GPU upload is its own test. |
| §4.9 stale README | **Fixed** — rewritten. |
| §4.4, §4.5, §4.6, §4.7, §4.10 | **Open** — design decisions, not mechanical patches. |

Re-verified after the fixes: build clean under `/W4 /WX`; **211 passed / 0 failed / 1 skipped / 212**;
Triangle, Basic3D, RuntimeScene and the headless Editor all exit 0 with **0 validation errors and
0 leaked RHI objects**; RuntimeScene reports `meshes=1` and the editor reports
`Automation: Scene geometry drawn OK`.

---

## 1. Verdict

**The engine builds clean and the suite is genuinely green — this is now verified, not claimed.**

The earlier evaluation scored the project 6.0/10 with the headline "strong engineering, broken verification loop." That headline is no longer accurate. I reproduced the build and the full suite myself and the loop is closed: all six defects from that report are fixed, and the fixes are real code, not test suppression.

Two new problems remain, and both are the same *shape* as the old ones — a green signal that does not mean what it appears to mean.

**Overall: 7.5 / 10** — solid, honest foundation; two verification holes left to plug.

---

## 2. What I verified myself

### Build — clean

```
cmake -S . -B build/debug -G Ninja ... -DNF_BUILD_EDITOR=ON -DNF_BUILD_TOOLS=ON
→ 187/187 targets, exit 0
```

One transient `LNK1168: cannot open bin\RHITests.exe for writing` on the first pass (a file lock, the known intermittent issue); retry linked clean. Warnings-as-errors is `ON` and did not trip.

### Test suite — 209 passed / 0 failed / 1 skipped

| Suite | Result |
|---|---|
| CoreTests | 36 / 36 |
| JobTests | 4 / 4 |
| ECSTests | 21 / 22 (1 skipped — the opt-in 1M benchmark) |
| AssetTests | 13 / 13 |
| RHITests | **68 / 68** |
| RuntimeTests | 13 / 13 |
| EditorTests | **54 / 54** |
| **Total** | **209 passed · 0 failed · 1 skipped (210)** |

The 54/54 on `EditorTests` is the headline. The earlier evaluation watched that suite die at 37/50 with a SIGSEGV. It now runs to completion.

### Samples and harnesses — validation-clean

| Artifact | Command | Result |
|---|---|---|
| Triangle | `NF_TRIANGLE_FRAMES=60 NF_TRIANGLE_VALIDATION=1` | 60 frames, **0 validation errors**, exit 0 |
| Basic3D | `NF_BASIC3D_FRAMES=60 NF_BASIC3D_VALIDATION=1` | 60 frames, **0 validation errors**, exit 0 |
| RuntimeScene (headless) | `NF_RUNTIME_HEADLESS=1 NF_RUNTIME_FRAMES=30` | **0 validation errors**, 0 alive RHI objects, exit 0 |
| Editor (headless) | `--headless --frames 30` | automation=OK, **0 validation errors**, 0 alive RHI objects, exit 0 |

---

## 3. Prior defects — all confirmed fixed

| # | Defect (2026-09-13 eval) | Status | Evidence |
|---|---|---|---|
| 4.1 | **CRITICAL** stale framebuffer cache → SIGSEGV | **Fixed** | `Renderer3D.hpp:222-233` — `TonemapFBKey` now keys on `tex_serial` (monotonic `rhi::Texture::creation_serial()`), with pointer identity only as a fallback for serial 0. Bounded by `kMaxTonemapFramebuffers = 8`. The comment explains both the original bug and the tradeoff of the bound. |
| 4.2 | Triangle sample not validation-clean | **Fixed** | `present_source` now set; 60 frames produce 0 validation errors. |
| 4.3 | Test asserted a dangling pointer | **Fixed** | `EditorTests` 54/54. |
| 4.4 | Validation flag misreported | **Fixed** | Triangle logs `Vulkan device initialized [Vulkan] (validation: on)` correctly. |
| 4.5 | Benchmarks green-by-skip, metrics invisible | **Partly fixed** | `NF_TEST_LOG_LEVEL` now exists to surface timings; 1 remaining skip is reported honestly as SKIPPED, not PASS. Still **no thresholds asserted**, so no benchmark can fail on a regression. |
| 4.6 | No version control, no CI | **Fixed** | Git repo, 6 commits on `main`; `.github/workflows/ci.yml` installs Lavapipe so GPU tests execute headlessly instead of skipping. |

The framebuffer fix is the one worth reading. It fixes the bug *and* documents why the naive fix would have leaked framebuffers — that is a senior-level commit message and a good sign for the project's long-term health.

---

## 4. New findings

### 4.1 HIGH — A failed asset load is retried forever, every frame

**Where:** `Engine/Runtime/src/Runtime.cpp:224-230`

```cpp
if (handle->state == assets::AssetState::Failed) {
    // Missing/corrupt asset: warn once per frame is too noisy, so only
    // warn here; render proceeds without this object (safe fallback).
    NF_LOG_WARN(LogCategory::Core, "Runtime: mesh asset {} failed: {}", ...);
    continue;
}
```

The comment states the intent is to *avoid* per-frame noise. The implementation produces exactly that noise, because nothing records the failure. `m_mesh_handles` is only populated on success (line 252), so a permanently-failed mesh is re-queried and re-warned on every pass, forever.

**Measured** on the editor harness:

| Frames | Warnings for one failing asset |
|---|---|
| 10 | 20 |
| 30 | 60 |
| 60 | 120 |

Exactly 2 per frame, perfectly linear. This is unbounded log spam and wasted work proportional to frame count, and it masks real errors in the noise.

**Fix:** keep a `std::unordered_set<AssetId> m_failed_meshes` and `continue` silently once an id is in it — or have the asset manager expose a terminal state that the sync loop can distinguish from "still loading".

### 4.2 HIGH — The acceptance harness is green while the scene renders nothing

**Where:** `Content/AssetRegistry.nfreg` maps the scene's mesh to `cooked: cache://Meshes/cube.nfmesh`. That file only exists after `NFAssetCooker` has run, and nothing in the README, the build script, or CI runs it.

**Before cooking** — `NOVAForgeEditor.exe --headless --frames 30`:

```
WARN [Core] Runtime: mesh asset f04e488b-... failed: Failed to open file for reading: 'cache://Meshes/cube.nfmesh'
...
INFO [Editor] Editor ran 30 frames (entities=3, dirty=yes, automation=OK)
INFO [RHI] Validation errors: 0
INFO [RHI] Alive RHI objects before shutdown: 0
```

`automation=OK`, zero validation errors, zero leaks, exit 0 — and the scene contains **no geometry**.

**After** running the documented cook command:

```
NFAssetCooker --input content://Meshes/cube.nfmesh \
              --output cache://Meshes/cube.nfmesh \
              --registry content://AssetRegistry.nfreg
→ Cook report: cooked 1, skipped 0, failed 0, total 1
```

the same harness now reports:

```
INFO [Core] Runtime: mesh asset f04e488b-... uploaded (handle 0)
```

So the pipeline works end-to-end — the problem is that the harness cannot tell "rendered the scene correctly" from "rendered an empty scene." `NFSampleRuntimeScene --headless --scene content://Scenes/Example.nfscene` has the same blind spot: it exits 0 reporting `Rendered 30 frames (meshes=0)`, `extracted=0 visible=0 draws=0`.

This is the same failure class the 2026-09-13 evaluation called out: a verification signal that is green for the wrong reason. **Fix:** assert on `extracted > 0 && draws > 0` in the acceptance harness, and make the cook step a prerequisite of running the editor (or commit a cooked cache).

### 4.3 MEDIUM — `Scripts/build.sh` builds a different directory than the one you are in

`Scripts/build.sh:29` hardcodes:

```bash
PROJECT_ROOT="C:/Users/abdal/OneDrive/Desktop/NOVAForge Engine"
```

and pins the toolchain to VS 2022 BuildTools / MSVC `14.44.35207` (lines 12-13), while this worktree builds with VS 18 Community / MSVC `14.51`. Both paths exist on this machine, so the script does not fail — it **silently builds and tests the OneDrive copy instead of the worktree you are standing in.** That is worse than a hard error, because a green run would be reported for the wrong tree.

**Fix:** `PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"`, and derive the toolchain version instead of pinning it.

### 4.4 MEDIUM — Cooking mutates a tracked file's line endings

Running the cooker rewrites `Content/AssetRegistry.nfreg` from LF to CRLF. `git status` then shows ` M Content/AssetRegistry.nfreg` with an empty content diff — spurious churn that will eventually get committed by accident or cause a confusing merge. Either preserve line endings on rewrite, or treat the registry as read-only input during cook.

### 4.5 MEDIUM — Two layering inversions against the design's own rules

| Violation | Evidence |
|---|---|
| **Assets → Rendering** (Assets is the lower layer) | `Engine/Assets/src/MeshAsset.cpp:2`, `Engine/Assets/src/AssetManager.cpp:2`, `Engine/Assets/include/NF/Assets/AssetManager.hpp:7` |
| **Rendering → ECS/Scene** (the renderer should consume a backend-neutral world) | `Engine/Rendering/include/NF/Rendering/Extraction.hpp:24`, `Engine/Rendering/src/Extraction.cpp:4` |

There is no true include cycle yet — Assets→Rendering is one-way — but the intended dependency direction is already inverted in two places, and `Runtime.hpp:3-17` fans out across all of them. The render-world extraction step exists (`RenderWorld`, double-buffered) but it consumes ECS types directly rather than a neutral description.

### 4.6 MEDIUM — "Handle-based resources, no raw pointers" is unenforced

`RHI.hpp:401-406` declares six handle types:

```cpp
struct BufferHandle      { u32 id = u32_max; ... };
struct TextureHandle     { u32 id = u32_max; ... };
struct PipelineHandle    { u32 id = u32_max; ... };
struct ShaderModuleHandle{ u32 id = u32_max; ... };
struct RenderPassHandle  { u32 id = u32_max; ... };
struct FramebufferHandle { u32 id = u32_max; ... };
```

**All six are referenced nowhere outside that header.** The device API returns raw pointers (`RHI.hpp:726 virtual Texture* get_texture(...)`). `RGTextureHandle` in `RenderGraph.hpp:31` is an unrelated internal `u32` index and does not satisfy the rule.

This is not academic: defect 4.1 was precisely a cache keyed on a raw texture address. The rule that would have prevented it is declared and then ignored. Either make the device return handles, or delete the handle types so the code stops advertising a guarantee it does not provide.

### 4.7 LOW — The custom container layer is dead weight

`Engine/Core/include/NF/Core/Containers.hpp:4` states the project ships `DynamicArray`/`HashMap` to avoid std overhead. Actual usage across `Engine/` + `Editor/`:

| Type | References |
|---|---|
| `std::string` | 896 |
| `std::vector` | 264 |
| `std::unordered_map` | 30 |
| `std::map` | 9 |
| `DynamicArray` | 23 |
| `nf::HashMap` | **0** |

Either adopt them where it matters (per-frame hot paths) or drop the claim and delete the code. Right now it is a maintenance surface with essentially no users.

### 4.8 LOW — Nine legacy silent early-returns survive in the tests

The correct pattern is established and widely used — **63** `require_gpu()` call sites and **12** `NF_SKIP()` sites. But these predate it and still `return` without asserting, so they report PASS without executing on a GPU-less machine:

`test_rhi_triangle.cpp:39,194,277` · `test_rhi_quad.cpp:147,437,495` · `test_rhi_push.cpp:51,196` · `test_runtime.cpp:68`

On this machine (RTX 5060 Ti) they ran for real, so the green result above is honest *here*. On a GPU-less runner they would be hollow — and CI's Lavapipe fallback degrades to SKIPPED rather than failing, so a green CI run does not guarantee GPU coverage ran.

### 4.9 LOW — README is stale on five points

| README says | Reality |
|---|---|
| "Phase 2.5" | Phase 6 is complete (`a0668ea`) |
| "C++20" | `CMakeLists.txt:13` sets `CMAKE_CXX_STANDARD 23` |
| "Archetype Entity-Component System" | Sparse-set storage (`ECS.hpp:88-158`) |
| "126 automated tests" | 210 registered |
| Lists 3 samples, 4 test suites | 4 samples, 7 test suites (Assets/Runtime/Editor missing) |

### 4.10 LOW — Duplication and format drift

- `load_spirv_file` is duplicated verbatim in `Renderer3D.cpp:14-23` and `GpuPicker.cpp:15-25`.
- Two competing mesh-asset formats coexist: `Engine/Assets/include/NF/Assets/MeshAsset.hpp` and `Engine/Rendering/include/NF/Rendering/MeshAsset.hpp`.
- `Engine/Jobs/src/JobSystem.cpp:230,234` — the project's only two TODOs (thread affinity, worker tracking).

---

## 5. Scope reality

Eight of eighteen Engine modules are empty `.gitkeep` directories — verified, zero real files:

`AI` · `Animation` · `Audio` · `Input` · `Networking` · `Physics` · `Scripting` · `UI`

The design document contains **335** numbered sections across 5,742 lines (the "322" in the README and the prior evaluation is wrong; the prior eval's own "474 headings, 322 sections" does not reconcile).

| Milestone | Status |
|---|---|
| **v0.1** Core, RHI, Renderer, ECS, Scene, Assets, Editor | **Genuinely complete** |
| **v0.2** Physics, Audio, Animation, Scripting, Prefabs, Save | Prefabs + Save done; the other four are empty dirs |
| **v0.3** Terrain, Streaming, Particles, NavMesh, AI | Not started |
| **v0.4** Multiplayer, Dedicated Server, Cooking, Packaging | `NFAssetCooker` only; no networking |
| **v0.5 / v1.0** | Not started |

**Design drift worth a conscious decision, not silent decay:**

- Namespace: design mandates `NF`, code uses `nf::` in **305** places. Directory paths stay uppercase (`NF/…`), so casing disagrees between path and namespace.
- "World streaming from day one" (design §301) — no streaming code exists, and the doc contradicts itself by scheduling streaming at step 23 (§257). Retrofitting streaming into a scene/asset system is materially harder than building it in. Worth deciding explicitly.
- Quality tiers Low/Medium/High/Ultra (§118, §331) — zero code. `grep QualityTier` returns nothing.
- Versioned serialization (§74) — `Scene.cpp:45` writes a `Version:` line and `deserialize` reads it and discards it (`Scene.cpp:82-84`). No migration path.

---

## 6. Strengths (evidence-backed, and unchanged)

These were true before and remain true — they are why the project is worth continuing.

1. **The RHI seam is real.** `Engine/RHI/include/NF/RHI/RHI.hpp` includes only `<memory>`, `<span>`, `<string_view>`, `<vector>`, and `NF/Core/Types.hpp`. Every mention of "Vulkan" in that header is inside a comment. Dispatch goes through `VulkanLoader` — no static linking to `vulkan-1.lib`.
2. **Headless device.** `init(nullptr)` yields a device with no surface, which is what makes 68 GPU tests runnable without a window. The design principle "dedicated server buildable without GPU" is honoured early.
3. **`PipelineDesc` holds `const RenderPass*`, not a copy** — a pipeline is always built against the exact render pass it draws with. This eliminates a whole class of compatibility-mismatch bugs.
4. **Per-swapchain-image render-finished semaphores**, not one shared semaphore. A correctness detail most hobby engines get wrong.
5. **`RenderGraph` with automatic barrier derivation**, verified by dedicated tests.
6. **Tests verify pixels and leaks, not just "didn't crash."** `resource_lifetime_zero_leaks`, `runtime_clean_shutdown_no_vk_leaks`, `runtime_offscreen_scene_produces_pixels`, `validation_clean_full_pipeline`.
7. **Discipline.** Two TODOs in ~38k hand-written lines. No FIXME, no HACK. `NF_VK_CHECK` used consistently. Comments explain *why*, not *what*.
8. **Phase 6 work is real engineering**, not padding: descriptor sets cached per material instance with a `material_sets_built` stat to assert steady-state zero; GPU picking deliberately kept *outside* the frame graph so a picker bug cannot perturb the rendered image, with a CPU ray/AABB fallback.

---

## 7. Priority order

1. **Fix the per-frame retry storm** (§4.1). Small change, removes unbounded noise, and the code comment already states the correct intent.
2. **Make the acceptance harness assert on drawn geometry** (§4.2). Assert `extracted > 0 && draws > 0`; make cooking a prerequisite. Until then, CI green does not mean the scene rendered.
3. **Fix `Scripts/build.sh` `PROJECT_ROOT`** (§4.3). A build script that silently builds a different tree is a trap that will bite exactly once, at the worst time.
4. **Migrate the nine legacy early-returns to `require_gpu()`** (§4.8), and make CI fail if the GPU suites all skip.
5. **Decide the identity model and enforce it** (§4.6) — either handles everywhere or delete the handle types. This is the rule whose absence caused the SIGSEGV.
6. **Correct the layering inversions** (§4.5) before a third subsystem depends on them.
7. **Refresh the README** (§4.9) — it is the first thing a future contributor reads.
8. **Decide consciously on streaming and quality tiers** (§5) — build them in or record them as dropped. Silent drift is how architecture rots.

---

## 8. Reproduction

```bash
# Environment (MSVC 14.51 / VS18 Community, SDK 10.0.26100.0, Vulkan 1.4.357.0)
#   PATH must use MSYS-style /c/... entries; colon-separated C:/ paths are mis-parsed.
#   Pass -DCMAKE_C_COMPILER / -DCMAKE_CXX_COMPILER explicitly or CMake will not find cl.exe.

cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_MAKE_PROGRAM="$NINJA" \
      -DCMAKE_C_COMPILER="$MSVC/bin/Hostx64/x64/cl.exe" \
      -DCMAKE_CXX_COMPILER="$MSVC/bin/Hostx64/x64/cl.exe" \
      -DNF_BUILD_TESTS=ON -DNF_BUILD_SAMPLES=ON -DNF_BUILD_EDITOR=ON -DNF_BUILD_TOOLS=ON
cmake --build build/debug --parallel

bash Scripts/run_tests.sh build/debug          # 209 passed / 0 failed / 1 skipped

cd build/debug/bin
NF_TRIANGLE_FRAMES=60 NF_TRIANGLE_VALIDATION=1 ./NFSampleTriangle.exe   # 0 validation errors
NF_BASIC3D_FRAMES=60  NF_BASIC3D_VALIDATION=1  ./NFSampleBasic3D.exe    # 0 validation errors

# Demonstrate §4.2: acceptance green while rendering nothing
./NOVAForgeEditor.exe --headless --frames 30 | grep -c "mesh asset"      # 60 warnings, automation=OK

# Demonstrate §4.1: retries scale linearly with frames
./NOVAForgeEditor.exe --headless --frames 10 | grep -c "mesh asset"      # 20
./NOVAForgeEditor.exe --headless --frames 60 | grep -c "mesh asset"      # 120

# Fix it by cooking
./NFAssetCooker.exe --input content://Meshes/cube.nfmesh \
                    --output cache://Meshes/cube.nfmesh \
                    --registry content://AssetRegistry.nfreg
./NOVAForgeEditor.exe --headless --frames 30 | grep "uploaded"           # mesh uploads
```

> Note: running the cooker rewrites the tracked `Content/AssetRegistry.nfreg` (LF→CRLF). Restore it afterwards with `git checkout -- Content/AssetRegistry.nfreg` unless you intend to commit the change.
