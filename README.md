# NOVAForge Engine

High-performance C++23 game engine designed with a modern data-oriented architecture and low-overhead Vulkan RHI.

## Current Status (Phase 11 — foundation: layering, debt, world streaming)

NOVAForge is at **Phase 11**. The core runtime pipeline (`Windows + Vulkan + Jobs + ECS + Deferred 3D Renderer`) is verified, test-hardened, and passes 100% of automated unit and rendering tests with zero validation errors. A project can be created, built, packaged and run as a standalone program. A self-contained deterministic rigid-body solver (`PhysicsWorld`), a skeletal animation system (`Skeleton`, `AnimationClip`, `AnimationPlayer`, `AnimationStateMachine`), an audio engine (`AudioDevice`, `AudioBus`, `AudioSource`), a reflection layer (`PropertyInfo`, `ClassInfo`, `ReflectionRegistry`), a C++ gameplay module system (`GameplayModule`, `GameplayModuleRegistry`), and a versioned save system (`SaveSystem`) are integrated into the ECS, runtime, editor, and serialized scene format.

Integration is end-to-end, not just API-level: `Runtime::update()` steps physics → animation → audio → gameplay → transform propagation, so an animated entity's transform is actually written every frame, audio is mixed where the frame will render it, and a gameplay module's writes reach the renderer in the frame it made them. Because the asset import pipelines (mesh/WAV) are still future work, the animation and audio subsystems ship deterministic generators — `make_procedural_clip()` and `make_tone_buffer()` — so a scene can name a clip and a buffer that produce real motion and real samples without a cooked asset. Every component has an editor inspector panel, and the gameplay module state is edited through reflection rather than a hand-written panel.

**Verified 2026-09-14**: build clean under `/W4 /WX`; **534 passed / 0 failed / 1 skipped across 14 suites**; the packaged `NFPlayer` runs the template scene — a falling box on a static plane plus an entity driven by a procedural clip and a generated tone — with 0 validation errors and 0 leaked RHI objects.

### Phase 11 — foundation, not features

Phase 11 deliberately added **no feature area**. It removed three things that were actively wrong and
landed one seam that the design document calls a "from day one" principle. The case was not aesthetic:
each item was a trap that had already cost time or structurally blocked a stated goal.

- **`Engine/Assets` no longer depends on `NFRendering`.** The asset library was pulling in the renderer
  — and therefore the RHI and all of Vulkan — because `AssetManager` was doing two jobs: load, parse
  and cache CPU assets, *and* upload meshes to the GPU. The upload now lives in `Engine/Rendering`
  (`MeshUpload`), the runtime owns the device, and `Assets` links only `NFCore`/`NFJobs`. Anything that
  only needs assets no longer links a graphics stack.
- **`Engine/Rendering` no longer depends on `NFEcs`/`NFScene`.** The ECS → render-world bridge moved up
  to `NF/Runtime/SceneExtraction.hpp`, the layer that legitimately knows about both sides. The renderer
  consumes plain data and can now be built and used standalone — which is what the **dedicated-server
  path** needs, since a server has no business compiling a renderer to walk a scene graph.
- **One `.nfmesh` format, not two.** The second writer (`rendering::mesh_asset`, magic `NFM1`) is
  deleted. `NFME` / `assets::MeshAsset` is the only format, so the cooker can no longer produce
  something the runtime cannot read.
- **The six dead RHI handle types are gone** — `BufferHandle`, `TextureHandle`, `PipelineHandle`,
  `ShaderModuleHandle`, `RenderPassHandle`, `FramebufferHandle`. They had zero references; the device
  returns owning `unique_ptr`s. Two resource models, one abandoned, is how SIGSEGV-class bugs start.
- **The ECS benchmarks assert a ceiling** instead of printing numbers nobody reads. The bound is
  deliberately loose (an order of magnitude) so it catches a real regression without flaking on a
  throttled runner — a flaky test gets muted, after which it detects nothing.
- **World-streaming seam** (`scene::StreamingVolume` + `runtime::WorldStreamer`): distance-based chunk
  load/unload on a uniform grid, with a hysteresis band so a volume parked on a chunk boundary does not
  thrash, and a per-update load cap with nearest-first ordering. The seam plus one working
  implementation — LOD, priority queues and a memory budget are deliberately deferred and documented.

