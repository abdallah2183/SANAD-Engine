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
- ⚠️ **Triangle Rendering (v0.1 milestone 1): was GREEN, now NOT green.** Re-audited 2026-09-13.
  - Historical (2026-09-01): Core 36/36 | Jobs 4/4 | RHI 3/3 | pixel-verified triangle.
  - **2026-09-13 audit:** Core 36/36 ✅ | Jobs 4/4 ✅ | ECS 22/22 ✅ | Assets 13/13 ✅ | RHI 64/64 ✅ | Runtime 13/13 ✅ | **EditorTests 37/50 then SIGSEGV** (12 tests never run).
  - The Triangle sample is **not** validation-clean: `Samples/Triangle/main.cpp:228-232` omits `rp_desc.present_source = true` → one `vkQueuePresentKHR` layout error per frame.
  - Full evaluation + reproduction commands: `Docs/Engine_Evaluation_2026-09-13.md`.
- 🔴 **Open critical bug:** `Renderer3D.hpp:217-229` keys `m_tonemap_fbs` on the raw `Texture*` **address**, cleared only on resolution change. Same-size target replacement → recycled address → cache hit → stale `VkFramebuffer` → SIGSEGV. Crashes `material_albedo_gpu_effect`, `material_params_gpu_effect`, `hotreload_mesh_rebuilds_live`. Also `Renderer3D.cpp:752` lacks a null-check on `create_framebuffer` before the deref at line 756.
- 🔴 **No version control and no CI.** The project is not a git repository.
- ⏭ Next: Textured Quad — vertex/index buffers, texture upload (staging), descriptor sets + sampled image, sampler. *(Already largely present in RHI tests; verify against sample.)*

## Toolchain Locations (cmake/ctest/ninja are NOT on PATH)
- cmake: `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`
- ninja: `.../Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe`
- `bash Scripts/build.sh` configures+builds `build/debug`. `build_nf.bat` builds `build/DebugNinja` (VS 18 Community vcvars64).
- `build/DebugNinja` is the freshest/most complete build (Editor, EditorTests, PlatformTests, NFAssetCooker). `build/Debug` is stale and lacks EditorTests.
- Test exes are custom-framework: run directly (`./bin/EditorTests.exe [filter]`); `ctest` is not on PATH.
- **Never touch `build/` cleanup casually** — several stale build dirs exist (`Debug`, `DebugNinja`, `default`, `msvc`).
