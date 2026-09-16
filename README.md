<p align="center">
  <img src="Docs/images/novaforge_logo.jpg" alt="شعار محرك سند" width="200" style="border-radius: 20px; box-shadow: 0 10px 30px rgba(0, 0, 0, 0.4);" />
</p>

<h1 align="center">محرك سَنَد | SANAD Engine</h1>

<p align="center">
  <strong>محرك ألعاب ثلاثي الأبعاد حديث ومفتوح المصدر مبني بلغة C++23 ومكتبة الرسوميات Vulkan</strong><br>
  <em>Modern, High-Performance C++23 & Vulkan 3D Game Engine</em>
</p>

<p align="center">
  <strong>المؤسس والقائم على العمل:</strong> <a href="https://github.com/abdallah2183"><strong>عبدالله (Abdallah)</strong></a>
</p>

<p align="center">
  <a href="#"><img src="https://img.shields.io/badge/Language-%D8%A7%D9%84%D8%B9%D8%B1%D8%A8%D9%8A%D8%A9%20%7C%20English-blue.svg" alt="اللغة" /></a>
  <a href="#"><img src="https://img.shields.io/badge/Standard-C%2B%2B23-00599C.svg?logo=c%2B%2B" alt="C++23" /></a>
  <a href="#"><img src="https://img.shields.io/badge/Graphics-Vulkan%201.2%2B-red.svg?logo=vulkan" alt="Vulkan 1.2+" /></a>
  <a href="#"><img src="https://img.shields.io/badge/Tests-733%20Passed-brightgreen.svg" alt="Tests" /></a>
  <a href="#"><img src="https://img.shields.io/badge/Validation-0%20Errors-success.svg" alt="Validation" /></a>
  <a href="CONTRIBUTING.md"><img src="https://img.shields.io/badge/Contributions-Welcome-orange.svg" alt="Contributions" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-Apache%202.0-lightgrey.svg" alt="License" /></a>
</p>

---

## الواجهة التفاعلية والموقع التعريفي

