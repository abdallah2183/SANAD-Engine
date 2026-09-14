# NOVAForge Engine — Codebase Evaluation

**Date:** 2026-09-13  
**Scope:** Actual source, build outputs, and live test execution (not the design document)  
**Build inspected:** `build/DebugNinja` (rebuilt 2026-09-13 18:25–18:33)  
**GPU:** NVIDIA GeForce RTX 5060 Ti · Vulkan SDK 1.4.357.0

---

## 1. Verdict

The foundation is real and unusually disciplined for a solo project. The abstraction boundaries  
hold, the test suite is serious, and v0.1 scope is genuinely built — not stubbed.

**But the project's central health claim is currently false.** The suite is not green: three  
GPU tests hard-crash with SIGSEGV, one fails on a flawed assertion, and the flagship triangle  
sample is not validation-clean. There is also no version control and no CI.

**Overall: 6.0 / 10** — strong engineering, broken verification loop.

---

## 2. Measured Scale

| Component                       | Hand-written LOC |   Files |
| ------------------------------- | ---------------: | ------: |
| `Engine/`                       |           18,787 |     119 |
| `Editor/`                       |            7,500 |      34 |
| `Tests/`                        |            8,961 |      48 |
| `Samples/`                      |            1,065 |       4 |
| `Tools/`                        |              352 |       1 |
| **Total hand-written**          |       **36,665** | **206** |
| `ThirdParty/` (Dear ImGui, stb) |           76,368 |      15 |

Design document: 5,742 lines, 474 headings, 322 sections.

### Implemented modules

`Core` · `Jobs` · `Platform` · `RHI` · `ECS` · `Scene` · `Rendering` · `Assets` · `Runtime` · `Editor`

### Empty placeholder directories (zero files)

`Engine/AI` · `Engine/Animation` · `Engine/Audio` · `Engine/Input` · `Engine/Networking` ·  
`Engine/Physics` · `Engine/Scripting` · `Engine/UI`

The directory skeleton anticipates the roadmap honestly — these are scaffolding, not  
half-finished code. `Input` is in fact implemented, but under `Platform/`, not `Engine/Input`.

---

## 3. Test Suite: Live Results

202 test cases registered, 1,245 `NF_CHECK` assertions.

| Suite           | Result                                                  |
| --------------- | ------------------------------------------------------- |
| CoreTests       | 36 / 36 ✅                                               |
| JobTests        | 4 / 4 ✅                                                 |
| ECSTests        | 22 / 22 ✅ *(see §4.5 — not all of these test anything)* |
| AssetTests      | 13 / 13 ✅                                               |
| RHITests        | 64 / 64 ✅                                               |
| RuntimeTests    | 13 / 13 ✅                                               |
| **EditorTests** | **37 / 50, then SIGSEGV ❌**                             |

**12 editor tests never execute.** The crash aborts the process mid-suite.

---

## 4. Defects Found


### 4.1 CRITICAL — Stale framebuffer cache causes SIGSEGV

**Where:** `Engine/Rendering/include/NF/Rendering/Renderer3D.hpp:217-229`

```cpp
struct TonemapFBKey {
    const rhi::Texture* texture = nullptr;   // identity == raw host address
    bool present_source = false;
    ...
};
struct TonemapFBKeyHash {
    size_t operator()(const TonemapFBKey& k) const noexcept {
        return reinterpret_cast<size_t>(k.texture) ^ ...;   // hashes the address
    }
};
std::unordered_map<TonemapFBKey, std::unique_ptr<rhi::Framebuffer>, ...> m_tonemap_fbs;
```

**Mechanism:**

1. The cache is keyed on the **address** of the output texture.
2. It is cleared only inside `destroy_resolution_dependent()` — `Renderer3D.cpp:406,414` —  
   reachable only from `resize()` and `shutdown()`.
3. `Renderer3D::resize()` early-returns when the size is unchanged (`Renderer3D.cpp:424`).
4. Therefore: render into texture A → destroy A → allocate a **new texture of the same size**  
   → the allocator hands back **the same address** → cache hit → a framebuffer built for the  
   *destroyed* texture is reused → `vkCmdBeginRenderPass` receives a dead `VkFramebuffer`  
   → validation error → **segmentation fault**.

**Observed error:**

