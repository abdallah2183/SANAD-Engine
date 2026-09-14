# NOVAForge Engine — Project Memory

## Project Overview
- **Name:** NOVAForge Engine
- **Type:** High-performance, cross-platform, unified game engine (C++)
- **Developer:** Abdal (solo builder)
- **Design Doc:** `NOVAForge_Engine_Complete_Design.md` — 322 sections, complete architecture specification

## Key Architecture Decisions (from design doc)
- C++ as engine language
- Separate RHI layer (Vulkan, D3D12, Metal)
- ECS as core paradigm
- Render World decoupled from Game World (extraction step)
- Handle-based resources (no raw pointers)
- Async IO, shared Job System
- Stable Asset IDs (UUID/content IDs, not paths)
- Versioned serialization with migration
- Editor separate from Runtime
- Modular subsystems, no circular dependencies
- Data-driven assets
- World streaming from day one
- Dedicated server buildable without GPU
- Profiling and CI from the start
- Quality scalability tiers (Low/Medium/High/Ultra)

## Roadmap Summary
- v0.1: Core, RHI, Renderer, ECS, Scene, Assets, basic Editor
- v0.2: Physics, Audio, Animation, Scripting, Prefabs, Save
- v0.3: Terrain, World Streaming, Particles, NavMesh, AI
- v0.4: Multiplayer, Dedicated Server, Cooking, Packaging
- v0.5: Advanced Renderer, GI, Virtualized systems, high-end scalability
- v1.0: Stable APIs, stable editor, samples, docs, profiling