يتوفر للمحرك موقع تفاعلي متكامل يعرض إمكانيات المحرك ومجسماً ثلاثي الأبعاد تفاعلياً:
- **الموقع المباشر:** [https://abdallah2183.github.io/SANAD-Engine/](https://abdallah2183.github.io/SANAD-Engine/)
- **الملف المحلي:** [`index.html`](index.html)

---

## لقطات حقيقية (Real Screenshots)

صور ملتقطة مباشرة من المحرر واللعبة العاملة — بدون فوتوشوب:

| محرر سند (إنجليزي) | محرر سند (عربي RTL) | لعبة جامع المكعبات |
| :---: | :---: | :---: |
| ![SANAD Editor](Docs/images/shot_editor_en.png) | ![Arabic UI](Docs/images/shot_editor_ar.png) | ![Cube Collector](Docs/images/shot_game.png) |

---

<div dir="rtl">

## نبذة عن المشروع والرؤية

**محرك سَنَد (SANAD Engine)** هو مشروع محرك ألعاب ثلاثي الأبعاد متكامل، أسسه ويقوده **عبدالله**، بهدف إرساء بنية هندسية عربية متطورة ومنافسة في مجال محركات الألعاب. تم بناء المحرك من الصفر بالاعتماد على معيار **C++23** ومكتبة الرسوميات **Vulkan 1.2+** ومعمارية الكيانات والمكونات الموجهة للبيانات (**Data-Oriented ECS**).

### نداء للمطورين والمبدعين العرب

المحرك صُمم من اليوم الأول ليكون قابلاً للتوسع والتطوير المستمر بنظام معماري منفصل الطبقات. نرحب بانضمام كافة الكفاءات العربية لبناء هذا المشروع معاً:
- مبرمجو C++ وهندسة النظم.
- مبرمجو الرسوميات ومظللات Vulkan / Direct3D.
- مهندسو الصوتيات ومعالجة الإشارات.
- مطورو المحاكاة الفيزيائية والرياضيات التطبيقية.
- مطورو واجهات المستخدم وأدوات المحرر.
- كتاب التوثيق والمصممون وصناع المحتوى.

### مجالات المساهمة والتطوير المطلوبة

1. **الرسوميات والتظليل (Vulkan & Shaders):**
   - خرائط الظلال الاتجاهية (Directional Shadow Maps).
   - نظام السماء الإجرائية وتأثيرات الإضاءة الجوية.
   - تأثيرات ما بعد المعالجة (Bloom, Tonemapping, SSAO).

2. **محرك الصوتيات (Audio Engine):**
   - استكمال دمج مكتبة MiniAudio لدعم ملفات WAV و OGG.
   - مؤثرات معالجة الصوت الموقعي ثلاثي الأبعاد.

3. **لغات البرمجة والسكربت (Scripting):**
   - دمج لغة C# أو Lua لبرمجة منطق الألعاب بسلاسة.

4. **واجهة المحرر والتعريب (Editor & UI):**
   - دعم التخطيط العربي (RTL) وتشكيل الحروف في واجهة ImGui.
   - تحسين أدوات المعاينة والمقابض الحركية (Gizmos).

5. **المحاكاة الفيزيائية المتقدمة (Physics):**
   - دمج محرك Jolt Physics لدعم تصادمات الأجسام والشبكات المعقدة.

6. **إدارة واستيراد الأصول (Asset Pipelines):**
   - بناء مستورد لنماذج glTF 2.0 و FBX وتحويلها إلى صيغة المحرك.

للتفاصيل الكاملة، يرجى مراجعة [دليل المساهمة (CONTRIBUTING.md)](CONTRIBUTING.md) و[خارطة الطريق (ROADMAP.md)](ROADMAP.md).

</div>

---

## Architecture Overview

SANAD Engine is built on a modular, decoupled architecture with a multi-threaded job scheduler and strict layering enforcement.

```
SANAD Engine Architecture
┌─────────────────────────────────────────────────────────────────┐
│               SANAD Editor (ImGui + Win32 Docking)              │
├───────────────────────────────┬─────────────────────────────────┤
│    Gameplay Module Registry   │      NFPlayer Standalone        │
├───────────────────────────────┴─────────────────────────────────┤
│                   NFRuntime (World & Stepping)                  │
├──────────────────────┬─────────────────────────┬────────────────┤
│  NFRendering (PBR)   │   NFPhysics (SAT/Imp)   │ NFAudio (3D)   │
├──────────────────────┴─────────────────────────┴────────────────┤
│         NFEcs (Sparse-Set) & NFScene (Hierarchical Graph)       │
├─────────────────────────────────────────────────────────────────┤
│            NFJobs (Work-Stealing Multi-threaded Graph)          │
├─────────────────────────────────────────────────────────────────┤
│    NFRHI (Vulkan 1.2+ Low Overhead) & NFPlatform (Win32)        │
├─────────────────────────────────────────────────────────────────┤
│     NFCore (Custom Allocators, Pure Math, SIMD, Logging, UUID)  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Current Status (Phase 12–16 Verification)

- **733 Automated Tests Passed** across 20 test suites (0 failed, 1 benchmark skipped).
- **0 Vulkan Validation Layer Errors** and zero memory leaks.
- Clean compilation under `/W4 /WX` with MSVC.
- End-to-end deterministic frame loop stepping physics, skeletal animation, 3D spatial audio, gameplay modules, Lua scripts, input replays, and scene transform hierarchies in lockstep.
- Native dockable **ImGui Editor** with Outliner, Reflected Inspector, Asset Browser, Undo/Redo, 3D Viewport, Sky/Shadow environment editing, live Profiler, and full Arabic RTL localisation.
- Virtual File System (`content://`, `cache://`, `project://`, `saves://`) and standalone CLI project tooling (`nf new`, `nf build`, `nf run`).

---

## Implemented Subsystems

| Subsystem | Description and Capabilities |
| :--- | :--- |
| **Vulkan RHI & Rendering** | Low-overhead Vulkan 1.2+ backend, DAG RenderGraph, Multi-pass Deferred PBR Pipeline (GBuffer, Cook-Torrance GGX, Directional/Point/Spot lights, 2048 PCF shadows, procedural sky, Tonemapping, GPU Picking, distance LOD + LOD generator). |
| **Data-Oriented ECS** | Cache-friendly sparse-set Entity-Component-System with high memory locality and hierarchical transform propagation. |
| **Physics Solver** | Fully deterministic rigid-body solver (Sequential Impulses, Warm Starting, Baumgarte position correction, SAT narrowphase, Coulomb friction) + dynamic-body character controller + **Jolt v5.6 advanced backend**. |
| **Skeletal Animation** | Bone hierarchy evaluation, animation clips with slerp/lerp keyframe sampling, state machine with transitions and cross-fading, procedural clip generator, analytic two-bone IK. |
| **3D Spatial Audio** | 3D audio listener with attenuation models (Linear, Inverse, Exponential), stereo panning, WASAPI shared-mode backend, WAV/OGG/MP3/FLAC import pipeline, and headless test driver. |
| **Asset Pipeline** | glTF 2.0 importer (`.gltf`/`.glb` → `.nfmesh` via `NFModelImporter`), mesh cooking, and asset registry management. |
| **Lua Scripting** | Sandboxed Lua 5.4 VM with `nf.*` host library, entity bindings, per-entity `ScriptComponent` ticking, and instruction budgets. |
| **Gameplay Framework** | Hierarchical gameplay tags + queries, staged quest log, stacked inventory, dialogue trees, deterministic input replays, and CPU profiler with Chrome-trace export. |
| **Game AI** | Deterministic grid A* pathfinding (no corner cutting, LOS smoothing) + reactive behavior trees with blackboard. |
| **VFX** | Deterministic CPU particle simulation (emission, gravity/drag, grading). |
| **Arabic UI & Localization** | UCD-verified Arabic shaper (contextual forms, lam-alef, bidi), Amiri font pipeline, and EN/AR editor localisation. |
| **World & Time** | Day/night cycle driver, procedural heightfield terrain, and multiplayer: UDP + reliable channel + snapshots + authoritative server with client prediction. |
| **Reflection & Serialization** | Zero-codegen reflection macros (`NF_CLASS`, `NF_PROPERTY`, `NF_ENUM`), bidirectional text serialization, and automated inspector panels. |
| **Native Editor** | Dear ImGui docking shell, scene outliner, entity inspector, live viewport gizmos, undo/redo command history, and game save manager. |
| **Build & Packaging CLI** | `nf` CLI tool supporting project templating, cooking, asset registry management, and single-directory relocatable standalone distribution. |

---

## Quick Start and Build Instructions

### Prerequisites
- **Operating System:** Windows 10 / 11 (64-bit)
- **Compiler:** Visual Studio 2022 / 2026 (MSVC 19.40+) with C++23 support
- **Build Tools:** CMake 3.25+ and Ninja
- **Graphics SDK:** Vulkan SDK 1.3+ with `glslc` on your `PATH`

### 1. Clone the Repository
```bash
git clone https://github.com/abdallah2183/SANAD-Engine.git
cd SANAD-Engine
```

### 2. Build the Engine
Run the automated build script:
```cmd
build_nf.bat
```

Or configure and build directly via CMake:
```bash
cmake -S . -B build/DebugNinja -G Ninja -DCMAKE_BUILD_TYPE=Debug -DNF_BUILD_TESTS=ON -DNF_BUILD_SAMPLES=ON -DNF_BUILD_EDITOR=ON
cmake --build build/DebugNinja --parallel
```

### 3. Run the Editor
```cmd
.\build\DebugNinja\bin\NOVAForgeEditor.exe
```

### 4. Run Automated Tests
```cmd
.\build\DebugNinja\bin\EditorTests.exe
.\build\DebugNinja\bin\RHITests.exe
.\build\DebugNinja\bin\RuntimeTests.exe
```

### 5. Create and Package a Project
```cmd
# Create a new project from template
.\build\DebugNinja\bin\nf.exe new MyGame --name MyGame

# Cook assets and package standalone binary
.\build\DebugNinja\bin\nf.exe build --project MyGame/MyGame.nfproj

# Run standalone game player
cd MyGame/dist && .\NFPlayer.exe
```

---

## Project Roadmap

- **Phase 1–11:** Core Engine Foundation (Completed)
- **Phase 12:** Dynamic Mesh LOD Generation & Model Importer (Completed)
- **Phase 13:** Directional Shadow Mapping (PCF 3x3) & Procedural Sky Atmosphere (Completed)
- **Phase 14:** Compressed Audio Import (WAV/OGG/MP3/FLAC) & glTF 2.0 Asset Importer (Completed)
- **Phase 15:** Lua Scripting Integration (Completed) & Arabic RTL Editor Localisation (Completed)
- **Phase 16:** Jolt Physics Backend (Completed) + UDP/Reliable/Snapshots + Authoritative Server & Prediction (Completed) — vehicle/constraint replication (Planned)

Full details are documented in [ROADMAP.md](ROADMAP.md).

---

## المساهمة في التطوير

نرحب بكافة المساهمات وفق الخطوات التالية:
1. عمل **Fork** للمستودع.
2. إنشاء فرع عمل جديد (`git checkout -b feature/your-feature`).
3. بناء المشروع والتأكد من نجاح جميع الاختبارات (`build_nf.bat`).
4. التأكد من خلو تشغيل Vulkan من أي أخطاء في طبقات الفحص (Zero Validation Errors).
5. فتح **Pull Request** مع توضيح مفصل للتغييرات.

للمزيد من الإرشادات، يرجى قراءة [دليل المساهمة (CONTRIBUTING.md)](CONTRIBUTING.md).

---

## License

This project is licensed under the Apache License, Version 2.0. See the [LICENSE](LICENSE) file for details.

---

<p align="center">
  <strong>المؤسس والقائم على العمل:</strong> <a href="https://github.com/abdallah2183"><strong>عبدالله (Abdallah)</strong></a><br>
  <em>Founder & Project Lead: Abdallah (@abdallah2183) & Community Contributors</em>
</p>