Both inversions are enforced by `Scripts/check_layering.sh`, which runs in CI immediately after the
build so a reintroduced `#include` fails in seconds with the offending line named, instead of
surfacing later as a confusing link error.

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
- **Reflection**: `PropertyInfo` / `ClassInfo` / `EnumInfo` metadata with `NF_CLASS` / `NF_PROPERTY` / `NF_ENUM` macros — no codegen step, so a property declaration sits next to the member it describes and nothing else has to be kept in sync. `ReflectionRegistry` registers classes during static initialisation, so the editor can enumerate types it has never instantiated. `property_to_string` / `property_from_string` are the single text form shared by the inspector, the scene format, and the save format — nine significant digits, so an `f32` survives a round trip exactly.
- **Gameplay (scripting S0–S1)**: `GameplayModule` with `on_init` / `on_update` / `on_shutdown` / `on_scene_load` / `on_scene_unload`, registered by `NF_GAMEPLAY_MODULE` and driven by `Runtime::step_gameplay()`. Ordering is by `update_priority()` with a name tie-break, so it does not depend on static-init order. A module reaches the engine only through `GameplayContext` (world, scene, physics, audio, input), and its settings are a plain reflected struct, which is what makes them editable in the inspector and serializable without hand-written code. `OrbitCameraModule` is a worked example.
- **Save system**: `SaveSystem` with named slots under `saves://`, each a directory of `scene.nfscene` + `modules.txt` + `meta.txt`. `save_game` builds into a staging directory and swaps it in with rollback, so a failed save cannot destroy a good one. `save_game_async` snapshots on the calling thread and writes on a worker — the scene is never serialized off-thread. Autosave is interval-driven, and `meta.txt` carries a schema version with a chained migration registry.
- **Assets**: VFS (`content://`, `cache://`, `engine://`, `project://`, `saves://`), UUID-keyed asset registry, and a CLI cooker. **CPU-pure since Phase 11**: the module reads bytes and parses them, and links only `NFCore`/`NFJobs` — no RHI, no renderer, no graphics device. `assets::MeshAsset` (`.nfmesh`, magic `NFME`) is the single mesh format; the conversion to the GPU mesh lives in `Engine/Rendering` (`MeshUpload`).
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

**534 passed / 0 failed / 1 skipped** across fourteen suites (the skip is the opt-in 1M-entity
benchmark). A skipped test is never counted as a pass. The runtime integration is covered by tests
that assert a transform *actually moved*, audio *actually mixed*, and a gameplay module *actually
stepped* after `Runtime::update()` —
not merely that component fields survive a serialization round-trip.

The module-layering invariants Phase 11 established are checked separately, and cheaply — CI runs this
immediately after the build, so a reintroduced inversion fails in seconds with the offending line
named instead of surfacing later as a confusing link error:

```bash
bash Scripts/check_layering.sh
```

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
│   ├── Scene/       — Scene hierarchy, Transform, entity management, streaming chunk grid
│   ├── Assets/      — VFS, asset registry, cooked mesh/material loaders (CPU-pure: no RHI, no renderer)
│   ├── Physics/     — Deterministic rigid-body solver (shapes, broadphase, narrowphase, solver, world)
│   ├── Animation/   — Skeletal animation (skeleton, clips, player, state machine, blending)
│   ├── Audio/       — Audio engine (attenuation, 3D pan/gain, mixer, null device for headless)
│   ├── Gameplay/    — C++ gameplay modules: lifecycle, registry, reflected state bridge
│   ├── Runtime/     — Runtime + Application: device ownership, frame loop, asset sync, subsystem
│   │                  stepping, the ECS→render-world bridge, and the world streamer
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
    ├── CoreTests/      — Math, memory, containers, IO, reflection           (59)
    ├── JobTests/       — Job scheduling & parallelism                        (5)
    ├── ECSTests/       — ECS, transform hierarchy, prefabs, benchmarks    (25+1skip)
    ├── AssetTests/     — VFS, registry, cooking, projects                   (45)
    ├── RHITests/       — Vulkan RHI, RenderGraph, 3D pipeline               (69)
    ├── PhysicsTests/   — Shapes, broadphase, narrowphase, solver, determinism (86)
    ├── AnimationTests/ — Skeleton, clips, player, state machine             (38)
    ├── AudioTests/     — Attenuation, pan/gain, mixer, null device          (27)
    ├── GameplayTests/  — Module lifecycle, registry, state serialization    (24)
    ├── SaveTests/      — Slots, rollback, async save, migrations            (16)
    ├── StreamingTests/ — Chunk grid geometry + streaming policy             (19)
    ├── RuntimeTests/   — Scene load, run config, subsystem stepping         (33)
    ├── EditorTests/    — Outliner, inspector, undo, prefabs, panels         (74)
    └── ToolTests/      — Project scaffold, cooker, packager                 (14)
```

## License

Copyright (c) 2026 NOVAForge Engine Contributors. All rights reserved.
Licensed under the Apache License, Version 2.0.