## Naming Convention
- `NF` prefix (e.g., `NFEntity`, `NFWorld`, `NFTexture`)
- Namespace: `nf::` (lowercase, as implemented — the design doc's `NF::` is aspirational)
- CLI tool: `nf`

## Build & Test
- Build: `bash Scripts/build.sh` (Debug, Ninja + MSVC, sets up env directly). Release: `bash Scripts/build.sh release`.
- Tests: `ctest --output-on-failure` in `build/debug`.
- Triangle sample: `build/debug/bin/NFSampleTriangle.exe [--frames N] [--validation]` (env: `NF_TRIANGLE_FRAMES`, `NF_TRIANGLE_VALIDATION`).
- Vulkan SDK: `C:/VulkanSDK/1.4.357.0`. glslc present → shaders compiled at build time via `CMake/NFShaders.cmake`.

## RHI Architecture (as built, not as designed)
- Abstract layer `Engine/RHI/include/NF/RHI/RHI.hpp`: zero Vulkan includes. Handles, device-factory, swapchain, buffers, textures, shader modules, pipeline, render pass, framebuffer, command buffer, semaphores/fences.
- Vulkan backend under `Engine/RHI/src/Vulkan/` (Device, Swapchain, Pipeline, CommandBuffer, Buffer, Texture, ShaderModule). Dynamic dispatch via `VulkanLoader` (no `vkGetInstanceProcAddr` static linking).
- `PipelineDesc` takes `const RenderPass*` (not a desc copy) — pipeline built against the exact VkRenderPass it draws with.
- `RenderPassDesc::present_source` decides final attachment layout (PRESENT_SRC vs COLOR_ATTACHMENT_OPTIMAL).
- `init(nullptr)` = headless device (no surface/swapchain) — enables GPU tests without a window.
- Per-swapchain-image render-finished semaphores (not a single shared one) — required for correct presentation.

## Milestone Status
- ✅ **Triangle Rendering (v0.1 milestone 1): GREEN, re-verified 2026-09-13 after fixes.**
  - Full suite: Core 36/36 | Jobs 4/4 | ECS 21/22 (1 opt-in skip) | Assets 13/13 | RHI 64/64 | Runtime 13/13 | Editor 53/53 → **204 passed, 0 failed, 1 skipped** (working tree, 19:30).
  - Triangle + TexturedQuad samples: 60 frames, **0 Vulkan validation errors**.
  - Verified from a clean checkout of commit `1fc62d0`/`0f9de23` in a separate `git worktree`, so the result reflects the committed state, not just the working tree.
  - Evaluation report (pre-fix state + reproduction commands): `Docs/Engine_Evaluation_2026-09-13.md`.
- ✅ **Stale-framebuffer fix: acceptance-verified 2026-09-13 19:30** against `Docs/Engine_Evaluation_2026-09-13.md` §4.1. `material_albedo_gpu_effect`, `material_params_gpu_effect`, `hotreload_mesh_rebuilds_live` each exit 0 with **0 validation errors**. Change is confined to `Renderer3D.hpp`/`.cpp`; the identity primitive (`rhi::Texture::creation_serial()`) was already present in the RHI, so no RHI edit was needed. `Renderer3D::resize()` and `destroy_resolution_dependent()` are byte-identical to the pre-fix versions.
- ✅ **Fixed 2026-09-13:** framebuffer cache now keyed on `rhi::Texture::creation_serial()` (monotonic, never reused) instead of the raw address, bounded to `kMaxTonemapFramebuffers` (8); `create_framebuffer` null-checked before deref; `present_source = true` added to the Triangle and TexturedQuad render passes; `test_viewport.cpp` compares `creation_serial()` instead of a recycled pointer.
- ✅ **Fixed 2026-09-13 (test honesty):** added `NF_SKIP` + a `Skipped` bucket to the test framework; `require_gpu()` replaces the 58 silent `if (!f.available) return;` early-returns; 11 silent returns on missing shader assets became skips; `benchmark_1m_entities` now SKIPs instead of reporting OK without running; `NF_TEST_LOG_LEVEL` env var exposes benchmark timings.
- ✅ **Version control + CI added 2026-09-13:** git repo initialised (3 commits, `main`), `.gitignore`/`.gitattributes`, `Scripts/run_tests.sh`, `.github/workflows/ci.yml` (installs Lavapipe so GPU tests execute headlessly).
  - Repo-local git identity is a **placeholder**: `Abdal <abdal@localhost>` — change with `git config user.email "..."`.
- ⚠️ **Working tree may not build:** Abdal is mid-refactor on mipmap/sampler support (`Runtime.hpp`, `Runtime.cpp`, `MaterialAsset.hpp/.cpp`). These are deliberately uncommitted; HEAD is green, the working tree is not, until that refactor lands.
- ⏭ Next: Textured Quad — vertex/index buffers, texture upload (staging), descriptor sets + sampled image, sampler. *(Largely present in RHI tests; verify against the sample.)*

## Toolchain Locations (cmake/ctest/ninja are NOT on PATH)
- **Two toolchains exist and they differ** — pick deliberately:
  - `build/DebugNinja` → VS 18 Community, MSVC `14.51.36231`, built via `build_nf.bat`. Freshest/most complete (Editor, EditorTests, NFAssetCooker).
  - `Scripts/build.sh` → BuildTools 2022, MSVC `14.44.35207`, via `CMake/Toolchain-MSVC.cmake`, builds `build/debug`.
- cmake: `C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`
- ninja: `.../Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe`
- **cmd.exe is blocked** in both the Bash and PowerShell tools — cannot call `build_nf.bat` or `vcvars64.bat`. Build by exporting the MSVC env directly:
  ```
  export INCLUDE="<msvc>/include;<sdk>/Include/<ver>/ucrt;<sdk>/Include/<ver>/um;<sdk>/Include/<ver>/shared"
  export LIB="<msvc>/lib/x64;<sdk>/Lib/<ver>/ucrt/x64;<sdk>/Lib/<ver>/um/x64"
  export PATH="<msvc>/bin/Hostx64/x64:<sdk>/bin/<ver>/x64:$PATH"
  export VULKAN_SDK="C:/VulkanSDK/1.4.357.0"
  ```
- Pass `-DCMAKE_MAKE_PROGRAM` a **Windows-style** path (`C:/Program Files/...`); MSYS `/c/...` paths fail with "no such file or directory".
- `CMake/Toolchain-MSVC.cmake` sets INCLUDE/LIB only for the configure process — you must still export them for the build step.
- Tests: `bash Scripts/run_tests.sh build/DebugNinja`. Individual exes: `./bin/EditorTests.exe [name-filter]` (per-test process isolation for GPU work). `ctest` is not on PATH.
- **Never touch `build/` cleanup casually** — several stale build dirs exist (`Debug`, `DebugNinja`, `default`, `msvc`). OneDrive intermittently locks freshly-linked `.exe` files (LNK1104) — just retry the build.
- `git worktree add` is the safe way to verify a commit builds without disturbing the working tree. Note MSYS path conversion: pass a Windows path or the worktree lands in a surprising place.

## Verification Convention
A state counts as green only when it has been built **and** run. The suite reports `Passed | Failed | Skipped`; skipped is never a pass. Prefer verifying a commit from a clean worktree over trusting the working tree.
