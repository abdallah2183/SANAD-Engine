# NOVAForge Engine

High-performance C++23 game engine designed with a modern data-oriented architecture and low-overhead Vulkan RHI.

## Current Status (Phase 9 — animation + audio)

NOVAForge is at **Phase 9**. The core runtime pipeline (`Windows + Vulkan + Jobs + ECS + Deferred 3D Renderer`) is verified, test-hardened, and passes 100% of automated unit and rendering tests with zero validation errors. A project can be created, built, packaged and run as a standalone program. A self-contained deterministic rigid-body solver (`PhysicsWorld`) and a skeletal animation system (`Skeleton`, `AnimationClip`, `AnimationPlayer`, `AnimationStateMachine`) and an audio engine (`AudioDevice`, `AudioBus`, `AudioSource`) are integrated into the ECS, runtime, editor, and serialized scene format.

Integration is end-to-end, not just API-level: `Runtime::update()` steps physics → animation → audio → transform propagation, so an animated entity's transform is actually written every frame and audio is mixed where the frame will render it. Because the asset import pipelines (mesh/WAV) are still future work, both subsystems ship a deterministic generator — `make_procedural_clip()` and `make_tone_buffer()` — so a scene can name a clip and a buffer that produce real motion and real samples without a cooked asset. Both components have editor inspector panels, and the default template scene contains an animated, audible entity.

**Verified 2026-09-14**: build clean under `/W4 /WX`; **439 passed / 0 failed / 1 skipped / 440**; the packaged `NFPlayer` runs the template scene — a falling box on a static plane plus an entity driven by a procedural clip and a generated tone — with 0 validation errors and 0 leaked RHI objects.

### Implemented & Stabilized Modules
- **Core**: Custom math library (`Vec2`, `Vec3`, `Vec4`, `Mat4`, `Quat`), custom memory allocators (Linear, Pool, Stack), fast containers, high-resolution time, GUID/UUID, and thread-safe logging.
- **Jobs**: Work-stealing multi-threaded job scheduler and dependency graph.
- **ECS & Scene**: Cache-friendly sparse-set Entity-Component System, hierarchical scene graphs (`Transform`, parent-child hierarchy), and prefab instantiation.
- **RHI (Vulkan)**: Low-overhead Vulkan 1.2+ backend supporting headless offscreen rendering, swapchain presentation, dynamic descriptor allocation, pipeline caching, and full validation layer integration.
- **Rendering & 3D**:
  - DAG-based **RenderGraph** with automatic dependency topological ordering and layout transitions.
  - Multi-pass deferred rendering pipeline: **Depth Prepass**, **GBuffer Generation** (Albedo, Normal, Roughness/Metallic, Emissive), **PBR Deferred Lighting** (Cook-Torrance GGX with Directional, Point, and Spot lights), and **Tonemapping** (Reinhard with gamma correction).
  - Frustum culling, render object extraction (`RenderWorld`), material library, mesh streaming, GPU picking, and per-material-instance descriptor caching.
- **Physics**: Self-contained deterministic rigid-body solver behind a `PhysicsWorld` seam. Sequential impulses with warm starting, Coulomb friction, restitution, Baumgarte position correction (split impulse), and island-based sleeping. Shapes: sphere, oriented box, infinite plane. Broadphase: uniform spatial hash grid. Narrowphase: SAT + Sutherland-Hodgman clipping for box–box. Fixed-timestep accumulator with deterministic state hashing. Generation-checked `BodyHandle`. `RigidBodyComponent`/`ColliderComponent` in the ECS; runtime steps physics on the fixed clock and writes transforms back; scene serialization round-trip.
- **Animation**: Skeletal animation system with `Skeleton` (hierarchy of bones, parent indices, rest poses), `AnimationClip` (tracks with keyframes, binary-search sampling, lerp translation/scale + slerp rotation), `AnimationPlayer` (play/pause/stop, speed, loop/ping-pong), `AnimationStateMachine` (states, transitions with parameter conditions, cross-fade), N-way blend and additive blend. `AnimationComponent` in the ECS; `Runtime::step_animation()` samples it each frame and writes the posed bone delta onto the entity's `Transform` relative to an authored base offset, so animating an entity never teleports it to the rig origin. `make_procedural_clip()` generates a spin/bob clip from a `ProceduralClipSpec`, which is what lets an `Animation:` scene line drive motion before the mesh import pipeline exists. Scene serialization round-trip; editor inspector panel.
- **Audio**: Audio engine with `AudioDevice` (abstract, `NullAudioDevice` for headless/CI), `AudioSource` (buffer, volume, pitch, looping), `AudioBus` (mixer), `AudioListener` (position + orientation). 3D positional audio with linear/inverse/exponential distance attenuation and equal-power stereo panning. All math is pure and testable without hardware. `Runtime::step_audio()` mixes sources into the bus each frame and reports the output peak; `make_tone_buffer()` generates a deterministic PCM sine so an `Audio:` line is audible before the WAV/OGG import pipeline exists. `AudioComponent` in the ECS; scene serialization round-trip; editor inspector panel.
- **Assets**: VFS (`content://`, `cache://`, `engine://`, `project://`), UUID-keyed asset registry, and a CLI cooker.
- **Samples**:
  - `NFSampleTriangle`: Bare-bones Vulkan RHI clear and pipeline verification.
  - `NFSampleTexturedQuad`: Texture upload, descriptor set binding, and sampler verification.
  - `NFSampleBasic3D`: Complete 3D deferred PBR pipeline rendering via `Renderer3D` with camera orbiting and lighting.
  - `NFSampleRuntimeScene`: Loads a `.nfscene` through the VFS/registry/AssetManager, with a `--headless` mode.

