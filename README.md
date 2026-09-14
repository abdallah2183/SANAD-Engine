# NOVAForge Engine

High-performance C++20 game engine designed with a modern data-oriented architecture and low-overhead Vulkan RHI.

## Phase 2.5 — Current Status (3D Renderer Stabilization)

NOVAForge is currently at **Phase 2.5** stabilization gate. The core runtime pipeline (`Windows + Vulkan + Jobs + ECS + Deferred 3D Renderer`) is verified, test-hardened, and passes 100% of automated unit and rendering tests with zero validation errors.

### Implemented & Stabilized Modules
- **Core**: Custom math library (`Vec2`, `Vec3`, `Vec4`, `Mat4`, `Quat`), custom memory allocators (Linear, Pool, Stack), fast containers, high-resolution time, GUID/UUID, and thread-safe logging.
- **Jobs**: Work-stealing multi-threaded job scheduler and dependency graph.
- **ECS & Scene**: Cache-friendly Archetype Entity-Component System, hierarchical scene graphs (`Transform`, parent-child hierarchy), and prefab instantiation.
- **RHI (Vulkan)**: Low-overhead Vulkan 1.2+ backend supporting headless offscreen rendering, swapchain presentation, dynamic descriptor allocation, pipeline caching, and full validation layer integration.
- **Rendering & 3D**:
  - DAG-based **RenderGraph** with automatic dependency topological ordering and layout transitions.
  - Multi-pass deferred rendering pipeline: **Depth Prepass**, **GBuffer Generation** (Albedo, Normal, Roughness/Metallic, Emissive), **PBR Deferred Lighting** (Cook-Torrance GGX with Directional, Point, and Spot lights), and **Tonemapping** (Reinhard with gamma correction).
  - Frustum culling, render object extraction (`RenderWorld`), material library, and mesh streaming.
- **Samples**:
  - `NFSampleTriangle`: Bare-bones Vulkan RHI clear and pipeline verification.
  - `NFSampleTexturedQuad`: Texture upload, descriptor set binding, and sampler verification.
  - `NFSampleBasic3D`: Complete 3D deferred PBR pipeline rendering via `Renderer3D` with camera orbiting and lighting.

### Planned Modules (Future Phases)
- Physics (Jolt/PhysX integration)
- Audio (MiniAudio integration)
- Networking & Replication
- Asset VFS & Cooker GUI
- Engine Editor & Scripting (C#/Lua)

---

## Build Instructions

### Prerequisites
- **OS**: Windows 10/11 (x64)
- **Compiler**: MSVC 19.40+ (Visual Studio 2022 / 2026 or VS Build Tools)
- **Build System**: CMake 3.25+ and Ninja
- **Vulkan SDK**: Vulkan SDK 1.3+ with `glslc` on `PATH`

### Building the Engine and Samples

```powershell
# Configure using CMake presets
cmake --preset default

# Build all libraries, tests, and samples (Warnings as Errors enabled)
cmake --build --preset debug
```

### Running Automated Tests

All 126 automated unit and integration tests across Core, Jobs, ECS, and RHI/3D pass cleanly:

```powershell
# Run full test suite via CTest
ctest --test-dir build/default --output-on-failure

# Or run individual test suites directly
./build/default/bin/CoreTests.exe
./build/default/bin/JobTests.exe
./build/default/bin/ECSTests.exe
./build/default/bin/RHITests.exe
```

### Running the 3D Sample

```powershell
# Run Basic3D sample with Vulkan validation layers
./build/default/bin/NFSampleBasic3D.exe --validation

# Headless / benchmark mode (e.g. 60 frames)
./build/default/bin/NFSampleBasic3D.exe --frames 60
```

---

## Codebase Structure

```
NOVAForge/
├── Engine/
│   ├── Core/        — Memory, Math, Containers, Logging, UUID
│   ├── Platform/    — Windows Win32 Window, Input, Events
│   ├── RHI/         — Graphics abstraction layer & Vulkan backend
│   ├── Rendering/   — RenderGraph, Renderer3D, PBR Shaders, Materials, Meshes
│   ├── Jobs/        — Fiber/Worker job system with work stealing
│   ├── ECS/         — Archetype ECS storage and query engine
│   ├── Scene/       — Scene hierarchy, Transform, Entity management
│   └── Shaders/     — GLSL shaders compiled to SPIR-V (Depth, GBuffer, Lighting, Tonemap)
├── Samples/
│   ├── Triangle/    — Hello Triangle RHI sample
│   ├── TexturedQuad/— Texture sampling RHI sample
│   └── Basic3D/     — Full deferred 3D PBR sample
└── Tests/
    ├── CoreTests/   — Math, Memory, Container, and IO tests (36 tests)
    ├── JobTests/    — Job scheduling & parallel tests (4 tests)
    ├── ECSTests/    — ECS archetype, transform & prefab tests (22 tests)
    └── RHITests/    — Vulkan RHI, RenderGraph, and 3D pipeline tests (64 tests)
```

## License

Copyright (c) 2026 NOVAForge Engine Contributors. All rights reserved.
Licensed under the Apache License, Version 2.0.