```
ERROR [RHI] [Vulkan] vkCmdBeginRenderPass(): pCreateInfo->pAttachments[0]
VkImageView 0x520000000052 is invalid.
at Engine/RHI/src/Vulkan/Device_Vk.cpp:461
```

The garbage handles (`0x520000000052`, `0x770000000077`, `0x2260000000226`) are recycled  
addresses whose generation bits no longer resolve to a live framebuffer.

**Failing tests (3 hard crashes):**

| Test                           | File                                            |
| ------------------------------ | ----------------------------------------------- |
| `material_albedo_gpu_effect`   | `Tests/EditorTests/test_materials.cpp:230-272`  |
| `material_params_gpu_effect`   | `Tests/EditorTests/test_materials.cpp`          |
| `hotreload_mesh_rebuilds_live` | `Tests/EditorTests/test_hot_reload.cpp:173-211` |

All three share the trigger: a **helper that creates a fresh 64×64 target on every call while  
reusing one `Runtime`**. The `Runtime` is the cache owner, so the second call hits the stale entry.

**Independent proof that the allocator recycles addresses** — `test_viewport.cpp:140` fails  
for exactly this reason (§4.3). The two findings are the same root cause observed twice.

**Why this matters beyond the tests:** this is not a test-only bug. Any host application that  
swaps its render target for another of equal size — editor panel re-creation, multi-viewport,  
PiP, thumbnail rendering — will render into a destroyed framebuffer.

**Note:** this directly contradicts the design document's own rule *"Handle-based resources  
(no raw pointers)."* The RHI exposes handle-like objects, but the renderer keys caches on raw  
addresses. The rule is stated and then not enforced at the caching layer.

**Recommended direction (a design decision, so flagged rather than applied):**

- Give `rhi::Texture` a monotonically increasing **resource id / generation** and key caches on  
  that instead of the address — matches the design doc and fixes the whole class of bug.
- Or hold a `weak_ptr`/lifetime token and evict on expiry.
- Or invalidate `m_tonemap_fbs` whenever the target identity changes, not only on resize.
- Independently: **null-check the result of `create_framebuffer`** at `Renderer3D.cpp:752`.  
  It currently stores a possibly-null `unique_ptr` and dereferences it unguarded at line 756.

---


### 4.2 HIGH — Triangle sample is not validation-clean

**Where:** `Samples/Triangle/main.cpp:228-232`

```cpp
rhi::RenderPassDesc rp_desc;
rp_desc.color_attachments = std::span<const rhi::ColorAttachment>(color_attachments);
rp_desc.has_depth = false;
// present_source never set — defaults to false
```

`present_source` defaults to `false` (`RHI.hpp:271`), which makes the pass's final layout  
`COLOR_ATTACHMENT_OPTIMAL` instead of `PRESENT_SRC_KHR`.

**Result — one validation error per frame:**

```
ERROR [RHI] [Vulkan] vkQueuePresentKHR(): pPresentInfo images passed to present must be in
layout VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ... but VkImage is in
VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL.
```

(10 errors in 60 frames — the validation duplicate-report cap.)

The header documents this trap explicitly at `RHI.hpp:266-271`:

> *"Getting this wrong is silent on some drivers and a validation error on others, so it is  
> explicit rather than inferred."*

The sample gets it wrong. **Fix:** `rp_desc.present_source = true;`

**Additionally:** the Triangle sample contains **no pixel verification** — `grep` for  
pixel/readback/verify returns nothing. The recorded milestone evidence `Triangle: PASS |
Pixels: PASS` is therefore split across two different artifacts: the sample proves it runs,  
and `RHITests` (`rhi_offscreen_triangle_writes_colored_pixels`) proves pixels. That is fine,  
but the sample alone does not verify output, and the "Validation: CLEAN" line does not hold  
for it.

---

### 4.3 MEDIUM — Test asserts a dangling pointer

**Where:** `Tests/EditorTests/test_viewport.cpp:140`

```cpp
rhi::Texture* first = res.target.get();          // line 129
...
NF_CHECK(editor::ensure_viewport_target(device, state, res));   // destroys the old texture
NF_CHECK(res.target.get() != first);             // line 140 — FAILS
```

`ensure_viewport_target` calls `res.reset()` (`Viewport.cpp:15`), destroying `first`, then  
allocates the replacement. The allocator returns the same address, so the comparison fails.

The assertion encodes an allocator implementation detail, not a contract. It will pass or fail  
depending on allocator behaviour — a latent flake.