### Planned Modules (Future Phases)
- Audio backend swap (MiniAudio behind the `AudioDevice` seam)
- Networking & Replication
- Asset VFS & Cooker GUI
- Engine Editor & Scripting (C#/Lua)
- Physics backend swap (Jolt behind the `PhysicsWorld` seam)

> `Engine/{AI,Networking,Scripting,UI}` are empty placeholders for
> this planned work. `Input` is implemented, but lives under `Engine/Platform/`.

---

## Projects: create, build, run standalone

A **project** is a directory with a `.nfproj` descriptor. The descriptor declares where content,
cache and shaders live, which scene to start in, and the window defaults — so the runtime no longer
has to guess, and a built game runs without the engine source tree.

```bash
# 1. Create a project (copies the default template: scene, mesh, material)
./build/debug/bin/nf new MyGame --name MyGame

# 2. Cook every asset and package into MyGame/dist/
./build/debug/bin/nf build --project MyGame/MyGame.nfproj

# 3. Run it — no engine tree, no editor
cd MyGame/dist && ./NFPlayer --frames 60 --validation
```

`nf` also has `nf cook` (cook only), `nf verify` (every registry entry's cooked file exists and
parses) and `nf run` (build, then launch).

The package is a **plain directory**, deliberately not an archive — it is debuggable and diffable:

```text
MyGame/dist/
  MyGame.nfproj        mounts are relative, so the package is relocatable
  NFPlayer.exe
  Content/             source assets, mirrored
  Cache/               cooked assets the runtime reads
  Shaders/Basic3D/     SPIR-V, so the hardcoded build-tree search is not load-bearing
  manifest.txt         sorted "<fingerprint>  <relative path>", diffable between builds
```

The editor opens inside a project with `--project`, shows its name in the toolbar, and its **Build**
button runs the same packaging step:

```bash
./build/debug/bin/NOVAForgeEditor.exe --project MyGame/MyGame.nfproj
```

### The `.nfproj` format

Line-based tolerant text, matching `.nfreg` / `.nfmat` / `.nfscene`:

```text
# NOVAForge Project
version: 1
name: MyGame
title: My Game
startup_scene: content://Scenes/Main.nfscene
window_width: 1280
window_height: 720

# Mounts. Relative paths resolve against this file's directory.
mount: project:// -> .
mount: content:// -> Content
mount: cache://   -> Cache
mount: shaders:// -> Shaders
```

Four mounts have defaults relative to the project root and an explicit line overrides them.
`engine://` has **no** default — it only means anything inside the engine source tree. A relative
mount may not escape the project directory; an unknown `version` is rejected rather than
half-parsed.

---

## Build Instructions

### Prerequisites
- **OS**: Windows 10/11 (x64)
- **Compiler**: MSVC 19.40+ (Visual Studio 2022 / 2026 or VS Build Tools)
- **Build System**: CMake 3.25+ and Ninja
- **Vulkan SDK**: Vulkan SDK 1.3+ with `glslc` on `PATH`

### Building the Engine and Samples

`Scripts/build.sh` locates the Visual Studio install, MSVC toolset and Windows SDK automatically
and builds the tree it lives in, so it works from any git worktree:

```bash
bash Scripts/build.sh              # Debug
bash Scripts/build.sh release      # Release
bash Scripts/build.sh rebuild      # Clean + configure + build
```

Or drive CMake directly:

```bash
cmake -S . -B build/debug -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=CMake/Toolchain-MSVC.cmake \
      -DCMAKE_BUILD_TYPE=Debug -DNF_BUILD_TESTS=ON -DNF_BUILD_SAMPLES=ON \
      -DNF_BUILD_EDITOR=ON -DNF_BUILD_TOOLS=ON
cmake --build build/debug --parallel
```

Warnings-as-errors is on by default (`-DNF_WARNINGS_AS_ERRORS=OFF` to disable).

### Cook the assets first

The asset registry maps meshes to `cache://` paths that **only exist after cooking**. Skip this and
samples and the editor render an empty scene — they will now fail loudly rather than exit 0 with a
blank frame.

```bash
./build/debug/bin/NFAssetCooker.exe --input content://Meshes/cube.nfmesh \
                                    --output cache://Meshes/cube.nfmesh \
                                    --registry content://AssetRegistry.nfreg
```

### Running Automated Tests

**439 passed / 0 failed / 1 skipped** across eleven suites (the skip is the opt-in 1M-entity
benchmark). A skipped test is never counted as a pass. The runtime integration is covered by tests
that assert a transform *actually moved* and audio *actually mixed* after `Runtime::update()` —
not merely that component fields survive a serialization round-trip.

```bash
bash Scripts/run_tests.sh build/debug

# Or a single suite, with an optional substring filter on the test name
./build/debug/bin/RHITests.exe
./build/debug/bin/RHITests.exe rhi_offscreen_triangle
```

### Running the Samples

```bash
# Validation layers on, fixed frame budget, headless where supported
NF_TRIANGLE_FRAMES=60   NF_TRIANGLE_VALIDATION=1   ./build/debug/bin/NFSampleTriangle.exe
NF_BASIC3D_FRAMES=60    NF_BASIC3D_VALIDATION=1    ./build/debug/bin/NFSampleBasic3D.exe
NF_RUNTIME_HEADLESS=1   NF_RUNTIME_FRAMES=30       ./build/debug/bin/NFSampleRuntimeScene.exe

# Editor acceptance harness (headless works on a machine with no display)
./build/debug/bin/NOVAForgeEditor.exe --headless --frames 30
```

---

## Codebase Structure

```
NOVAForge/
├── Engine/
│   ├── Core/        — Memory, Math, Containers, Logging, UUID
│   ├── Platform/    — Windows Win32 Window, Input, Events
│   ├── RHI/         — Graphics abstraction layer & Vulkan backend
│   ├── Rendering/   — RenderGraph, Renderer3D, PBR Shaders, Materials, Meshes, GPU picking
│   ├── Jobs/        — Fiber/Worker job system with work stealing
│   ├── ECS/         — Sparse-set ECS storage and query engine
│   ├── Scene/       — Scene hierarchy, Transform, Entity management
│   ├── Assets/      — VFS, asset registry, cooked mesh/material loaders
│   ├── Physics/     — Deterministic rigid-body solver (shapes, broadphase, narrowphase, solver, world)
│   ├── Animation/   — Skeletal animation (skeleton, clips, player, state machine, blending)
│   ├── Audio/       — Audio engine (attenuation, 3D pan/gain, mixer, null device for headless)
│   ├── Runtime/     — Runtime + Application: device ownership, frame loop, asset sync, physics stepping
│   └── Shaders/     — GLSL shaders compiled to SPIR-V (Depth, GBuffer, Lighting, Tonemap, Pick)
├── Editor/          — Dear ImGui + Win32 + Vulkan editor, with a headless acceptance harness
├── Samples/
│   ├── Triangle/     — Hello Triangle RHI sample
│   ├── TexturedQuad/ — Texture sampling RHI sample
│   ├── Basic3D/      — Full deferred 3D PBR sample
│   └── RuntimeScene/ — .nfscene loaded through VFS + registry + AssetManager
├── Templates/
│   └── Default/      — what `nf new` copies into a new project
├── Tools/
│   ├── ProjectTool/  — project descriptor, cooker, scaffold, packager (library)
│   ├── AssetCooker/  — cook one asset or a whole project
│   ├── BuildTool/    — the `nf` CLI (new / cook / build / run / verify)
│   └── Player/       — NFPlayer, the standalone game runtime
└── Tests/
    ├── CoreTests/    — Math, Memory, Container, IO            (44)
    ├── JobTests/     — Job scheduling & parallelism            (4)
    ├── ECSTests/     — ECS, transform hierarchy & prefabs     (26)
    ├── AssetTests/   — VFS, registry, cooking, projects        (45)
    ├── RHITests/     — Vulkan RHI, RenderGraph, 3D pipeline   (69)
    ├── PhysicsTests/ — Shapes, broadphase, narrowphase, solver, determinism (86)
    ├── RuntimeTests/ — Scene load, offscreen render, physics serialization (25)
    ├── EditorTests/  — Outliner, inspector, undo, prefabs     (54)
    └── ToolTests/    — Project scaffold, cooker, packager     (14)
```

## License

Copyright (c) 2026 NOVAForge Engine Contributors. All rights reserved.
Licensed under the Apache License, Version 2.0.