**Fix:** assert something real — that the dimensions changed, that a generation counter  
advanced, or hold the old texture alive and compare *its* validity instead of the address.

---

### 4.4 LOW — Validation flag misreported

`Tests` print `GpuFixture: device available (validation: off)` while validation-layer messages  
are actively emitted (reproducible in `AssetTests`, `RuntimeTests`, `EditorTests`). The  
fixture's reported flag does not reflect the instance's real state, which will mislead whoever  
debugs the next GPU failure.

---

### 4.5 LOW — Benchmarks cannot detect regressions

**Where:** `Tests/ECSTests/test_benchmarks.cpp:76-91`

```cpp
if (!has_env) {
    NF_LOG_INFO(LogCategory::Core, "Skipping 1M benchmark (set NF_BENCH_1M=1 to run)");
    return;                     // reports [ OK ]
}
```

Two compounding problems:

1. **Green by skip.** `benchmark_1m_entities` reports `[OK]` without running unless  
   `NF_BENCH_1M=1`. The `22/22 ECS pass` includes a no-op.
2. **Invisible metrics.** All benchmarks report through `NF_LOG_INFO`, but  
   `Tests/TestFramework.cpp` sets `set_min_level(LogLevel::Warn)` — so the timings are  
   **never printed**. Verified: `NF_BENCH_1M=1 ECSTests benchmark_1m` produces no numbers.

No thresholds are asserted, so no benchmark can fail on a performance regression. The  
`benchmark_*_render_objects` tests in `RHITests` have the same shape.

---

### 4.6 LOW — No CI, and no version control

- **No CI configuration exists** (no `.github/workflows`, no equivalent) — despite the design  
  document mandating *"Profiling and CI from the start."*
- **This is not a git repository.** `git status` → `fatal: not a git repository`.

For a multi-year solo project, absent version control is the single highest-risk item on this  
list — higher than any code defect here. Every finding above would have been caught by a  
pre-commit build plus a CI run.

---


## 5. Strengths (evidence-backed)

**RHI abstraction is genuinely clean — 9/10.** `Engine/RHI/include/NF/RHI/RHI.hpp` includes  
only `<memory>`, `<span>`, `<string_view>`, `<vector>`, and `NF/Core/Types.hpp`. Every  
occurrence of "Vulkan" in that header is inside a comment. Dynamic dispatch goes through  
`VulkanLoader` — no static linking to `vulkan-1.lib`. This is a real, working backend-neutral  
seam, not a claim.

**Headless device.** `init(nullptr)` produces a device with no surface/swapchain, which is what  
makes 64 GPU tests runnable without a window. This is exactly the design principle  
*"dedicated server buildable without GPU"* being honoured early — the right call.

**Pipeline/render-pass coupling done correctly.** `PipelineDesc` holds a `const RenderPass*`  
rather than a copy, so a pipeline is always built against the exact `VkRenderPass` it draws  
with. This eliminates a whole class of compatibility-mismatch bugs.

**Per-swapchain-image render-finished semaphores** rather than one shared semaphore — a  
correctness detail many hobby engines get wrong.

**`RenderGraph` with automatic barriers and explicit `ResourceState`** — verified by  
`rendergraph_orders_writes_before_reads_and_transitions` and  
`generic_rendergraph_barrier`.

**Test culture is serious, not decorative.** 202 cases / 1,245 assertions. Critically, the GPU  
tests verify **actual pixels** (`rhi_textured_quad_samples_uploaded_texture`,  
`game_entity_to_render_graph_pixel_verification`, `runtime_offscreen_scene_produces_pixels`),  
not merely "it didn't crash". There are explicit **zero-leak** tests  
(`resource_lifetime_zero_leaks`, `runtime_clean_shutdown_no_vk_leaks`) and validation-error  
counting. A custom deterministic framework with per-test process isolation for GPU work is a  
mature choice.

**Discipline.** Only **2 TODO comments across 36,665 lines** — both in `JobSystem.cpp`  
(thread affinity, worker tracking). No `FIXME`, no `HACK`. Clean per-module CMake. Comments  
explain *why*, not *what*.

---

## 6. Roadmap Coverage

| Milestone | Scope                                                 | Status                                                              |
| --------- | ----------------------------------------------------- | ------------------------------------------------------------------- |
| **v0.1**  | Core, RHI, Renderer, ECS, Scene, Assets, basic Editor | **Essentially complete**                                            |
| **v0.2**  | Physics, Audio, Animation, Scripting, Prefabs, Save   | Prefabs + Save ✅ · Physics/Audio/Animation/Scripting = empty dirs ❌ |
| **v0.3**  | Terrain, World Streaming, Particles, NavMesh, AI      | **None started**                                                    |
| **v0.4**  | Multiplayer, Dedicated Server, Cooking, Packaging     | `NFAssetCooker` exists · no networking ❌                            |
| **v0.5**  | Advanced Renderer, GI, Virtualized systems            | Not started                                                         |
| **v1.0**  | Stable APIs, samples, docs, profiling                 | Not started                                                         |

v0.1 delivered at ~37k LOC is a credible, honest milestone. The gap to v1.0 remains very large.

**Deferred design principles:** *"World streaming from day one"* — no streaming code exists,  
and retrofitting streaming into a scene/asset system is materially harder than building it in.  
Worth deciding consciously whether that principle is being consciously deferred or quietly  
dropped.

---


## 7. Scorecard

| Dimension                        |     Score    | Basis                                                                         |
| -------------------------------- | :----------: | ----------------------------------------------------------------------------- |
| Architecture & module separation |  **9** / 10  | 10 clean modules, no circular deps, empty scaffolding honest                  |
| RHI abstraction quality          |  **9** / 10  | Zero Vulkan in public header; headless device; correct pass/pipeline coupling |
| Test infrastructure              |  **8** / 10  | 202 tests, 1,245 assertions, pixel-level + leak verification                  |
| **Test suite health**            |  **4** / 10  | 3 SIGSEGVs, 1 false failure, 12 tests never run                               |
| Renderer robustness              |  **5** / 10  | Address-keyed cache = whole class of lifetime bugs                            |
| Asset pipeline                   |  **7** / 10  | VFS, registry, stable IDs, cooking, hot reload                                |
| Editor                           |  **7** / 10  | 7,500 LOC: undo/redo, prefabs, play mode, hot reload, panels                  |
| Performance validation           |  **3** / 10  | No visible metrics, no thresholds, green-by-skip                              |
| Process & tooling                |  **3** / 10  | No git, no CI                                                                 |
| Scope vs. ambition               |  **5** / 10  | v0.1 real; v0.2–v1.0 largely empty                                            |
| **Overall**                      | **6.0** / 10 | Excellent foundation, broken verification loop                                |

---

## 8. Priority Order

1. **Put the project under version control.** Nothing else on this list is as risky.
2. **Fix the stale framebuffer cache** (§4.1). Decide the identity model — resource id /  
   generation is the choice consistent with the design doc. Add the `create_framebuffer`  
   null-check regardless.
3. **Set `present_source = true`** in the Triangle sample (§4.2) and re-run with validation to  
   confirm the milestone claim honestly.
4. **Fix the dangling-pointer assertion** (§4.3) so the suite stops reporting a false failure.
5. **Make benchmarks real** (§4.5): raise the log level for benchmark output, assert  
   thresholds, and fail loudly instead of skipping silently.
6. **Add CI** (§4.6): build + run the suite on every push. This is what keeps 1–5 from  
   recurring.

---

## 9. Reproduction Commands

```bash
# Full suite (note: EditorTests crashes)
cd build/DebugNinja
./bin/CoreTests.exe; ./bin/JobTests.exe; ./bin/ECSTests.exe
./bin/AssetTests.exe; ./bin/RHITests.exe; ./bin/RuntimeTests.exe
./bin/EditorTests.exe            # exit code 139 — SIGSEGV

# Isolate a crashing test (per-test process isolation is supported)
./bin/EditorTests.exe material_albedo_gpu_effect      # exit 139
./bin/EditorTests.exe hotreload_mesh_rebuilds_live    # exit 139

# Confirm the false failure
./bin/EditorTests.exe editor_viewport_resize_lifecycle   # test_viewport.cpp:140

# Confirm the triangle sample is not validation-clean
NF_TRIANGLE_FRAMES=60 NF_TRIANGLE_VALIDATION=1 ./bin/NFSampleTriangle.exe | grep -c "ERROR \[RHI\]"
# -> 10

# Confirm the 1M benchmark is a no-op
./bin/ECSTests.exe benchmark_1m      # [OK] with no work done
```

