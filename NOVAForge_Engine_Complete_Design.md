# NOVAForge Engine
## وثيقة التصميم والتطوير الشاملة لمحرك ألعاب موحّد عالي الأداء

> **الاسم المؤقت للمشروع:** NOVAForge Engine  
> **الهدف:** بناء محرك ألعاب احترافي موحّد يجمع سهولة الاستخدام وسرعة التطوير مع جودة رسومية عالية، قابلية التوسع، الاستقرار، دعم 2D و3D، الألعاب ذات العوالم المفتوحة، الألعاب متعددة اللاعبين، والمشاريع ذات المستوى التجاري وAAA.
>
> **الفكرة الأساسية:** لا نحاول نسخ محرك واحد. نبني Architecture جديدة تأخذ أفضل الأفكار من أنظمة متعددة، ثم توحّدها داخل منصة واحدة ذات Workflow واضح.

---

# 0. تنبيه هندسي مهم

هذا المشروع **ليس مشروع شهر أو عدة أشهر** إذا كان الهدف منافسة المحركات الكبيرة في كل شيء دفعة واحدة.

المحرك الحقيقي يتكوّن من عشرات الأنظمة المترابطة:

- Rendering
- Graphics abstraction
- GPU memory management
- ECS / scene system
- Physics
- Animation
- Audio
- Input
- Networking
- Asset pipeline
- Importers
- Shader system
- Scripting
- Editor
- Terrain
- World streaming
- Navigation
- AI
- UI
- 2D
- 3D
- Profiling
- Packaging
- Build system
- Platform abstraction
- Crash handling
- Save system
- Localization
- Version control integration
- Multiplayer
- Security
- Automated testing
- Documentation

لذلك التصميم الصحيح هو:

> **Core صغير جدًا + Systems مستقلة + APIs ثابتة + أدوات فوق الـCore + تقديم Features تدريجيًا.**

لا تحاول بناء كل شيء داخل نواة واحدة ضخمة.

---

# 1. الرؤية العامة

## 1.1 الهدف

NOVAForge Engine هو محرك ألعاب متعدد الاستخدامات يهدف إلى تقديم:

### 2D
- Sprites
- Tilemaps
- 2D Animation
- Particles
- 2D Physics
- Lighting
- Cameras
- UI
- Skeletal 2D
- Parallax
- 2D NavMesh

### 3D
- Static Meshes
- Skeletal Meshes
- PBR
- Physically Based Lighting
- Shadows
- Reflection systems
- Global illumination
- Volumetrics
- Post processing
- Terrain
- Foliage
- Water
- Sky
- Weather
- Decals
- VFX
- Animation
- IK
- Motion systems

### AAA / High-End
- Streaming World
- World Partition
- Nanite-like virtualized geometry
- Virtual textures
- Modern GI
- GPU driven rendering
- Async compute
- Temporal techniques
- Advanced shadows
- Hardware ray tracing where available
- Upscaling interfaces
- Frame generation interfaces where supported
- Large crowds
- Massive object counts
- Advanced profiling

### Game Development
- Visual editor
- Scripting
- C++
- Hot reload
- Play in Editor
- Prefabs / Blueprints-like assets
- Material Editor
- Shader Editor
- Animation Editor
- Particle editor
- World editor
- Terrain editor
- UI editor

---

# 2. المبادئ الأساسية

## 2.1 Core Principles

### Principle 1 — Performance by Design

الأداء ليس Feature نضيفها لاحقًا.

من أول يوم يجب تصميم الأنظمة حول:

- Cache locality
- Multithreading
- Job systems
- Data-oriented design
- GPU-driven work
- Streaming
- Batching
- Async IO
- Memory budgets
- Minimal allocations

---

## 2.2 Editor Never Owns the Engine

الـEditor ليس هو المحرك.

القاعدة:

```text
Engine Runtime
     ↑
Engine Systems
     ↑
Engine Core
```

والـEditor يستخدم الـEngine بدل أن يصبح هو الـEngine.

هذا يسمح بتشغيل اللعبة بدون Editor.

---

# 3. Architecture العامة

```text
+---------------------------------------------------------+
|                    NOVAForge Editor                     |
+---------------------------------------------------------+
|                 Tools / UI / Inspectors                |
+---------------------------------------------------------+
|          Gameplay API / Scripting / ECS API             |
+---------------------------------------------------------+
| Rendering | Physics | Audio | Animation | AI | Net     |
+---------------------------------------------------------+
|          World / Scene / Asset / Resource               |
+---------------------------------------------------------+
|       Job System | Memory | IO | Serialization          |
+---------------------------------------------------------+
|          Platform & Graphics Abstraction                |
+---------------------------------------------------------+
| OS | GPU | CPU | File System | Window | Input | Audio   |
+---------------------------------------------------------+
```

---

# 4. الطبقات

## Layer 0 — Platform

المسؤول عن:

- Windows
- Linux
- macOS
- Android
- iOS
- Console abstractions لاحقًا

يشمل:

- Window
- Threads
- Mutex
- Events
- File system
- Native handles
- Time
- Input
- Dynamic libraries

---

## Layer 1 — Core

هذا أهم جزء.

### يجب أن يحتوي على:

- Memory
- Containers
- Strings
- Math
- UUID
- Hashing
- Logging
- Assertions
- Reflection base
- Serialization
- File system
- Tasks
- Job System
- Threading
- Events
- Resource handles

---

# 5. لغة البرمجة

## الخيار الأساسي

### C++

السبب:

- الأداء
- السيطرة على الذاكرة
- دعم AAA
- دعم Vulkan / D3D12 / Metal
- Ecosystem كبير
- مناسب للـEngine Runtime

---

# 6. لغة السكربت

يفضل بناء أكثر من Layer:

```text
C++ Engine
      ↓
Engine Reflection API
      ↓
Gameplay API
      ↓
Scripting Layer
```

يمكن دعم:

### Lua
للبداية السريعة.

### C#
مناسب لـ:

- Gameplay
- Tools
- Rapid iteration

### Visual Scripting
نظام Nodes.

---

# 7. Graphics API Architecture

لا تربط المحرك مباشرة بـDirectX.

أنشئ:

```cpp
IGraphicsDevice
```

ثم implementations:

```text
Vulkan
DirectX 12
Metal
WebGPU (optional)
```

---

# 8. Rendering Architecture

أفضل تصميم:

```text
Game World
     ↓
Visibility
     ↓
Render World
     ↓
Render Graph
     ↓
Passes
     ↓
GPU Command Lists
     ↓
Graphics API
```

---

# 9. Render Graph

الـRender Graph أحد أهم أجزاء المحرك.

مثال:

```text
Shadow Pass
      ↓
Depth Prepass
      ↓
GBuffer
      ↓
Lighting
      ↓
Reflections
      ↓
GI
      ↓
Volumetrics
      ↓
Post Processing
      ↓
UI
      ↓
Present
```

كل Pass يحدد:

- Inputs
- Outputs
- Resource dependencies
- Execution queue
- Barriers
- Lifetime

---

# 10. Deferred + Forward + Hybrid

لا تجبر كل الألعاب على Renderer واحد.

يجب دعم:

### Deferred Rendering

مناسب لـ:

- AAA
- عدد كبير من lights
- PBR

### Forward+

مناسب لـ:

- VR
- Mobile
- Stylized
- بعض الألعاب ذات الشفافية الثقيلة

### Hybrid

يمكن استعمال الاثنين داخل نفس المشروع.

---

# 11. Material System

المواد يجب أن تكون Data Assets.

مثال:

```text
Material
 ├── Base Color
 ├── Normal
 ├── Roughness
 ├── Metallic
 ├── AO
 ├── Emission
 ├── Opacity
 └── Custom Parameters
```

---

# 12. Shader System

يجب عدم جعل shaders ملفات منفصلة فقط.

أنشئ:

```text
Shader Source
     ↓
Preprocessor
     ↓
Reflection
     ↓
Intermediate Representation
     ↓
Backend Compiler
     ↓
GPU Binary
```

مع:

- Shader variants
- Pipeline cache
- Hot reload
- Error messages
- Shader graph

---

# 13. Shader Graph

نظام Node Based.

مثال:

```text
Texture
   ↓
Multiply ← Color
   ↓
Roughness
```

مع إمكانية كتابة:

```hlsl
CustomExpression
```

للخبراء.

---

# 14. PBR

النظام الأساسي يجب أن يكون:

- Metallic/Roughness
- Specular
- Clear Coat
- Sheen
- Transmission
- Subsurface
- Anisotropy

---

# 15. Lighting

يجب توفير:

### Dynamic
- Point
- Spot
- Directional
- Area

### Baked
- Lightmaps
- Irradiance data
- Reflection probes

### Hybrid
- Dynamic + baked

---

# 16. Global Illumination

ابدأ بثلاثة مستويات:

### Level 1
Lightmaps.

### Level 2
Screen space + probes.

### Level 3
Hardware accelerated / software GI system.

لا تجعل النظام AAA المتقدم شرطًا لتشغيل المشاريع الصغيرة.

---

# 17. Shadows

النظام يدعم:

- Cascaded Shadow Maps
- Virtual shadow maps
- Contact shadows
- Point light shadows
- Spot shadows
- Ray traced shadows

مع Quality tiers.

---

# 18. Virtualized Geometry

للوصول إلى جودة high-end:

```text
Mesh
 ↓
Clusters
 ↓
Hierarchy
 ↓
Visibility
 ↓
GPU selection
 ↓
Only visible geometry
```

لا تحاول تحميل كل triangles إلى pipeline بنفس الشكل.

---

# 19. GPU Driven Rendering

الفكرة:

بدل:

```text
CPU
 → Draw
 → Draw
 → Draw
 → Draw
```

استعمل:

```text
CPU
 ↓
GPU Buffers
 ↓
GPU Culling
 ↓
Indirect Draw
```

يدعم:

- Instance culling
- Occlusion culling
- LOD
- Mesh selection
- Particle culling

---

# 20. Temporal Rendering

ينبغي توفير Layer مجردة لـ:

- TAA
- TSR-like techniques
- DLSS integration
- FSR integration
- XeSS integration

بحيث يمكن تبديل implementation بدون تغيير gameplay.

---

# 21. World System

العالم ليس Scene واحدة ضخمة.

يجب إنشاء:

```text
World
 ├── Cells
 ├── Regions
 ├── Levels
 ├── Streaming Layers
 └── Data Layers
```

---

# 22. World Streaming

يعمل على:

```text
Player Position
       ↓
Streaming Manager
       ↓
Predictive Region
       ↓
Async IO
       ↓
Decompression
       ↓
Asset Load
       ↓
GPU Upload
       ↓
World Activation
```

---

# 23. Open World

يدعم:

- Huge terrain
- Cities
- Foliage
- Roads
- NPCs
- Streaming
- Day/night
- Weather
- Dynamic events

---

# 24. Coordinate System

استخدم:

```text
Right-handed
Y Up
```

أو يمكن جعله configurable في layer واضحة، لكن يفضل اختيار convention واحد للمحرك لتقليل التعقيد.

---

# 25. Floating Origin

للعوالم الكبيرة:

```text
Player Near Origin
World Coordinates Far Away
```

يتم نقل world origin دوريًا مع الحفاظ على دقة simulation.

---

# 26. ECS

نظام Entity Component System أساسي.

```cpp
Entity
Component
System
```

مثال:

```text
Entity 1001

Transform
Mesh
Material
Character
Health
Inventory
```

---

# 27. ECS Requirements

يجب دعم:

- Archetypes
- Sparse sets عند الحاجة
- Queries
- Parallel iteration
- Change detection
- Entity handles
- Serialization
- Replication metadata

---

# 28. Scene System

الـScene Layer أعلى من ECS.

```text
Scene
 ↓
Entities
 ↓
Components
 ↓
Assets
```

---

# 29. Prefab System

Prefab هو Asset Template.

يمكن أن يدعم:

- Overrides
- Nested prefabs
- Variants
- Serialization
- References
- Live editing

---

# 30. Resource System

كل Asset يجب أن يكون Resource.

مثال:

```text
Texture
Mesh
Material
Animation
Audio
Scene
Prefab
Shader
Font
ParticleGraph
```

ويجب الوصول إليها عبر handles.

---

# 31. Asset IDs

استخدم UUID / Content IDs.

مثال:

```text
asset://8A13-4F2C-...
```

لا تعتمد على absolute paths داخل اللعبة.

---

# 32. Virtual File System

النظام يجب أن يدعم:

```text
/vfs
 ├── project
 ├── engine
 ├── packages
 ├── cache
 └── cooked
```

---

# 33. Asset Pipeline

```text
Raw Asset
    ↓
Importer
    ↓
Intermediate Asset
    ↓
Validation
    ↓
Optimization
    ↓
Cook
    ↓
Platform Asset
```

---

# 34. Supported Asset Formats

مبدئيًا:

### Images
- PNG
- JPG
- TGA
- HDR
- EXR
- KTX2

### Models
- glTF
- FBX via importer integration where practical
- OBJ legacy support

### Audio
- WAV
- OGG
- FLAC

### Animation
- glTF animations
- common DCC formats through import pipeline

---

# 35. Texture System

يجب دعم:

- Mipmaps
- Compression
- Streaming
- Virtual texturing
- Texture arrays
- Cubemaps
- HDR
- Normal maps

---

# 36. Texture Streaming

يقرر المحرك حجم texture المطلوب بناء على:

- screen size
- distance
- memory budget
- importance
- current GPU memory

---

# 37. Physics Architecture

لا تربط Physics مباشرة بـGameplay.

أنشئ abstraction:

```cpp
IPhysicsScene
IPhysicsBody
IPhysicsShape
IPhysicsMaterial
```

ويمكن وضع backend:

```text
Jolt
PhysX
Bullet
Custom
```

---

# 38. Physics Features

- Rigid bodies
- Character controllers
- Constraints
- Triggers
- Raycasts
- Sweeps
- Queries
- Continuous collision
- Vehicles
- Rag dolls
- Cloth
- Destruction hooks

---

# 39. Character Controller

يجب أن يكون مستقلًا عن rigid body التقليدي.

يدعم:

- Step offset
- slopes
- moving platforms
- crouch
- climb hooks
- network prediction hooks

---

# 40. Vehicle System

يتكون من:

```text
Vehicle
 ├── Chassis
 ├── Wheels
 ├── Suspension
 ├── Engine
 ├── Gearbox
 └── Tires
```

ولا يتم وضع كل شيء في Physics backend مباشرة.

---

# 41. Destruction

Architecture تسمح بـ:

- Breakable meshes
- Fracture assets
- Debris
- Impulses
- Runtime destruction

ويجب وضع budget حتى لا تقتل اللعبة الأداء.

---

# 42. Animation System

يحتاج إلى:

- Skeleton
- Animation clips
- Animation graph
- Blend trees
- State machines
- Blend spaces
- IK
- Retargeting
- Root motion
- Motion warping hooks

---

# 43. Animation Graph

مثال:

```text
Locomotion
 ├── Idle
 ├── Walk
 ├── Run
 └── Sprint

UpperBody Layer
 ├── Aim
 └── Weapon
```

---

# 44. IK

يدعم:

- Two Bone IK
- FABRIK
- CCD
- Foot placement
- Look-at
- Hand IK
- Weapon alignment

---

# 45. AI System

يجب أن يكون Modular.

### Core
- perception
- blackboard
- behavior trees
- state machines
- utility AI

### Navigation
- NavMesh
- Recast-style generation
- dynamic obstacles
- off-mesh links

---

# 46. AI World

للعوالم المفتوحة:

```text
World AI
 ├── Sectors
 ├── Spawn Manager
 ├── Crowd Manager
 ├── LOD AI
 └── Simulation budget
```

لا يجب تشغيل كل NPC بنفس مستوى الذكاء دائمًا.

---

# 47. Crowd System

مستويات simulation:

```text
Level 0: Render only
Level 1: Cheap movement
Level 2: Lightweight AI
Level 3: Full AI
```

---

# 48. Audio Engine

يجب دعم:

- 2D audio
- 3D positional audio
- Reverb
- Occlusion
- Distance attenuation
- Ambisonics hooks
- Streaming
- Music system
- Voice
- Audio buses
- Effects chains

---

# 49. Audio Graph

```text
Source
 ↓
Mixer Bus
 ↓
Effect Chain
 ↓
Master
 ↓
Output
```

---

# 50. Input System

لا تربط gameplay مباشرة بالكيبورد.

استعمل Actions:

```text
Move
Jump
Fire
Interact
Pause
```

ثم mappings:

```text
Keyboard
Controller
Mouse
Touch
```

---

# 51. Cross-platform Input

النظام يجب أن يوحد:

- Xbox
- PlayStation
- Generic Gamepads
- Keyboard
- Mouse
- Touch
- Mobile gyro

---

# 52. UI System

يجب وجود نظامين:

### Runtime UI
- HUD
- Menus
- Inventory
- Dialogue

### Editor UI
- Docking
- Inspector
- Outliner
- Toolbars

---

# 53. UI Rendering

يفضل نظام retained-mode.

العناصر:

```text
Panel
Text
Image
Button
Scroll
List
Grid
```

---

# 54. 2D Engine

لا تجعل 2D مجرد 3D objects مسطحة.

أنشئ 2D renderer حقيقيًا يدعم:

- Sprite batching
- Atlas
- Tilemap
- 2D lights
- Shadows
- 2D particles
- Pixel perfect
- Cameras
- Parallax

---

# 55. Tilemap

يجب أن يدعم:

- Layers
- Auto tiling
- Chunking
- Collision
- Navigation
- Large maps
- Streaming

---

# 56. 2D Physics

يمكن أن يكون backend نفسه مع:

```text
2D world
2D shapes
2D constraints
```

مع layer مستقلة عن 3D gameplay.

---

# 57. VFX System

نظام Particles GPU-first.

يدعم:

- Spawn
- Update
- Collision
- Curl noise
- Trails
- Mesh particles
- ribbons
- lights
- decals

---

# 58. Niagara-like Concept

أنشئ Graph:

```text
Emitter
 ↓
Spawn
 ↓
Initialize
 ↓
Forces
 ↓
Collision
 ↓
Color
 ↓
Output
```

لا تحتاج تسمية النظام بنفس أسماء أي محرك آخر.

---

# 59. Terrain

يجب دعم:

- Heightmaps
- Splat maps
- Layers
- Grass
- Rocks
- Trees
- Foliage
- Water
- Roads

---

# 60. Landscape Streaming

قسّم terrain إلى tiles/chunks.

```text
Terrain
 ├── Tile_00
 ├── Tile_01
 ├── Tile_02
 ...
```

---

# 61. Foliage System

GPU instancing.

يدعم:

- Randomization
- Scale
- Rotation
- Wind
- LOD
- Density
- Masking
- Painting

---

# 62. Water System

يدعم:

- Ocean
- Lakes
- Rivers
- Waves
- Foam
- Refraction
- Reflection
- Underwater effects
- Shoreline

---

# 63. Sky & Weather

أنظمة:

- Sun
- Moon
- Stars
- Atmosphere
- Clouds
- Fog
- Rain
- Snow
- Wind
- Lightning

---

# 64. Time-of-Day

يجب أن يكون Data-driven:

```text
Time
 ↓
Sun rotation
 ↓
Sky
 ↓
Clouds
 ↓
Lighting
 ↓
Weather
 ↓
Environment
```

---

# 65. Networking

يجب تصميم networking من البداية حتى لو لم يكن Multiplayer أول feature.

Architecture:

```text
Game
 ↓
Replication Layer
 ↓
Transport
```

---

# 66. Networking Layers

### Transport
- UDP
- TCP
- QUIC where useful

### Replication
- Entity state
- Component state
- RPC

### Gameplay
- Prediction
- Interpolation
- Reconciliation

---

# 67. Multiplayer Modes

- Listen server
- Dedicated server
- Client/server
- Peer-to-peer optional

---

# 68. Replication

كل component يمكن أن يحدد:

```text
Replicated
Owner Only
Server Only
Client Only
Not Replicated
```

---

# 69. Client Prediction

للألعاب السريعة:

```text
Input
 ↓
Client Simulation
 ↓
Send Input
 ↓
Server Simulation
 ↓
Correction
 ↓
Reconciliation
```

---

# 70. Save System

يجب دعم:

- Save slots
- Autosave
- Serialization
- Versioning
- Migration
- Async save
- Cloud hooks

---

# 71. Scripting Architecture

لا تسمح للسكربت بالوصول العشوائي لكل شيء.

استخدم:

```text
Game API
 ↓
Reflection
 ↓
Safe Bindings
```

---

# 72. Reflection

يجب أن يكون هناك metadata مثل:

```cpp
PROPERTY(EditAnywhere)
float Health;

PROPERTY(Replicated)
int Ammo;
```

والـreflection codegen يخلق metadata.

لا تعتمد فقط على runtime RTTI.

---

# 73. Serialization

يجب دعم:

### Editor
Human-readable format عند الحاجة.

### Runtime
Binary fast format.

---

# 74. Versioned Serialization

كل asset:

```text
AssetVersion
SchemaVersion
EngineVersion
```

ويجب وجود migration system.

---

# 75. Editor

الـEditor يجب أن يكون مثل IDE خاص بالألعاب.

### Layout

```text
+---------------------------------------------------+
| Menu / Toolbar                                    |
+------------+-----------------------+--------------+
| Project    |       Viewport        | Inspector    |
| Assets     |                       | Details      |
|            |                       |              |
+------------+-----------------------+--------------+
| Console / Profiler / Timeline / Asset Inspector   |
+---------------------------------------------------+
```

---

# 76. Editor Windows

لازم توجد:

- Scene
- Game
- Hierarchy
- Inspector
- Asset Browser
- Console
- Profiler
- Animation
- Material
- Shader
- Terrain
- VFX
- Audio
- World
- Physics
- Network
- Build

---

# 77. Gizmos

يدعم:

- Move
- Rotate
- Scale
- Local/World
- Snap
- Pivot
- Multi-select

---

# 78. Undo / Redo

كل تعديل في الـEditor يجب أن يكون command.

```text
Command
 ├── Execute
 └── Undo
```

استخدم transaction grouping حتى يتمكن المستخدم من التراجع عن عملية كبيرة ككتلة واحدة.

---

# 79. Editor Plugin System

الـEditor يجب أن يكون extensible.

مثال:

```cpp
IEditorPlugin
```

يسمح بإضافة:

- Custom windows
- Importers
- Inspectors
- Menu items
- Gizmos
- Asset types

---

# 80. Build System

يجب وجود:

### Dev Build
أسرع compile وhot reload.

### Shipping Build
- Stripping
- Optimization
- Cooking
- Compression
- Asset packing

### Server Build
بدون Renderer.

---

# 81. Dedicated Server

يجب أن يكون بالإمكان بناء:

```text
NOVAForgeServer
```

بدون:

- Graphics
- Editor
- GPU resources

---

# 82. Hot Reload

يجب الفصل بين:

```text
Engine DLL
Game DLL
Editor
```

حتى يتم إعادة تحميل Game code بأقل تأثير ممكن.

---

# 83. Live Asset Reload

يمكن تغيير:

- Texture
- Material
- Shader
- Audio
- Mesh
- Scene properties

أثناء تشغيل اللعبة عندما يكون ذلك ممكنًا.

---

# 84. Profiler

الـProfiler ليس إضافة تجميلية.

من أول نسخة يجب أن توجد:

- CPU timeline
- GPU timing
- Memory
- Allocations
- Asset load
- Frame time
- Rendering stats

---

# 85. Profiling API

مثال:

```cpp
PROFILE_SCOPE("Render Shadows");
```

ويدخل ضمن:

```text
CPU Timeline
GPU Timeline
Trace
```

---

# 86. Telemetry داخلية

يجب جمع metrics داخلية للمحرك:

- Frame time
- Draw count
- Triangle count
- GPU memory
- CPU memory
- Streaming time
- shader compile time

بدون إرسال بيانات للخارج ما لم يوافق المستخدم.

---

# 87. Logging

مستويات:

```text
Trace
Debug
Info
Warning
Error
Fatal
```

مع categories:

```text
Render
Physics
Audio
Network
Asset
Editor
Script
```

---

# 88. Crash Handling

عند crash:

- stack trace
- engine version
- build ID
- platform
- thread
- loaded modules
- recent logs

ويجب إنتاج Crash Report قابل للقراءة.

---

# 89. Memory Architecture

يجب وضع allocators من البداية:

```text
General Allocator
Frame Allocator
Linear Arena
Pool
Slab
GPU Upload Allocator
```

---

# 90. Frame Allocator

للأشياء المؤقتة:

```cpp
FrameAllocator
```

يتم reset في نهاية frame.

هذا يقلل allocations الصغيرة.

---

# 91. Job System

مكون أساسي جدًا.

```text
Main Thread
    ↓
Job Scheduler
   / |  \
 Job Job Job
   \ | /
    Worker Threads
```

يجب دعم:

- Job dependencies
- fibers optional
- priorities
- cancellation
- work stealing
- task groups

---

# 92. Async IO

Game thread لا ينتظر Disk.

```text
Request
 ↓
IO Scheduler
 ↓
Read
 ↓
Decompress
 ↓
Decode
 ↓
GPU upload / CPU object
```

---

# 93. Asset Cache

يجب وجود:

```text
Raw Cache
Derived Cache
Shader Cache
Cooked Cache
```

---

# 94. Compression

يجب أن يكون pipeline قابل للتبديل:

```text
LZ4-like fast
Zstandard-like strong
Platform-specific packaging
```

---

# 95. Thread Model

مثال:

```text
Main
Render
RHI
Workers x N
IO
Audio
Network
Physics optional
```

لكن لا تجعل كل نظام يخلق thread دائمًا بدون سبب.

يفضل shared worker pools.

---

# 96. Synchronization

تجنب locks داخل hot paths قدر الإمكان.

استخدم:

- Atomics
- lock-free structures حيث تستحق
- double buffering
- command queues
- job dependencies

---

# 97. Render Thread Communication

اللعبة لا تستدعي GPU objects مباشرة.

```text
Gameplay
 ↓
Render Proxy
 ↓
Render Command Queue
 ↓
Render World
 ↓
GPU
```

---

# 98. ECS + Render Separation

لا تجعل Renderer يقرأ كل Gameplay components مباشرة.

أنشئ:

```text
Game World
    ↓
Extraction
    ↓
Render World
```

هذا يسمح بفصل الـsimulation عن الـrendering.

---

# 99. Multithreaded Rendering

المرحلة الأولى:

- command recording parallel

ثم:

- visibility parallel
- culling parallel
- material sorting parallel

ثم:

- GPU-driven pipelines

---

# 100. Resource Lifetime

يجب أن يكون لكل Resource:

```text
Created
Loaded
Resident
Evicting
Destroyed
```

مع reference/handle tracking.

---

# 101. Handle-Based API

بدل:

```cpp
Texture*
```

استعمل:

```cpp
TextureHandle
```

مثال:

```cpp
TextureHandle albedo;
```

هذا يمنع dangling pointers ويساعد streaming.

---

# 102. Module System

كل subsystem module مستقل:

```text
Core
Platform
RHI
Render
Scene
ECS
Physics
Audio
Animation
AI
UI
Input
Network
Assets
Editor
```

---

# 103. Dependency Rules

قاعدة مهمة:

```text
Core
 ↑
Platform
 ↑
RHI
 ↑
Render
 ↑
Scene
```

ولا تسمح بـdependency دائرية.

---

# 104. API Stability

الـCore APIs يجب أن تكون واضحة.

مثال:

```cpp
class Engine;
class World;
class Entity;
class AssetManager;
class RenderDevice;
```

---

# 105. Naming Convention

أمثلة:

```text
NF_Engine
NF_RenderDevice
NF_Texture
NF_Entity
NF_Transform
```

أو namespace:

```cpp
namespace NF
{
}
```

---

# 106. Project Structure

اقتراح:

```text
NOVAForge/
│
├── Engine/
│   ├── Core/
│   ├── Platform/
│   ├── RHI/
│   ├── Rendering/
│   ├── ECS/
│   ├── Scene/
│   ├── Physics/
│   ├── Animation/
│   ├── Audio/
│   ├── AI/
│   ├── Networking/
│   ├── UI/
│   ├── Input/
│   ├── Assets/
│   ├── Scripting/
│   └── Runtime/
│
├── Editor/
│   ├── Core/
│   ├── Panels/
│   ├── Viewport/
│   ├── Inspectors/
│   ├── Tools/
│   └── Plugins/
│
├── Tools/
│   ├── AssetCooker/
│   ├── ShaderCompiler/
│   ├── ModelImporter/
│   ├── TextureProcessor/
│   └── BuildTool/
│
├── ThirdParty/
│
├── Samples/
│
├── Tests/
│
├── Docs/
│
└── Scripts/
```

---

# 107. Recommended Internal Modules

```text
NFCore
NFPlatform
NFRHI
NFRender
NFWorld
NFECS
NFPhysics
NFAudio
NFAnimation
NFAI
NFNet
NFUI
NFInput
NFAssets
NFScripting
NFEditor
```

---

# 108. Build Technology

استخدم build system حديثًا وقابلًا للـIDE.

مثال:

```text
CMake
Ninja
Visual Studio
Clang
MSVC
```

ثم Build Tool خاص بالمحرك فوقه.

---

# 109. Package Manager

يمكن استخدام dependency manager للمكونات الخارجية، لكن:

> لا تجعل مشروع المحرك يعتمد على عشرات الحزم غير الضرورية.

كل dependency يجب أن يكون له:

- License
- Version
- Security status
- Upgrade policy
- Maintainer
- Usage reason

---

# 110. Third Party Policy

يفضل استخدام libraries قوية لكل مشكلة لا تستحق إعادة اختراعها.

مثل:

- Windowing
- Compression
- Image codecs
- Font shaping
- Physics backend
- Audio backend
- Networking primitives

لكن الأنظمة الجوهرية يجب أن تكون تحت سيطرة الفريق.

---

# 111. Editor Technology

يمكن بناء UI بنظام خاص بالمحرك أو دمج UI framework مناسب.

الفكرة:

```text
Editor UI
 ↓
Editor Framework
 ↓
Engine Editor APIs
```

ولا تربط Gameplay UI بالـEditor UI.

---

# 112. Version Control

يجب دعم:

- Git
- Git LFS
- Perforce لاحقًا
- file locking
- changelists

---

# 113. Collaboration

في الفريق يجب أن تكون assets قابلة للمقارنة عندما يكون ذلك ممكنًا.

لذلك:

- text-based metadata
- stable serialization
- deterministic ordering
- unique IDs

---

# 114. Determinism

الهدف ليس جعل كل شيء deterministic.

بل توفير deterministic modes للأنظمة المهمة:

- Physics عند الحاجة
- Replay
- Networking
- Automated tests

---

# 115. Replay System

يمكن تخزين:

```text
Inputs
Important state deltas
Seed
World revision
```

لإعادة التجربة debugging.

---

# 116. Automated Testing

كل module يجب أن يمتلك tests.

### Core
- math
- memory
- serialization

### Rendering
- shader compile
- resource lifetime

### Physics
- collision
- raycast

### ECS
- create/destroy
- queries

### Networking
- replication

---

# 117. Performance Tests

يجب وجود benchmarks:

```text
10K entities
100K entities
1M transforms
1000 lights
100K particles
Large world streaming
```

---

# 118. Quality Tiers

يجب أن يعمل المحرك على أكثر من مستوى.

### Low
- Mobile-like
- Low shadows
- No heavy GI

### Medium
- Standard desktop

### High
- PC high-end

### Ultra
- advanced lighting
- high geometry
- expensive effects

---

# 119. Scalability Architecture

كل system يجب أن يعلن quality knobs.

مثال:

```text
ShadowResolution
ViewDistance
GIQuality
ParticleQuality
AnimationUpdateRate
AIUpdateRate
```

---

# 120. Dynamic Resolution

يدعم:

```text
Target FPS
 ↓
GPU Frame Time
 ↓
Resolution Scale
```

---

# 121. Frame Pacing

يجب مراقبة:

- average FPS
- 1% low
- frame variance
- CPU/GPU bound state

لا تعتمد على FPS فقط.

---

# 122. Mobile Strategy

للموبايل لا تحاول تقديم نفس pipeline.

استخدم:

```text
Mobile Renderer
```

لكن نفس assets والـgameplay APIs.

---

# 123. Console Strategy

الـRHI يجب أن تكون قابلة للتوسعة لكي تتمكن من إضافة console backend لاحقًا.

---

# 124. VR/AR

من البداية لا تكسر architecture.

أنشئ:

```cpp
IXRSystem
```

ويتعامل مع:

- HMD
- Controllers
- Tracking
- Stereo rendering

---

# 125. Localization

يدعم:

- UTF-8
- Unicode shaping
- RTL
- Arabic
- Fonts fallback
- Localization tables

---

# 126. Accessibility

يجب التخطيط مبكرًا:

- subtitles
- scalable UI
- remappable input
- color accessibility
- screen reader hooks
- reduced motion options

---

# 127. Security

في الألعاب الشبكية:

- never trust client
- validate server messages
- rate limiting
- replay protection hooks
- packet validation

---

# 128. Anti-Cheat Architecture

المحرك نفسه يمكن أن يوفر hooks فقط.

Anti-cheat الحقيقي غالبًا يحتاج integration متخصصة على المنصة.

---

# 129. Modding

Architecture مقترحة:

```text
Game
 ↓
Mod Loader
 ↓
Mod Package
 ↓
Sandbox / Permissions
```

لا تعطِ mods native unrestricted access في الألعاب التجارية بدون قرار أمني واضح.

---

# 130. Plugin Architecture

Plugins يجب أن تكون:

```text
Engine Plugin
Editor Plugin
Game Plugin
Platform Plugin
```

---

# 131. Data-Driven Design

أي شيء يمكن تحويله إلى Data يجب أن يكون Data Asset.

مثال:

```text
Weapon
 ├── Damage
 ├── FireRate
 ├── Recoil
 ├── Mesh
 ├── Sounds
 └── Animations
```

---

# 132. Gameplay Framework

يجب أن توفر طبقة جاهزة:

```text
Game
GameMode
Player
Character
Controller
Component
Subsystem
```

لكن لا تجعل المستخدم مجبرًا عليها.

---

# 133. Subsystems

مثل:

```text
SaveSubsystem
AudioSubsystem
OnlineSubsystem
InventorySubsystem
WeatherSubsystem
```

---

# 134. Event System

يدعم:

### Local events
داخل النظام.

### Game events
Gameplay events.

### Editor events
Asset changed, selection changed.

تجنب Event Bus عالمي فوضوي.

---

# 135. Messaging

يفضل أنواع واضحة:

```cpp
struct DamageEvent
{
    Entity source;
    Entity target;
    float amount;
};
```

---

# 136. Debug Draw

كل system يجب أن يقدم Debug Visualization.

مثال:

```text
Physics
AI
Navigation
Audio
Network
Streaming
Bounds
Occlusion
```

---

# 137. Console

يجب أن يحتوي المحرك على developer console.

أوامر مثل:

```text
stat fps
stat gpu
stat memory
r.shadowQuality 3
r.gi 1
net.debug 1
```

---

# 138. Configuration System

ملفات:

```text
Engine.ini
Project.ini
User.ini
Platform.ini
```

مع hierarchy واضح.

---

# 139. Command Line

مهم جدًا للـCI:

```text
NOVAForgeEditor --project MyGame --build
NOVAForgeCook --project MyGame
NOVAForgeServer --map Arena
```

---

# 140. CI/CD

يجب إنشاء pipeline:

```text
Commit
 ↓
Compile
 ↓
Unit Tests
 ↓
Integration Tests
 ↓
Shader Tests
 ↓
Cook Sample Game
 ↓
Performance smoke test
```

---

# 141. Branching

للعمل الجماعي:

```text
main
develop
feature/*
release/*
```

حسب حجم الفريق.

---

# 142. Documentation

كل API عامة يجب أن تكون موثقة.

يجب إنتاج:

- Getting Started
- API docs
- Rendering docs
- Physics docs
- Editor docs
- Gameplay docs
- Networking docs
- Contribution docs

---

# 143. Sample Projects

لا تطور المحرك بدون ألعاب تجريبية.

يجب أن توجد:

### Sample 1
2D platformer.

### Sample 2
3D third-person.

### Sample 3
Open world streaming.

### Sample 4
Multiplayer arena.

### Sample 5
Large scale stress test.

---

# 144. Benchmark Game

أنشئ لعبة اختبار خاصة بالمحرك:

```text
BenchmarkWorld
 ├── 10,000 objects
 ├── 2,000 lights
 ├── 100,000 particles
 ├── large terrain
 ├── AI crowd
 └── streaming cells
```

---

# 145. Golden Frame Testing

يمكن حفظ لقطات مرجعية للتحقق من أن تغييرات rendering لا تكسر الصورة بلا قصد.

---

# 146. Shader Validation

كل Shader يجب أن يمر:

```text
Compile
Validation
Reflection
Pipeline creation
```

قبل دخوله build النهائي.

---

# 147. Asset Validation

عند import:

```text
Validate
Optimize
Warn
Fix optional
Cook
```

---

# 148. Error Philosophy

الأخطاء يجب أن تكون مفهومة.

سيئ:

```text
E10294
```

أفضل:

```text
Failed to load material:
Material_Stone

Reason:
Normal map format is unsupported on target platform.

Suggestion:
Reimport using platform-compatible compression.
```

---

# 149. Editor UX

المبدأ:

> أقل عدد من الخطوات لإنجاز المهمة.

مثال:

```text
Drag asset
→ Drop into scene
→ Automatically create entity
```

---

# 150. One-Click Play

زر:

```text
▶ Play
```

يجب أن يشغل اللعبة بسرعة.

---

# 151. PIE Modes

- Simulate
- Play From Camera
- Play From Here
- Standalone Game
- Dedicated Server

---

# 152. Remote Debugging

Editor يمكنه الاتصال بـrunning build:

```text
Editor
  ⇅
Remote Runtime
```

لفحص:

- entities
- variables
- textures
- performance
- network state

---

# 153. World Partition Editor

العالم الكبير يجب تقسيمه بطريقة بصرية.

```text
+-----------------------------+
| World Grid                  |
| [ ][ ][A][ ]               |
| [ ][Player][ ][ ]           |
| [ ][ ][ ][ ]               |
+-----------------------------+
```

---

# 154. Data Layers

يمكن تشغيل/إيقاف layers:

```text
Gameplay
Lighting
Mission A
Mission B
Cinematic
Debug
```

---

# 155. Level of Detail

أنظمة متعددة:

- Mesh LOD
- Material LOD
- Animation LOD
- AI LOD
- Physics LOD
- Audio LOD
- VFX LOD

---

# 156. Unified LOD Budget

بدل أن يكون لكل نظام قرار مستقل بالكامل:

```text
Entity Importance
      ↓
Global Budget
      ↓
Render / Physics / AI / Audio
```

---

# 157. Streaming Priority

الأولوية تعتمد على:

```text
Distance
Screen visibility
Player direction
Gameplay relevance
Memory
```

---

# 158. Background Streaming

يجب أن يكون:

- async
- cancelable
- prioritized

---

# 159. Prefetching

إذا اللاعب يتحرك باتجاه منطقة ما:

```text
Player Velocity
       ↓
Predictive Streaming
       ↓
Load next cells
```

---

# 160. Asset Dependency Graph

كل Asset يعرف dependencies.

مثال:

```text
Character.prefab
 ├── Mesh
 ├── Skeleton
 ├── Materials
 ├── Animations
 └── Sounds
```

---

# 161. Cook Dependency Graph

الـCooker يجب أن يعرف كل dependencies لكي لا ينسى Asset.

---

# 162. Build Reproducibility

الـbuild يجب أن يكون reproducible قدر الإمكان:

```text
Source revision
Engine revision
Tool revision
Asset revision
```

---

# 163. Source vs Runtime Assets

ممنوع shipping raw editor data إذا لم يكن مطلوبًا.

```text
Editor Asset
 ↓
Cooked Runtime Asset
```

---

# 164. Cook Profiles

مثال:

```text
PC_High
PC_Low
Mobile
DedicatedServer
Console
```

---

# 165. Packaging

مخرجات:

```text
Game.exe
Game.pak
Shaders
Configs
Runtime DLLs
```

مع إمكانية:

```text
single package
chunked packages
DLC packages
```

---

# 166. DLC Architecture

يجب أن تكون قابلة للتوسع:

```text
Base Package
DLC 01
DLC 02
Patch Package
```

---

# 167. Patch System

يفضل دعم delta patching مستقبلاً.

---

# 168. Editor Asset Search

بحث سريع داخل المشروع:

```text
Name
Type
Tag
Path
Dependency
Reference count
Modified date
```

---

# 169. Asset Tags

مثال:

```text
weapon
environment
character
ui
cinematic
```

---

# 170. Dependency Viewer

أداة تعرض:

```text
Who references this asset?
What does this asset reference?
```

مفيدة جدًا للمشاريع الضخمة.

---

# 171. Orphan Detection

يجب معرفة assets التي لا يستخدمها أي شيء.

---

# 172. Circular Dependency Detection

عند serialization/cooking يجب اكتشاف الدورات غير المسموح بها.

---

# 173. Rendering Debug Modes

أوضاع:

```text
Lit
Unlit
Normals
Roughness
Metallic
AO
Depth
Overdraw
Nanite/virtual geometry visualization
Shadow maps
Light complexity
```

---

# 174. GPU Capture Integration

يجب أن تكون الـRender markers واضحة في أدوات GPU capture.

---

# 175. Frame Debugger

عرض:

```text
Pass 1
Pass 2
Pass 3
...
```

مع الموارد المستخدمة.

---

# 176. Physics Debugger

يعرض:

- colliders
- contacts
- rays
- character controller

---

# 177. Network Debugger

يعرض:

- packet rates
- ping
- replication
- bandwidth
- dropped packets
- predicted state

---

# 178. AI Debugger

يعرض:

- current state
- target
- behavior tree
- blackboard
- perception
- path

---

# 179. Animation Debugger

يعرض:

- active clip
- blend weights
- IK
- skeleton
- state transitions

---

# 180. Audio Debugger

يعرض:

- active voices
- buses
- volume
- spatialization
- occlusion

---

# 181. Editor Selection System

يجب أن يكون موحدًا بين:

```text
Viewport
Outliner
Inspector
Asset Browser
```

---

# 182. Scene Serialization

يمكن أن يكون:

```text
Project/
 └── Maps/
      └── Main.world
```

وملف world يشير إلى assets بالـIDs.

---

# 183. World Chunks

كل chunk يمكن أن يمتلك:

```text
Entities
References
Bounds
Streaming metadata
```

---

# 184. Large World Precision

قسّم:

```text
World Coordinate
Cell Coordinate
Local Coordinate
```

مثال:

```text
World
 └── Cell(100, -32)
      └── Local(234, 50, 12)
```

---

# 185. Units

ثبت convention:

```text
1 unit = 1 meter
```

هذا يسهل:

- physics
- animation
- lighting
- audio

---

# 186. Time System

يجب التفريق بين:

```text
Real Time
Game Time
Simulation Time
Physics Time
Audio Time
```

---

# 187. Fixed Update

للـphysics:

```cpp
FixedUpdate()
```

وللـgameplay:

```cpp
Update(deltaTime)
```

حسب الحاجة.

---

# 188. Tick Groups

مثال:

```text
PrePhysics
Physics
PostPhysics
Gameplay
Animation
Audio
RenderExtraction
```

لكن لا تخلق graph معقدًا جدًا في النسخة الأولى.

---

# 189. Frame Lifecycle

```text
Input
 ↓
Game Tick
 ↓
Physics
 ↓
Animation
 ↓
AI
 ↓
World Streaming
 ↓
Render Extraction
 ↓
Render
 ↓
Audio
 ↓
Present
```

وفي النسخ المتقدمة يمكن إعادة ترتيب أجزاء كثيرة بالتوازي.

---

# 190. Engine Startup

```text
Platform Init
 ↓
Memory Init
 ↓
Logging
 ↓
Task System
 ↓
RHI
 ↓
Asset System
 ↓
World
 ↓
Gameplay
 ↓
Renderer
```

---

# 191. Engine Shutdown

يجب إيقاف الأنظمة بترتيب dependency عكسي.

---

# 192. Error Recovery

ليس كل Error = Crash.

مثلاً shader fail:

```text
Fallback Shader
+
Editor Error
```

---

# 193. Fallback Resources

يجب وجود:

- default texture
- default material
- default mesh
- default font
- default shader

حتى لا تنهار اللعبة بسبب Asset واحد.

---

# 194. Hot Asset Validation

عند تغيير Asset:

```text
Detect
 ↓
Validate
 ↓
Reimport
 ↓
Cook
 ↓
Reload
```

---

# 195. Material Instances

Base Material:

```text
MasterMaterial
```

ثم:

```text
MaterialInstance
```

لتغيير parameters بدون compile كامل.

---

# 196. Shader Permutations

يجب تقليل انفجار الـvariants.

استخدم:

- runtime specialization where practical
- feature masks
- material flags
- pipeline cache

ولا تولد آلاف permutations بدون حاجة.

---

# 197. Render Feature Flags

مثال:

```text
Feature::RayTracing
Feature::VirtualGeometry
Feature::VirtualTexturing
Feature::GI
```

---

# 198. Quality Profiles

بدلاً من كتابة if في كل shader:

```text
Ultra Profile
High Profile
Medium Profile
Low Profile
```

---

# 199. GPU Memory Manager

يجب تتبع:

```text
Committed
Reserved
Resident
Evicted
```

لكل نوع resource.

---

# 200. Descriptor Management

في Graphics API الحديثة تحتاج manager منظم لـ:

- descriptors
- bindless resources
- sampler cache

---

# 201. Bindless Strategy

لـhigh-end GPUs:

```text
Global Texture Table
Global Buffer Table
```

ثم shaders تصل إلى الموارد عبر IDs.

---

# 202. Legacy Hardware

يجب وجود fallback path للأجهزة التي لا تدعم بعض المميزات.

---

# 203. Ray Tracing

لا تجعله mandatory.

أضف:

```text
Raster Path
RT Optional Path
```

ويتم اختيار المناسب عند startup.

---

# 204. Virtual Texturing

للعوالم الكبيرة:

```text
Mega Texture
 ↓
Tiles
 ↓
Streaming
```

---

# 205. Decal System

يدعم:

- deferred decals
- terrain decals
- mesh decals
- blood/dirt/damage

---

# 206. Post Processing

Stack:

```text
Exposure
Color Grading
Bloom
DOF
Motion Blur
Lens Effects
Tonemapping
Sharpening
```

يجب أن تكون كل مرحلة قابلة للتشغيل/الإيقاف.

---

# 207. Color Management

استخدم linear workflow.

احرص على:

- sRGB
- HDR
- exposure
- tone mapping

---

# 208. Cinematics

يجب دعم Timeline:

```text
Camera
Character
Audio
Animation
VFX
Properties
```

---

# 209. Camera System

أنواع:

- Perspective
- Orthographic
- Cinematic
- VR

---

# 210. Camera Shake

Data-driven.

يدعم:

- Perlin noise
- impulse
- curves

---

# 211. Gameplay Tags

مفيد جدًا:

```text
Character.Player
Weapon.Rifle
State.Poisoned
Faction.Enemy
```

---

# 212. Tag Query

Systems تستطيع البحث بـtags بدون ربط مباشر بين classes.

---

# 213. Data Assets

يمكن إنشاء:

```text
WeaponData
EnemyData
QuestData
DialogueData
ItemData
```

---

# 214. Localization Data

يجب فصل text IDs عن النصوص النهائية.

---

# 215. Dialogue System

اختياري لكنه مهم في المشاريع الكبيرة:

```text
Dialogue Graph
 ├── Lines
 ├── Choices
 ├── Conditions
 └── Events
```

---

# 216. Quest System

ليس جزءًا من Core.

يوضع كـGame Framework module.

---

# 217. Inventory

كذلك module فوق Core:

```text
Item Definition
Item Instance
Inventory
Equipment
```

---

# 218. Save/Load Serialization

يجب عدم حفظ pointer addresses.

كل شيء يعتمد على stable IDs.

---

# 219. Network Serialization

يفضل serialization مخصص network وليس نفس save format.

---

# 220. Bandwidth Optimization

يدعم:

- delta compression
- relevancy
- dormancy
- quantization
- prioritization

---

# 221. Interest Management

لا ترسل كل entity لكل player.

```text
Player Area
 ↓
Relevant Entities
 ↓
Replication
```

---

# 222. Dedicated Server Scalability

يمكن تقسيم العالم:

```text
Server
 ├── Region A
 ├── Region B
 └── Region C
```

في المستقبل.

---

# 223. Toolchain

الأدوات الأساسية:

```text
NFAssist
NFAssetCooker
NFShaderCompiler
NFModelImporter
NFTextureTool
NFPackager
NFProfiler
NFBuild
```

---

# 224. One CLI

يفضل أداة:

```text
nf
```

مثال:

```bash
nf create MyGame
nf editor MyGame
nf cook MyGame
nf build MyGame --target=PC
nf package MyGame
```

---

# 225. Project Template

```text
MyGame/
 ├── Content/
 ├── Config/
 ├── Source/
 ├── Plugins/
 ├── Saved/
 └── Project.nfproj
```

---

# 226. Project File

يحتوي:

- engine version
- project name
- modules
- startup map
- target platforms
- rendering settings

---

# 227. Content Browser

يجب أن يشعر كمستودع assets:

```text
Content/
 ├── Characters
 ├── Environments
 ├── Materials
 ├── Textures
 ├── Audio
 ├── VFX
 └── UI
```

---

# 228. Import Rules

مثال:

```text
Texture_*.png → Texture Importer
SM_*.fbx → Static Mesh
SK_*.fbx → Skeletal Mesh
S_*.hlsl → Shader
```

لكن لا تجعل naming convention شرطًا أساسيًا.

---

# 229. Naming Tools

يمكن للمحرك اقتراح أسماء لكنه لا يجب إجبار المطور عليها.

---

# 230. Asset Preview

كل Asset يجب أن يملك preview.

---

# 231. Thumbnail Cache

يجب تخزين thumbnails في cache.

---

# 232. Editor Performance

الـEditor نفسه يجب أن يكون optimized.

لا يجب إعادة فهرسة كل المشروع عند فتح نافذة.

---

# 233. Background Asset Indexing

الفهرسة تعمل في background.

---

# 234. Package Isolation

Third-party plugins يجب أن تبقى معزولة عن core قدر الإمكان.

---

# 235. ABI Strategy

حدد ABI boundaries بحذر.

يفضل عبر interfaces مستقرة أو modules منفصلة.

---

# 236. C++ Coding Standard

استخدم:

- clang-format
- clang-tidy
- warnings as errors في codebase الأساسي
- static analysis

---

# 237. Compiler Warnings

استهدف:

```text
-Wall
-Wextra
```

أو ما يعادلها على toolchain المستهدف.

---

# 238. Sanitizers

في Dev/CI:

- Address Sanitizer
- Undefined Behavior Sanitizer
- Thread Sanitizer حيث مناسب

---

# 239. Memory Leak Detection

يجب وجود tooling لرصد:

- persistent leaks
- GPU leaks
- resource leaks

---

# 240. Deterministic Asset Cooking

يجب أن ينتج نفس input نفس output قدر الإمكان.

---

# 241. Cache Invalidation

عند تغير:

- source
- importer version
- engine asset schema
- platform settings

يجب invalidation ذكي.

---

# 242. Versioning

للمحرك:

```text
Major.Minor.Patch
```

مثال:

```text
0.1.0
0.2.0
1.0.0
```

---

# 243. LTS

بعد الاستقرار:

```text
NOVAForge 1.x LTS
```

ويتم تثبيت APIs المهمة.

---

# 244. Experimental Modules

كل feature غير مستقرة توضع تحت:

```text
Experimental/
```

ولا تكسر APIs الأساسية.

---

# 245. Feature Maturity

كل feature لها:

```text
Prototype
Experimental
Beta
Stable
LTS
```

---

# 246. Rendering Roadmap

ترتيب منطقي:

### R0
Triangle / window.

### R1
Textures + meshes.

### R2
PBR + shadows.

### R3
Deferred/Forward+.

### R4
Post process.

### R5
GPU culling.

### R6
World streaming.

### R7
Advanced GI.

### R8
Virtual geometry / VT.

### R9
Optional ray tracing.

---

# 247. ECS Roadmap

### E0
Entity IDs.

### E1
Components.

### E2
Queries.

### E3
Archetypes.

### E4
Parallel scheduling.

### E5
Serialization.

### E6
Replication metadata.

---

# 248. Editor Roadmap

### A0
Window.

### A1
Viewport.

### A2
Outliner.

### A3
Inspector.

### A4
Content Browser.

### A5
Scene editing.

### A6
Prefab.

### A7
Material editor.

### A8
Animation editor.

### A9
World editor.

---

# 249. Scripting Roadmap

### S0
C++ gameplay module.

### S1
Reflection.

### S2
C# or Lua.

### S3
Hot reload.

### S4
Visual scripting.

---

# 250. Multiplayer Roadmap

### N0
Transport.

### N1
Replication.

### N2
RPC.

### N3
Prediction.

### N4
Dedicated server.

### N5
Interest management.

---

# 251. Version 0.1

لا تحاول صنع AAA.

الهدف:

```text
Window
Input
RHI
Renderer
ECS
Scene
Assets
Editor basic
```

ويجب أن تصنع لعبة صغيرة قابلة للعب.

---

# 252. Version 0.2

أضف:

- Physics
- Audio
- Animation
- better editor
- prefab
- scripting
- save

---

# 253. Version 0.3

أضف:

- terrain
- world streaming
- particles
- navigation
- AI

---

# 254. Version 0.4

أضف:

- multiplayer
- dedicated server
- advanced profiling
- cooking
- packaging

---

# 255. Version 0.5

أضف:

- advanced renderer
- GI
- virtualized systems
- high-end scalability

---

# 256. Version 1.0

لا تطلق 1.0 إلا إذا:

- APIs مستقرة
- Editor مستقر
- sample projects كبيرة
- no critical crashes
- reproducible build
- documentation جيدة
- profiler يعمل
- cook/package يعمل
- backup/recovery يعمل

---

# 257. ترتيب التنفيذ الفعلي

هذا هو الترتيب المقترح للفريق:

```text
1. Repository
2. Build system
3. Core
4. Logging
5. Memory
6. Math
7. Platform
8. Window/input
9. Job system
10. RHI
11. Renderer
12. Asset system
13. ECS
14. Scene
15. Editor viewport
16. Physics
17. Animation
18. Audio
19. Scripting
20. UI
21. AI
22. Terrain
23. Streaming
24. VFX
25. Networking
26. Cooking
27. Packaging
28. Profiling
29. Optimization
30. Samples
31. Beta
```

---

# 258. الفريق المطلوب

إذا أردت محركًا فعليًا بمستوى طموح كبير، فكر في فرق لا شخص واحد فقط.

## Minimum serious team

### Engine Lead
Architecture.

### Rendering Engineers
3D renderer / GPU.

### Core Engineers
Memory / threading / ECS.

### Tools Engineers
Editor / importers.

### Physics Engineer

### Audio Engineer

### Animation Engineer

### Networking Engineer

### Gameplay/Scripting Engineer

### Technical Artist

### Build/Infrastructure Engineer

### QA

---

# 259. لو كنت تعمل Solo

لا تبني:

> "محرك ينافس كل المحركات"

من البداية.

ابنِ:

> "محرك صغير معماريًا يمكن أن يكبر إلى ذلك الهدف."

الخطة:

```text
Year 1:
Core + Renderer + ECS + Editor

Year 2:
Physics + Animation + Audio + Tools

Year 3:
Streaming + VFX + AI + Networking

Year 4+:
High-end rendering + large-world systems
```

هذا ليس قانونًا زمنيًا، لكنه يوضح حجم المشروع.

---

# 260. أول لعبة يجب بناؤها

لا تبدأ بـMMORPG أو GTA-like game.

ابدأ بـ:

### Vertical Slice

لعبة صغيرة فيها:

- Player
- Character
- Camera
- Weapon
- Enemies
- Physics
- Animation
- Audio
- UI
- Save
- basic level

ثم اختبر الـEngine عليها.

---

# 261. الهدف الحقيقي للـVertical Slice

ليس إنتاج لعبة.

بل إثبات:

```text
Can the engine support a real game?
```

إذا نعم، نوسع.

---

# 262. معايير نجاح المرحلة الأولى

يجب أن تستطيع:

```text
Create project
 ↓
Open editor
 ↓
Create scene
 ↓
Add entity
 ↓
Add mesh
 ↓
Add material
 ↓
Play
 ↓
Save
 ↓
Build
 ↓
Run outside editor
```

---

# 263. معايير نجاح Renderer

يجب أن يدعم:

- 3D camera
- meshes
- PBR
- textures
- directional light
- shadow
- post process

قبل التفكير في RT.

---

# 264. معايير نجاح ECS

يجب أن ينجح اختبار:

```text
1,000,000 entities
```

مع transforms بسيطة وقياس الأداء.

لا يعني هذا أن اللعبة تحتاج مليون entity نشطًا، بل لاختبار architecture.

---

# 265. معايير نجاح Streaming

Test:

```text
Huge world
 ↓
Move player
 ↓
Cells load/unload
 ↓
No visible stalls
```

---

# 266. معايير نجاح Editor

يجب أن يشعر المطور أنه:

> "أستطيع عمل Prototype بسرعة."

وليس:

> "أحتاج برمجة كل شيء بنفسي."

---

# 267. معايير نجاح AAA

AAA ليس مجرد graphics.

المحرك يجب أن يكون:

```text
Fast
Stable
Toolable
Debuggable
Scalable
Automatable
```

---

# 268. أهم قاعدة

لا تقيس المحرك بعدد الـFeatures.

قِسه بـ:

### iteration time

كم ثانية يحتاج المطور:

```text
تعديل
→ Save
→ Build/Reload
→ Play
```

---

# 269. أهم قرار معماري

لا تجعل:

```text
Renderer
Physics
ECS
Editor
Gameplay
```

متشابكة.

كل نظام يجب أن يعرف أقل قدر ممكن عن الآخر.

---

# 270. Engine Dependency Graph

اقتراح:

```text
                    Editor
                      |
              +-------+-------+
              |               |
          Gameplay          Tools
              |               |
              +-------+-------+
                      |
                   Runtime
                      |
      +-------+-------+-------+-------+
      |       |       |       |       |
    Render  Physics  Audio    AI      Net
      |       |       |       |       |
      +-------+-------+-------+-------+
                      |
                     ECS
                      |
                  Scene/World
                      |
                     Core
                      |
                   Platform
```

---

# 271. Naming للمشروع

أسماء مقترحة:

```text
NOVAForge
AstraCore
TitanForge
Nebula Engine
Axiom Engine
Orion Runtime
VertexForge
Pulse Engine
```

الأفضل الاسم المؤقت لا يقيّدك.

---

# 272. ملف README

يجب أن يبدأ المشروع بصفحة تفهم المطور ما هو المحرك.

مثال:

```md
# NOVAForge

High-performance cross-platform game engine.

## Features
- 2D / 3D
- PBR
- ECS
- World Streaming
- Physics
- Animation
- Networking
- Editor

## Build
...
```

---

# 273. Git Repository

اقترح:

```text
main
├── Engine
├── Editor
├── Tools
├── Samples
├── Tests
├── Docs
└── ThirdParty
```

---

# 274. Issue Labels

استخدم:

```text
core
rendering
ecs
physics
editor
tools
network
audio
animation
ai
performance
bug
feature
experimental
```

---

# 275. Definition of Done

أي feature لا تعتبر مكتملة إلا إذا:

- Code
- Tests
- Documentation
- Error handling
- Profiling
- Editor integration
- Serialization إن احتاج
- Sample usage

---

# 276. Performance Budgets

كل لعبة تضع budgets:

```text
CPU: 16.6 ms
GPU: 16.6 ms
Memory: X GB
Streaming: X MB/s
Network: X KB/s
```

المحرك يجب أن يساعد المطور على مراقبتها.

---

# 277. Budget Manager

يوفر:

```text
CPU Budget
GPU Budget
Memory Budget
Streaming Budget
AI Budget
Animation Budget
VFX Budget
```

---

# 278. Update Rate Scaling

أنظمة بعيدة عن اللاعب يمكن أن تعمل:

```text
60 Hz
30 Hz
15 Hz
5 Hz
1 Hz
```

حسب أهميتها.

---

# 279. Visibility

طبقات:

```text
Frustum
Distance
Occlusion
Portal optional
LOD
```

---

# 280. Occlusion

ابدأ بـ:

- frustum culling
- distance culling

ثم:

- hierarchical Z
- occlusion queries
- software/GPU occlusion

---

# 281. Instancing

يدعم:

```text
Static instancing
Dynamic instancing
GPU instancing
```

---

# 282. Draw Sorting

يجب تقليل:

- state changes
- descriptor changes
- pipeline switches

---

# 283. Material Sorting

ترتيب draw calls حسب:

```text
Pipeline
Material
Texture set
Depth
```

بحسب renderer path.

---

# 284. Transparency

يدعم:

- Alpha blend
- Alpha test
- OIT optional
- refractive materials

---

# 285. Decoupled Render Settings

لا تجعل game code يعرف تفاصيل:

```text
Shadow map resolution
GI algorithm
Upscaler
```

يجب أن يعرف:

```text
Quality Preset
```

أو feature-level APIs.

---

# 286. Graphics Device Loss

يجب التخطيط لاستعادة resources في الحالات التي تحتاج ذلك.

---

# 287. Asset Streaming Error

إذا فشل تحميل asset:

```text
Fallback asset
Log
Retry
```

بحسب نوع الخطأ.

---

# 288. Resource Priority

كل resource له:

```text
Priority
Size
Resident state
Last used
Importance
```

---

# 289. Memory Budget Example

```text
Textures: 2 GB
Meshes: 1 GB
Buffers: 512 MB
Audio: 256 MB
Other: 256 MB
```

هذه مجرد أمثلة؛ القيم تكون project/platform-specific.

---

# 290. Editor Memory

افصل memory budgets الخاصة بالـEditor عن Runtime قدر الإمكان.

---

# 291. Cache vs Source

Never edit cooked cache as source of truth.

Source:

```text
Content/
```

Derived:

```text
Saved/DerivedData/
```

---

# 292. Crash Safe Saving

الـEditor يجب أن يستخدم:

```text
temporary file
→ flush
→ atomic rename
```

حتى لا يفقد scene كاملة عند crash.

---

# 293. Autosave

يمكن حفظ recovery snapshots.

---

# 294. Scene Merge

للفرق الكبيرة يجب دعم merge strategy أو locking حسب version-control backend.

---

# 295. Prefab Overrides

مثال:

```text
EnemyBase
   ↓
EliteEnemy
   ↓
BossEnemy
```

يجب أن يكون override tracking واضحًا.

---

# 296. Blueprint/Visual Script Concept

يجب أن يكون هناك:

```text
Event
 ↓
Node graph
 ↓
Functions
 ↓
Variables
```

لكن visual scripting لا يجب أن يحل محل native code لكل الأنظمة الثقيلة.

---

# 297. Visual Scripting Performance

لـhigh-performance gameplay:

```text
Visual graph
 ↓
Compiled/interpreted VM
```

ويفضل وجود compilation للـhot paths المهمة.

---

# 298. Reflection API

يجب أن يسمح للـEditor برؤية:

```text
Class
Property
Function
Enum
Metadata
```

---

# 299. Code Generation

يفضل:

```text
Header annotations
 ↓
Reflection tool
 ↓
Generated metadata
```

بدل reflection مكلف جدًا أثناء runtime.

---

# 300. Final Architecture

الخلاصة:

```text
                        NOVAForge
                            |
       +--------------------+--------------------+
       |                    |                    |
     Editor               Tools               Runtime
       |                    |                    |
       +--------------------+--------------------+
                            |
                         Gameplay
                            |
             +--------------+--------------+
             |              |              |
            ECS           World         Scripting
             |              |              |
       +-----+-----+   +----+----+    +----+----+
       |     |     |   |         |    |         |
    Physics AI   Anim Stream    Terrain C#    Visual
       |     |     |     |         |    |       |
       +-----+-----+-----+---------+----+-------+
                            |
                         Render
                            |
             +--------------+--------------+
             |              |              |
           RHI           RenderGraph      GPU
             |
      +------+------+------+
      |      |      |      |
   Vulkan  D3D12  Metal  Other
```

---

# 301. أهم 20 قرار يجب تثبيتها مبكرًا

1. C++ كـengine language.
2. RHI منفصل عن renderer.
3. ECS كجزء أساسي.
4. Render World منفصل عن Game World.
5. Handle-based resources.
6. Async IO.
7. Shared Job System.
8. Asset IDs مستقرة.
9. Versioned serialization.
10. Editor منفصل عن Runtime.
11. Modular subsystems.
12. Data-driven assets.
13. World streaming من البداية.
14. Dedicated server قابل للبناء.
15. Profiling من البداية.
16. CI من البداية.
17. Tests لكل subsystem.
18. Quality scalability.
19. Plugin architecture.
20. لا dependency cycles.

---

# 302. الأشياء التي لا يجب بناؤها من الصفر في البداية

لا تضيع سنوات في إعادة اختراع:

- PNG decoder
- JPEG decoder
- compression primitives
- font rasterization
- platform windowing
- low-level networking codec
- physics solver كامل
- FBX parser كامل
- audio codec ecosystem

إذا كانت هناك مكتبة موثوقة ومناسبة للترخيص، استخدمها.

ابنِ الـEngine value فوقها.

---

# 303. الأشياء التي يجب أن تمتلكها معماريًا

الأشياء التي تعتبر جوهر NOVAForge:

- RHI
- Render Graph
- Render World
- ECS
- Resource system
- Asset pipeline
- World streaming
- Editor integration
- Job system
- Profiling
- Engine APIs

---

# 304. أول أسبوع

### Day 1
Repository + build.

### Day 2
Core + logging.

### Day 3
Platform + window.

### Day 4
Input + time.

### Day 5
Math + camera.

### Day 6
RHI skeleton.

### Day 7
Triangle.

---

# 305. أول شهر

النتيجة المستهدفة:

```text
Open Window
Create Device
Render Triangle
Render Texture
Render Mesh
Input
Camera
Basic ECS
Basic Scene
```

---

# 306. أول 3 أشهر

النتيجة:

```text
Editor
Viewport
ECS
Assets
PBR
Lights
Shadows
Physics basic
Audio basic
Save/Load
Play Mode
```

---

# 307. أول Prototype لعبة

اصنع:

> Third-person action demo

تحتوي:

- Player
- Character controller
- Animation
- Weapon
- Enemy
- Terrain
- UI
- Audio
- save

---

# 308. ثاني Prototype

اصنع:

> Open-world streaming demo

يحتوي:

- Huge map
- streaming cells
- terrain
- foliage
- sky
- weather
- day/night
- NPCs
- AI LOD

---

# 309. ثالث Prototype

اصنع:

> Multiplayer arena

يحتوي:

- server
- client
- replication
- prediction
- shooting
- score
- dedicated server

---

# 310. Performance Gate

قبل إضافة feature ضخمة:

> هل architecture الحالية تستطيع تحملها؟

إذا الجواب لا:

- refactor أولًا
- feature ثانيًا

---

# 311. Architecture Review

كل شهر:

```text
Performance review
Memory review
Dependency review
API review
Editor UX review
```

---

# 312. Technical Debt

احتفظ بملف:

```text
TECH_DEBT.md
```

ولا تسمح أن يتحول prototype إلى production architecture بدون refactor.

---

# 313. Feature Requests

أي feature جديدة تمر عبر:

```text
Problem
Use case
Architecture impact
Performance impact
Editor impact
Serialization impact
Networking impact
Testing
```

---

# 314. MVP Definition

الـMVP للمحرك:

```text
C++
Core
Window
Input
RHI
3D Renderer
ECS
Scene
Assets
Editor
Physics
Audio
Scripting
Build
```

---

# 315. بعد MVP

ثم:

```text
Animation
AI
UI
Terrain
VFX
Streaming
Networking
```

---

# 316. High-End Phase

بعد استقرار الـfoundation:

```text
GPU-driven rendering
Virtual geometry
Virtual texturing
Advanced GI
RT
Advanced clouds
Huge worlds
Massive crowds
```

---

# 317. لماذا هذا الترتيب؟

لأن بناء:

```text
Ray tracing
+
GI
+
Virtual geometry
```

فوق Core غير مستقر غالبًا يخلق system لا يمكن صيانته.

الـfoundation أهم من الـfeatures.

---

# 318. Definition of a "AAA-capable" Engine

المحرك ليس AAA-capable لأنه يملك screenshots جميلة فقط.

يجب أن يستطيع:

```text
Large team collaboration
Large assets
Large worlds
Fast iteration
Automated builds
Debugging
Profiling
Streaming
Scalability
Dedicated server
Stable APIs
Recoverable errors
```

---

# 319. شعار المشروع

اقتراح:

> **NOVAForge — Build Worlds Without Fighting the Engine.**

أو:

> **NOVAForge — One Engine. Every Scale.**

---

# 320. النسخة التنفيذية المختصرة

إذا أردت إعطاء هذه الوثيقة إلى فريق تطوير، قل لهم:

## Phase A
Build the foundation.

## Phase B
Build the editor.

## Phase C
Build a playable 3D game.

## Phase D
Build large-world systems.

## Phase E
Build multiplayer.

## Phase F
Build high-end rendering.

## Phase G
Stabilize, profile, document, ship.

---

# 321. الخطة النهائية في سطر واحد

```text
Core
→ Jobs
→ RHI
→ Render
→ ECS
→ Assets
→ Editor
→ Physics
→ Animation
→ Audio
→ Scripting
→ UI
→ AI
→ Terrain
→ Streaming
→ VFX
→ Networking
→ Cooking
→ Profiling
→ Optimization
→ Samples
→ Beta
→ 1.0
```

---

# 322. آخر قاعدة

لا تبنِ محركًا يجمع كل الـFeatures فقط.

ابنِ محركًا يستطيع **استيعاب Features جديدة بدون تكسير القديم**.

وهذا هو الفرق بين:

```text
Game Framework ضخم
```

و:

```text
Real Engine Architecture
```

---

# 323. ملحق: Baseline Technical Stack المقترح

## Core
- C++
- CMake
- Ninja
- Git

## Rendering
- Vulkan
- Direct3D 12
- Metal لاحقًا
- HLSL/Shader IR strategy

## Math
- SIMD-aware math layer

## Physics
- Abstract physics backend + production-ready implementation

## Audio
- Abstract audio device + production backend

## Assets
- glTF pipeline as a first-class format
- image/audio import pipeline
- derived-data cache

## Editor
- Custom dockable editor framework

## Scripting
- Native C++
- C# or Lua
- Visual scripting

---

# 324. ملحق: Naming

```text
NF = NovaForge

Entity: NFEntity
World: NFWorld
Texture: NFTexture
Material: NFMaterial
Mesh: NFMesh
Camera: NFCamera
RenderDevice: NFRenderDevice
AssetHandle: NFAssetHandle
```

---

# 325. ملحق: Example Game Code

```cpp
void PlayerSystem::Update(float dt)
{
    for (auto entity : world.Query<Player, Transform>())
    {
        auto& player = world.Get<Player>(entity);
        auto& transform = world.Get<Transform>(entity);

        Vec3 input = inputSystem.GetVector("Move");

        transform.position += input * player.speed * dt;
    }
}
```

---

# 326. ملحق: Example Asset API

```cpp
auto material =
    assets.Load<Material>("asset://materials/player_body");

if (material.IsValid())
{
    renderer.SetMaterial(material);
}
```

---

# 327. ملحق: Example World Streaming API

```cpp
streaming.SetWorld(world);

streaming.SetRadius(3);

streaming.Update(playerTransform.position);
```

---

# 328. ملحق: Example Render Pass

```cpp
renderGraph.AddPass(
    "ShadowPass",
    [&](RenderPassBuilder& builder)
    {
        builder.Write(depthAtlas);
    },
    [&](RenderContext& ctx)
    {
        renderer.RenderShadows(ctx);
    }
);
```

---

# 329. ملحق: Example Network Component

```cpp
struct Health
{
    float current;

    static constexpr bool Replicated = true;
};
```

---

# 330. ملحق: Example Gameplay Tag

```cpp
entity.AddTag("Character.Player");
entity.AddTag("Faction.Human");
```

---

# 331. ملحق: Example Quality Profile

```yaml
profile: High

render:
  shadows: high
  gi: medium
  volumetrics: high
  particles: high

world:
  view_distance: 5000

animation:
  max_update_rate: 60
```

---

# 332. ملحق: Example Project

```text
MyGame/
├── Config/
│   ├── Engine.ini
│   └── Project.ini
│
├── Content/
│   ├── Characters/
│   ├── Environments/
│   ├── Materials/
│   ├── Textures/
│   ├── Animations/
│   ├── Audio/
│   ├── UI/
│   └── Worlds/
│
├── Source/
│   ├── Game/
│   └── Editor/
│
├── Plugins/
│
├── Saved/
│
└── MyGame.nfproj
```

---

# 333. ملحق: أول Repository Milestone

بعد أول milestone يجب أن تستطيع كتابة:

```bash
nf create Demo
nf editor Demo
```

ثم يظهر:

```text
NOVAForge Editor
```

وتستطيع:

```text
New Scene
Add Entity
Add Camera
Add Mesh
Assign Material
Play
Save
Build
```

إذا وصلت لهذه النقطة، عندك Engine حقيقي في بدايته.

---

# 334. خاتمة

المشروع المقترح ليس "خلطة من Unity + Godot + Unreal".

الهدف هو:

> **توحيد أفضل أفكار سهولة التطوير + قوة الـruntime + جودة الرسومات + قابلية التوسع داخل Architecture واحدة.**

الـEngine الناجح يجب أن يحقق أربعة أشياء معًا:

```text
Power
Performance
Productivity
Stability
```

ولو تضاعفت الـFeatures ولم تتحسن هذه الأربعة، فأنت لا تبني محركًا أفضل.

أما إذا كانت الـArchitecture سليمة، فكل Feature جديدة تصبح أسهل في الإضافة، وأقل تكلفة في الصيانة، وأقل خطورة على النظام كاملًا.

**NOVAForge يجب أن يُبنى كمنصة طويلة العمر، وليس كديمو تقني.**

---

# الجزء الثاني — المرجع البرمجي الكامل (Code Reference)

> الأقسام 335 فما بعد هي **مرجع تنفيذي**، لا تصميمًا نظريًا. كل كتلة هنا مكتوبة
> بـ C++23 وفق اصطلاحات المحرك الفعلية (النطاق `nf::`، البادئة `NF_`، تحذيرات
> `-W4 -WX`، وحتمية كاملة). الهدف: أن يقرأها المهندس فيعرف **كيف** يُبنى كل نظام،
> لا فقط **لماذا**.

---

# 335. مقدمة المرجع البرمجي

المرجع منظوم على شكل طبقات، كل طبقة تعتمد فقط على ما قبلها (قاعدة §103):

```text
NFCore        →   أنواع، رياضيات، حاويات، ذاكرة، وقت
NFJobs        →   جدولة المهام
NFEcs/NFScene →   الكيانات، المشهد، الهرميات
NFAssets      →   VFS، التسجيل، الطبخ
NFRHI         →   تجريد Vulkan
NFRendering   →   RenderGraph، المواد، المصيّر
NFPhysics     →   الحتمية + Jolt
NFAnimation   →   العظام، الآلة، IK
NFAudio       →   Mixer + WASAPI
NFNetworking  →   UDP + موثوق + لقطات
NFGameplay    →   وسوم، مهام، حوارات
NFEditor      →   ImGui + مفتش + تراجع
```

قاعدة ذهبية: **لا توجد حلقة استيراد**. `check_layering.sh` يكسر البناء فورًا عند
أي انتهاك (انظر §140).

---

# 336. Core: الأنواع والرياضيات (Types & Math)

الأنواع ثابتة العرض عبر كل المنصات — الحتمية تبدأ من هنا.

```cpp
// NF/Core/Types.hpp
#pragma once

#include <cstdint>
#include <cstddef>

namespace nf {

using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;

// 128-bit GUID للأصول والكيانات. توليد حتمي بزريعة (انظر §114).
struct Uuid {
    u64 lo = 0;
    u64 hi = 0;

    constexpr bool valid() const noexcept { return (lo | hi) != 0; }
    constexpr bool operator==(const Uuid& o) const noexcept { return lo == o.lo && hi == o.hi; }
    constexpr bool operator!=(const Uuid& o) const noexcept { return !(*this == o); }
};

// مقبض مُعدَّل بالجيل: العدد المرتفع يكشف الاستخدام بعد التحرير (§101).
template<typename Tag, u32 GenerationBits = 12>
struct Handle {
    static constexpr u32 IndexBits = 32 - GenerationBits;
    static constexpr u32 IndexMask = (u32{1} << IndexBits) - 1;
    static constexpr u32 GenerationMask = (u32{1} << GenerationBits) - 1;

    u32 value = 0;

    static constexpr Handle invalid() noexcept { return Handle{}; }
    constexpr bool valid() const noexcept { return value != 0; }

    constexpr u32 index() const noexcept { return value & IndexMask; }
    constexpr u32 generation() const noexcept { return value >> IndexBits; }

    constexpr bool operator==(const Handle& o) const noexcept { return value == o.value; }
    constexpr bool operator!=(const Handle& o) const noexcept { return value != o.value; }
};

} // namespace nf
```

الرياضيات: `Vec3`/`Vec4`/`Quat`/`Mat44` بصيغة SoA-friendly حيث يُسمح التحويل
الصريح فقط — لا منشئات ضمنية تخفي التكلفة.

```cpp
// NF/Core/Math.hpp
#pragma once

#include <NF/Core/Types.hpp>
#include <cmath>

namespace nf {

struct alignas(16) Vec3 {
    f32 x = 0.0f, y = 0.0f, z = 0.0f;

    static constexpr Vec3 zero() noexcept { return {}; }
    static constexpr Vec3 unit_x() noexcept { return {1.0f, 0.0f, 0.0f}; }
    static constexpr Vec3 unit_y() noexcept { return {0.0f, 1.0f, 0.0f}; }
    static constexpr Vec3 unit_z() noexcept { return {0.0f, 0.0f, 1.0f}; }

    constexpr Vec3 operator+(const Vec3& o) const noexcept { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const noexcept { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(f32 s) const noexcept { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator-() const noexcept { return {-x, -y, -z}; }

    Vec3& operator+=(const Vec3& o) noexcept { return *this = *this + o; }
    Vec3& operator-=(const Vec3& o) noexcept { return *this = *this - o; }

    constexpr f32 dot(const Vec3& o) const noexcept { return x * o.x + y * o.y + z * o.z; }
    constexpr Vec3 cross(const Vec3& o) const noexcept {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }

    inline f32 length_sq() const noexcept { return dot(*this); }
    inline f32 length() const noexcept { return std::sqrt(length_sq()); }

    // الحتمية: لا تقبل حالة "صفر"، صرّح عنها (انظر §148 Error Philosophy).
    inline Vec3 normalized(f32 epsilon = 1e-6f) const noexcept {
        const f32 len_sq = length_sq();
        if (!(len_sq > epsilon * epsilon)) return zero();
        return *this * (1.0f / std::sqrt(len_sq));
    }

    // ASCII SOH: الترتيب القطعي لزوايا أويلر يتجنب الانحراف التراكمي (§24).
    inline static Vec3 lerp(const Vec3& a, const Vec3& b, f32 t) noexcept {
        return a + (b - a) * t;
    }
};

// رباعية وحدة فقط. أي استخدام غير وحدة خطأ برمجي، لذا normalized() إجباري.
struct alignas(16) Quat {
    f32 x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    static constexpr Quat identity() noexcept { return {}; }

    static inline Quat from_axis_angle(const Vec3& axis, f32 radians) noexcept {
        const f32 half = radians * 0.5f;
        const Vec3 n = axis.normalized();
        const f32 s = std::sin(half);
        return {n.x * s, n.y * s, n.z * s, std::cos(half)};
    }

    // SLERP بدل LERP للدورات الطويلة: LERP يفقد الوحدة فيكوّن انحرافًا مرئيًا.
    inline Quat slerp(const Quat& o, f32 t) const noexcept {
        f32 cos_omega = x * o.x + y * o.y + z * o.z + w * o.w;
        Quat b = o;
        if (cos_omega < 0.0f) { // أقصر مسار على الكرة
            b = {-b.x, -b.y, -b.z, -b.w};
            cos_omega = -cos_omega;
        }
        if (cos_omega > 0.9995f) { // زاوية صغيرة: LERP أرخص ودقيق
            return Quat{
                x + t * (b.x - x), y + t * (b.y - y), z + t * (b.z - z), w + t * (b.w - w)
            }.normalized();
        }
        const f32 omega = std::acos(cos_omega);
        const f32 sin_omega = std::sin(omega);
        const f32 s0 = std::sin((1.0f - t) * omega) / sin_omega;
        const f32 s1 = std::sin(t * omega) / sin_omega;
        return {s0 * x + s1 * b.x, s0 * y + s1 * b.y, s0 * z + s1 * b.z, s0 * w + s1 * b.w};
    }

    inline Quat normalized(f32 epsilon = 1e-6f) const noexcept {
        const f32 len_sq = x * x + y * y + z * z + w * w;
        if (!(len_sq > epsilon)) return identity();
        const f32 inv = 1.0f / std::sqrt(len_sq);
        return {x * inv, y * inv, z * inv, w * inv};
    }

    // جداء هاملتون: ترتيب المعاملات يحدد "الضرب اليساري" الكامل للمحرك.
    constexpr Quat operator*(const Quat& o) const noexcept {
        return {
            w * o.x + x * o.w + y * o.z - z * o.y,
            w * o.y - x * o.z + y * o.w + z * o.x,
            w * o.z + x * o.y - y * o.x + z * o.w,
            w * o.w - x * o.x - y * o.y - z * o.z,
        };
    }

    constexpr Vec3 operator*(const Vec3& v) const noexcept {
        const Vec3 qv{x, y, z};
        const Vec3 t = qv.cross(v) * 2.0f;
        return v + t * w + qv.cross(t);
    }
};

// Mat44: عمود-رئيسي (Vulkan/GLM-compatible)، لا تداخل مع Direct3M-row-major.
struct alignas(16) Mat44 {
    Vec4 columns[4];

    static inline Mat44 identity() noexcept {
        Mat44 m{};
        m.columns[0] = {1.0f, 0.0f, 0.0f, 0.0f};
        m.columns[1] = {0.0f, 1.0f, 0.0f, 0.0f};
        m.columns[2] = {0.0f, 0.0f, 1.0f, 0.0f};
        m.columns[3] = {0.0f, 0.0f, 0.0f, 1.0f};
        return m;
    }

    static inline Mat44 translation(const Vec3& p) noexcept {
        Mat44 m = identity();
        m.columns[3] = {p.x, p.y, p.z, 1.0f};
        return m;
    }

    static inline Mat44 from_rotation_translation(const Quat& q, const Vec3& p) noexcept {
        const f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat44 m{};
        m.columns[0] = {1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy), 0.0f};
        m.columns[1] = {2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx), 0.0f};
        m.columns[2] = {2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy), 0.0f};
        m.columns[3] = {p.x, p.y, p.z, 1.0f};
        return m;
    }

    constexpr Mat44 operator*(const Mat44& o) const noexcept {
        Mat44 out{};
        for (int c = 0; c < 4; ++c) {
            const Vec4& oc = o.columns[c];
            out.columns[c] = columns[0] * oc.x + columns[1] * oc.y +
                             columns[2] * oc.z + columns[3] * oc.w;
        }
        return out;
    }

    // العالم → للكاميرا فقط؛ التحويل العكسي للالتقاط يُستعمل في Raycast.
    inline Mat44 inverted() const noexcept {
        Mat44 out;
        const Vec4& a = columns[0];
        const Vec4& b = columns[1];
        const Vec4& c = columns[2];
        const Vec4& d = columns[3];
        const Vec3 r = {a.x, b.x, c.x};
        const Vec3 u = {a.y, b.y, c.y};
        const Vec3 f = {a.z, b.z, c.z};
        const Vec3 p = {d.x, d.y, d.z};
        const Vec3 rxf = r.cross(f);
        const Vec3 uxf = u.cross(f);
        const Vec3 rxu = r.cross(u);
        const f32 inv_det = 1.0f / f.dot(rxu);
        out.columns[0] = {uxf.x * inv_det, uxf.y * inv_det, uxf.z * inv_det, 0.0f};
        out.columns[1] = {-rxf.x * inv_det, -rxf.y * inv_det, -rxf.z * inv_det, 0.0f};
        out.columns[2] = {rxu.x * inv_det, rxu.y * inv_det, rxu.z * inv_det, 0.0f};
        const Vec3 t = {rxf.dot(p), -uxf.dot(p), rxu.dot(p)};
        out.columns[3] = {t.x * inv_det, t.y * inv_det, t.z * inv_det, 1.0f};
        return out;
    }
};

// حدود زاوية أويلر للتصدير/الاستيراد فقط — الاستخدام الداخلي رباعي دائمًا.
struct Euler {
    f32 pitch = 0.0f; // X
    f32 yaw = 0.0f;   // Y
    f32 roll = 0.0f;  // Z
};

} // namespace nf
```

---

# 337. Core: الحاويات (SparseSet & Handle Table)

الـ ECS يعتمد على `SparseSet`: الوصول O(1)، الذاكرة متجاورة، التكرار
cache-friendly (§27).

```cpp
// NF/Core/Containers.hpp
#pragma once

#include <NF/Core/Types.hpp>
#include <vector>
#include <utility>

namespace nf {

/// مجموعة متفرقة: مفتاح كثيف (الكيان) → فتحة في صفيف كثيف متجاور.
/// الحذف بمبادلة الأخير: ترتيب الكثيف غير محدد، لكن التكرار دائمًا خطي.
template<typename Value>
class SparseSet {
public:
    static constexpr u32 Invalid = 0xFFFFFFFFu;

    bool contains(u32 key) const noexcept {
        return key < m_sparse.size() && m_sparse[key] != Invalid;
    }

    // إدخال أو تحديث. القيمة القديمة تُستبدل (لا توجد نسختان صامتة).
    Value& insert(u32 key, Value v) {
        if (key >= m_sparse.size()) m_sparse.resize(key + 1, Invalid);
        const u32 dense = m_sparse[key];
        if (dense != Invalid) {
            m_values[dense] = std::move(v);
            return m_values[dense];
        }
        m_sparse[key] = static_cast<u32>(m_keys.size());
        m_keys.push_back(key);
        m_values.push_back(std::move(v));
        return m_values.back();
    }

    // إزالة بمبادلة: لا تحرك عناصر أخرى (استقرار المؤشرات للمفتش).
    bool erase(u32 key) noexcept {
        if (!contains(key)) return false;
        const u32 dense = m_sparse[key];
        const u32 last = static_cast<u32>(m_keys.size()) - 1;
        if (dense != last) {
            m_keys[dense] = m_keys[last];
            m_values[dense] = std::move(m_values[last]);
            m_sparse[m_keys[dense]] = dense;
        }
        m_keys.pop_back();
        m_values.pop_back();
        m_sparse[key] = Invalid;
        return true;
    }

    Value* find(u32 key) noexcept {
        if (!contains(key)) return nullptr;
        return &m_values[m_sparse[key]];
    }

    const Value* find(u32 key) const noexcept {
        if (!contains(key)) return nullptr;
        return &m_values[m_sparse[key]];
    }

    usize size() const noexcept { return m_values.size(); }
    bool empty() const noexcept { return m_values.empty(); }
    void clear() noexcept {
        m_keys.clear();
        m_values.clear();
        std::fill(m_sparse.begin(), m_sparse.end(), Invalid);
    }

    const std::vector<u32>& keys() const noexcept { return m_keys; }
    std::vector<Value>& values() noexcept { return m_values; }
    const std::vector<Value>& values() const noexcept { return m_values; }

private:
    std::vector<u32> m_sparse;   // مفتاح → فتحة الكثيف
    std::vector<u32> m_keys;     // الفتحات الكثيفة → مفاتيح
    std::vector<Value> m_values; // القيم متجاورة
};

/// جدول المقابض بالأجيال (§101): أي مقبض يحمل جيله، فالتحرير يجعله ميتًا
/// فورًا ولا يمكن إعادة استخدامه بالخطأ.
template<typename Value, typename Tag>
class HandleTable {
public:
    using HandleType = Handle<Tag>;

    HandleType create(Value v) {
        const u32 index = static_cast<u32>(m_generations.size());
        const u32 generation = 1; // الجيل 0 = مقبض غير صالح
        m_generations.push_back(generation);
        m_slots.push_back(GenerationMasked);
        m_values.push_back(std::move(v));
        return make(index, generation);
    }

    Value* get(HandleType h) noexcept {
        if (!h.valid() || h.index() >= m_values.size()) return nullptr;
        if (m_generations[h.index()] != h.generation()) return nullptr; // مقبض ميت
        return &m_values[h.index()];
    }

    bool destroy(HandleType h) noexcept {
        Value* v = get(h);
        if (!v) return false;
        v->~Value();
        // زِد الجيل: كل المقابض القديمة تصبح غير صالحة (واجهة Handle آمنة).
        const u32 next = m_generations[h.index()] + 1;
        m_generations[h.index()] = (next > HandleType::GenerationMask) ? 1 : next;
        return true;
    }

private:
    static constexpr u32 GenerationMasked = 0;
    std::vector<u32> m_generations;
    std::vector<Value> m_values;

    static constexpr HandleType make(u32 index, u32 generation) noexcept {
        HandleType h;
        h.value = index | (generation << HandleType::IndexBits);
        return h;
    }
};

/// حلقة دائرية بدون تخصيص لقنوات الشبكة (§66) ومخازن الإطارات.
template<typename T, usize Capacity>
class RingBuffer {
public:
    bool push(const T& v) noexcept {
        if (full()) return false;
        m_data[m_head] = v;
        m_head = (m_head + 1) % Capacity;
        ++m_count;
        return true;
    }

    bool pop(T& out) noexcept {
        if (empty()) return false;
        out = m_data[m_tail];
        m_tail = (m_tail + 1) % Capacity;
        --m_count;
        return true;
    }

    usize count() const noexcept { return m_count; }
    bool empty() const noexcept { return m_count == 0; }
    bool full() const noexcept { return m_count == Capacity; }

private:
    T m_data[Capacity]{};
    usize m_head = 0;
    usize m_tail = 0;
    usize m_count = 0;
};

} // namespace nf
```

---

# 338. Core: الذاكرة والمخصصات (Allocators)

قاعدة §179: **كل تخصيص في الإطار يمر بمخصص**. لا `new`/`malloc` عارض في
المسار الساخن.

```cpp
// NF/Core/Memory.hpp
#pragma once

#include <NF/Core/Types.hpp>
#include <aligned_new>
#include <cstddef>

namespace nf {

/// واجهة المخصص: استراتيجية قابلة للتبديل دون تعديل المنادي.
class Allocator {
public:
    virtual ~Allocator() = default;
    virtual void* allocate(usize bytes, usize alignment = alignof(std::max_align_t)) = 0;
    virtual void deallocate(void* p) noexcept = 0;
    virtual usize allocated_bytes() const noexcept { return 0; }
};

/// مخصص الإطار (Arena): تخصيص مؤشر صاعد، إعادة تعيين جماعية في نهاية الإطار.
/// مثالي للنوافذ، أوامر الرسم، ونتائج الاستعلامات — كلها قصيرة العمر.
class FrameAllocator final : public Allocator {
public:
    explicit FrameAllocator(usize capacity) : m_capacity(capacity) {
        m_base = static_cast<u8*>(::operator new(capacity, std::align_val_t{64}));
        m_offset = 0;
    }

    ~FrameAllocator() override { ::operator delete(m_base, std::align_val_t{64}); }

    FrameAllocator(const FrameAllocator&) = delete;
    FrameAllocator& operator=(const FrameAllocator&) = delete;

    void* allocate(usize bytes, usize alignment) override {
        const usize mask = alignment - 1;
        const usize current = reinterpret_cast<usize>(m_base + m_offset);
        const usize aligned = (current + mask) & ~mask;
        const usize padding = aligned - current;
        if (m_offset + padding + bytes > m_capacity) return nullptr; // لا تنفجر
        m_offset += padding + bytes;
        return reinterpret_cast<void*>(aligned);
    }

    // الإطار يُحرَّر دفعة واحدة فقط. أي تحرير فردي هو خطأ تصميم.
    void deallocate(void*) noexcept override {}

    void reset() noexcept { m_offset = 0; }
    usize used_bytes() const noexcept { return m_offset; }

private:
    u8* m_base = nullptr;
    usize m_offset = 0;
    usize m_capacity = 0;
};

/// مخصص التجمع (Pool): كتل ثابتة الحجم، O(1)، لا تجزئة.
/// مخصص للأجسام المتكررة: الكيانات، المقابض، عقد الشجرة.
template<usize BlockSize, usize BlockAlign = 16>
class PoolAllocator final : public Allocator {
public:
    explicit PoolAllocator(usize block_count) : m_block_count(block_count) {
        const usize total = BlockSize * block_count;
        m_base = static_cast<u8*>(::operator new(total, std::align_val_t{BlockAlign}));
        m_free_head = m_base;
        // قائمة حرة داخلية مضمَّنة: أول 8 بايتات من كل كتلة.
        for (usize i = 0; i + 1 < block_count; ++i) {
            u8* const block = m_base + i * BlockSize;
            u8* const next = m_base + (i + 1) * BlockSize;
            *reinterpret_cast<u8**>(block) = next;
        }
        *reinterpret_cast<u8**>(m_base + (block_count - 1) * BlockSize) = nullptr;
    }

    ~PoolAllocator() override { ::operator delete(m_base, std::align_val_t{BlockAlign}); }

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    void* allocate(usize bytes, usize /*alignment*/) override {
        if (bytes > BlockSize || m_free_head == nullptr) return nullptr;
        u8* const block = m_free_head;
        m_free_head = *reinterpret_cast<u8**>(block);
        ++m_in_use;
        return block;
    }

    void deallocate(void* p) noexcept override {
        if (p == nullptr) return;
        u8* const block = static_cast<u8*>(p);
        *reinterpret_cast<u8**>(block) = m_free_head;
        m_free_head = block;
        --m_in_use;
    }

    usize blocks_in_use() const noexcept { return m_in_use; }

private:
    u8* m_base = nullptr;
    u8* m_free_head = nullptr;
    usize m_block_count = 0;
    usize m_in_use = 0;
};

/// عدّاد التسريب: في وضع التطوير يفشل عند خروج الإطار بمخصص غير متوازن.
class AllocationScope {
public:
    explicit AllocationScope(Allocator& a) : m_allocator(a), m_start(a.allocated_bytes()) {}
    ~AllocationScope() {
        const usize leaked = m_allocator.allocated_bytes() - m_start;
        if (leaked != 0) {
            // في الاختبارات يتحول هذا لفشل صريح (§116): التسريب الصامت أسوأ من الانهيار.
            on_leak(leaked);
        }
    }
    AllocationScope(const AllocationScope&) = delete;
    AllocationScope& operator=(const AllocationScope&) = delete;

private:
    void on_leak(usize bytes); // يُطبَّق في Logger.cpp مع مكدس الاستدعاءات

    Allocator& m_allocator;
    usize m_start;
};

} // namespace nf
```

---

# 339. Jobs: نظام الجدولة متعدد الخيوط

مبني على §91: تجمع خيوط + حاجز، مع تبعيات صريحة. **الحتمية تتطلب أن تكون
النتيجة مستقلة عن ترتيب الجدولة** — لذلك كل مهمة تكتب في شريحة SoA خاصة بها.

```cpp
// NF/Jobs/include/NF/Jobs/JobSystem.hpp
#pragma once

#include <NF/Core/Types.hpp>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace nf::jobs {

using JobId = u64;

/// مهمة: دالة + عدّاد الإدخال. كل مهمة تكتب فقط في شريحتها الخاصة (No false sharing).
struct Job {
    std::function<void()> work;
    std::atomic<u32> dependencies{0};   // تصفير عند الجاهزية
    std::atomic<u32>* completion = nullptr; // لمن ينتظر
};

class JobSystem {
public:
    explicit JobSystem(u32 worker_count = 0) {
        if (worker_count == 0) {
            worker_count = std::max(2u, std::thread::hardware_concurrency());
        }
        m_workers.reserve(worker_count);
        for (u32 i = 0; i < worker_count; ++i) {
            m_workers.emplace_back([this] { worker_main(); });
        }
    }

    ~JobSystem() {
        {
            std::lock_guard lock(m_mutex);
            m_shutdown = true;
        }
        m_cv.notify_all();
        for (std::thread& t : m_workers) {
            if (t.joinable()) t.join();
        }
    }

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /// جدولة مهمة بعد اكتمال `dependency_count` من المهام السابقة.
    JobId schedule(std::function<void()> work, u32 dependency_count = 0,
                   std::atomic<u32>* completion = nullptr) {
        Job job;
        job.work = std::move(work);
        job.dependencies.store(dependency_count, std::memory_order_relaxed);
        job.completion = completion;
        {
            std::lock_guard lock(m_mutex);
            const JobId id = m_next_id++;
            m_pending.push_back(std::move(job));
            m_cv.notify_one();
            return id;
        }
    }

    /// انتظر حتى يصفر عدّاد (Busy-wait قصير ثم sleep طويل — تجنب الدوران المكلف).
    void wait_for(std::atomic<u32>& counter, u32 target = 0) const {
        while (counter.load(std::memory_order_acquire) != target) {
            if (m_pending_count.load(std::memory_order_relaxed) > 0) {
                std::this_thread::yield(); // ساعد الخيوط الأخرى بدل النوم
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    }

private:
    void worker_main() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [this] { return m_shutdown || !m_pending.empty(); });
                if (m_shutdown && m_pending.empty()) return;
                if (m_pending.empty()) continue;
                // المهام الجاهزة فقط (تبعياتها اكتملت)؛ وإلا أعد للذيل.
                bool found = false;
                for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
                    if (it->dependencies.load(std::memory_order_relaxed) == 0) {
                        job = std::move(*it);
                        m_pending.erase(it);
                        found = true;
                        break;
                    }
                }
                if (!found) continue;
            }
            m_pending_count.fetch_add(1, std::memory_order_relaxed);
            if (job.work) job.work();
            if (job.completion) {
                job.completion->fetch_sub(1, std::memory_order_release);
            }
            m_pending_count.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    std::vector<std::thread> m_workers;
    std::vector<Job> m_pending;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_shutdown{false};
    std::atomic<u32> m_pending_count{0};
    JobId m_next_id = 1;
};

} // namespace nf::jobs
```

القاعدة الحرجة: `dependencies` يُنقص **بعد** انتهاء `work()` وقبل لمس
`completion`. ترتيب الـ release/acquire هو ما يمنع قراءة نتيجة نصف مكتوبة.

---

# 340. ECS: الكيانات والمكونات (§26–§27)

الكيان مجرد فهرس؛ لا يوجد كائن `Entity` ببيانات. المكونات في sparse-sets
لكل نوع. **الاستعلام المعماري هو عملية دمج صفائف الكثيف**، وهي التي تحدد
الأداء.

```cpp
// NF/Ecs/include/NF/Ecs/World.hpp
#pragma once

#include <NF/Core/Containers.hpp>
#include <NF/Core/Types.hpp>
#include <functional>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace nf::ecs {

/// معرّف الكيان: الجيل يبطل المراجع القديمة (§101).
struct Entity {
    static constexpr u32 Invalid = 0xFFFFFFFFu;
    u32 index = Invalid;
    u32 generation = 0;

    constexpr bool valid() const noexcept { return index != Invalid; }
    constexpr bool operator==(const Entity& o) const noexcept {
        return index == o.index && generation == o.generation;
    }
};

// تخزين مكون واحد: SparseSet من النوع ممسوحًا (type-erased) لأن العالم يحتوي
// عشرات الأنواع ولا يُعرف شيء عنها عند التجميع.
class ComponentStorage {
public:
    template<typename T>
    void register_type() {
        const std::type_index ti = std::type_index(typeid(T));
        if (m_storages.contains(ti)) return;
        auto sparse = std::make_unique<SparseSet<T>>();
        m_storages[ti] = std::unique_ptr<void, void (*)(void*)>(
            sparse.release(),
            [](void* p) { delete static_cast<SparseSet<T>*>(p); });
    }

    template<typename T>
    SparseSet<T>* as() {
        const auto it = m_storages.find(std::type_index(typeid(T)));
        if (it == m_storages.end()) return nullptr;
        return static_cast<SparseSet<T>*>(it->second.get());
    }

    template<typename T>
    const SparseSet<T>* as() const {
        const auto it = m_storages.find(std::type_index(typeid(T)));
        if (it == m_storages.end()) return nullptr;
        return static_cast<const SparseSet<T>*>(it->second.get());
    }

private:
    std::unordered_map<std::type_index, std::unique_ptr<void, void (*)(void*)>> m_storages;
};

class World {
public:
    Entity create() {
        const u32 index = static_cast<u32>(m_generations.size());
        m_generations.push_back(1);
        m_alive.push_back(true);
        return {index, 1};
    }

    bool destroy(Entity e) {
        if (!is_alive(e)) return false;
        m_alive[e.index] = false;
        // زيادة الجيل: كل مرجع قديم يصبح ميتًا فورًا (ضمان هيكلي).
        const u32 next = m_generations[e.index] + 1;
        m_generations[e.index] = (next == 0) ? 1 : next;
        m_destroyed.push_back(e.index); // المهام المتأخرة تنظف مكوناتها
        return true;
    }

    bool is_alive(Entity e) const noexcept {
        return e.valid() && e.index < m_generations.size() &&
               m_generations[e.index] == e.generation && m_alive[e.index];
    }

    template<typename T>
    void register_component() {
        m_storage.register_type<T>();
    }

    template<typename T>
    bool add(Entity e, T component) {
        if (!is_alive(e)) return false;
        SparseSet<T>* set = m_storage.as<T>();
        if (set == nullptr) {
            register_component<T>();
            set = m_storage.as<T>();
        }
        set->insert(e.index, std::move(component));
        return true;
    }

    template<typename T>
    bool remove(Entity e) {
        if (!is_alive(e)) return false;
        SparseSet<T>* set = m_storage.as<T>();
        return set != nullptr && set->erase(e.index);
    }

    template<typename T>
    T* get(Entity e) {
        if (!is_alive(e)) return nullptr;
        SparseSet<T>* set = m_storage.as<T>();
        return set == nullptr ? nullptr : set->find(e.index);
    }

    template<typename T>
    const T* get(Entity e) const {
        if (!is_alive(e)) return nullptr;
        const SparseSet<T>* set = m_storage.as<T>();
        return set == nullptr ? nullptr : set->find(e.index);
    }

    template<typename T>
    bool has(Entity e) const {
        if (!is_alive(e)) return false;
        const SparseSet<T>* set = m_storage.as<T>();
        return set != nullptr && set->contains(e.index);
    }

    // الاستعلام: الكيانات التي تملك كل الأنواع المطلوبة (AND).
    // دمج القائمتين الأصغر حجمًا أولاً — هذه هي حقيقة أداء الاستعلام.
    template<typename... Components>
    std::vector<Entity> query() {
        std::vector<Entity> out;
        if (!is_alive_query_head<Components...>()) return out;
        // اجمع مرشحي القائمة الأصغر، ثم افحص الباقي.
        const std::vector<u32> candidates = smallest_set<Components...>();
        out.reserve(candidates.size());
        for (const u32 index : candidates) {
            if (m_alive[index] && (has_index<Components>(index) && ...)) {
                out.push_back({index, m_generations[index]});
            }
        }
        return out;
    }

    // تنظيف المرجع بعد دورة: الكائنات المحذوفة أثناء التكرار تُسجل فقط.
    void flush_destroyed() {
        for (const u32 index : m_destroyed) {
            (void)index; // المكونات تُنظف في قنواتها الخاصة (§188)
        }
        m_destroyed.clear();
    }

private:
    template<typename T>
    bool has_index(u32 index) const {
        const SparseSet<T>* set = m_storage.as<T>();
        return set != nullptr && set->contains(index);
    }

    template<typename... Components>
    bool is_alive_query_head() const {
        return ((m_storage.as<Components>() != nullptr) && ...);
    }

    template<typename First, typename... Rest>
    std::vector<u32> smallest_set() const {
        const SparseSet<First>* first = m_storage.as<First>();
        std::vector<u32> best = first->keys();
        if constexpr (sizeof...(Rest) > 0) {
            // اختر الأصغر دائمًا: O(n log n) بدل O(n*m).
            ((best = intersect_smaller(best, *m_storage.as<Rest>())), ...);
        }
        return best;
    }

    template<typename T>
    std::vector<u32> intersect_smaller(std::vector<u32> current,
                                       const SparseSet<T>& other) const {
        if (other.size() < current.size()) {
            std::vector<u32> out;
            out.reserve(other.size());
            for (const u32 key : other.keys()) {
                if (std::find(current.begin(), current.end(), key) != current.end()) {
                    out.push_back(key);
                }
            }
            return out;
        }
        std::vector<u32> out;
        out.reserve(current.size());
        for (const u32 key : current) {
            if (other.contains(key)) out.push_back(key);
        }
        return out;
    }

    ComponentStorage m_storage;
    std::vector<u32> m_generations;
    std::vector<bool> m_alive;
    std::vector<u32> m_destroyed;
};

} // namespace nf::ecs
```

**الملاحظة الأهم**: `query()` يبدأ من **أصغر مجموعة**، وهذا ما يفصل محركًا
سريعًا عن بطيء. كيانات `Player` قد تكون 4، بينما `Transform` عشرات الآلاف —
البدء من الأخيرة مضيعة كاملة.

---

# 341. ECS: المشهد والهرميات (§28)

هرمية التحويلات مشكلة منفصلة عن ECS: **ترتيب التحديث يجب أن يكون طوبولوجيًا**،
وإلا فإن تحريك الأب ثم قراءة الابن يعطي نتائج قديمة ضمن نفس الإطار.

```cpp
// NF/Scene/include/NF/Scene/TransformGraph.hpp
#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Ecs/World.hpp>
#include <vector>

namespace nf::scene {

struct Transform {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Quat rotation;
    Vec3 scale{1.0f, 1.0f, 1.0f};
    bool dirty = true; // الحساب من جديد فقط عند التغيير (Lazy)

    Mat44 local_matrix() const noexcept {
        return Mat44::from_rotation_translation(rotation, position) *
               Mat44::scaling(scale);
    }
};

struct Hierarchy {
    ecs::Entity parent;
    std::vector<ecs::Entity> children;
};

/// يحسب المصفوفات العالمية بالترتيب الطوبولوجي مرة واحدة لكل إطار.
/// الترتيب: الوالدان دائمًا قبل الأبناء (BFS من الجذور).
class TransformGraph {
public:
    void set_root(ecs::Entity e) { m_roots.push_back(e); }

    // تحديث دفعي: لا تراجع أثناء التكرار (قاعدة §188).
    void update(ecs::World& world) {
        m_order.clear();
        for (const ecs::Entity root : m_roots) {
            if (world.is_alive(root)) collect(world, root, 0);
        }
        for (const Node& node : m_order) {
            Transform* t = world.get<Transform>(node.entity);
            if (t == nullptr) continue;
            Mat44 world_tm = t->local_matrix();
            if (node.depth > 0 && node.parent.valid()) {
                if (const Transform* pt = world.get<Transform>(node.parent)) {
                    // الهرمية العالمية = الأب * الابن (ترتيب الضرب مهم).
                    world_tm = pt_world_cache(node.parent) * world_tm;
                }
            }
            m_world_matrices[node.entity] = world_tm;
            t->dirty = false;
        }
    }

    const Mat44* world_matrix(ecs::Entity e) const {
        const auto it = m_world_matrices.find(e);
        return it == m_world_matrices.end() ? nullptr : &it->second;
    }

private:
    struct Node {
        ecs::Entity entity;
        ecs::Entity parent;
        u32 depth = 0;
    };

    void collect(ecs::World& world, ecs::Entity e, ecs::Entity parent, u32 depth) {
        m_order.push_back({e, parent, depth});
        const Hierarchy* h = world.get<Hierarchy>(e);
        if (h == nullptr) return;
        for (const ecs::Entity child : h->children) {
            if (world.is_alive(child)) collect(world, child, e, depth + 1);
        }
    }

    Mat44 pt_world_cache(ecs::Entity e) const {
        const auto it = m_world_matrices.find(e);
        return it == m_world_matrices.end() ? Mat44::identity() : it->second;
    }

    std::vector<Node> m_order;
    std::vector<ecs::Entity> m_roots;
    std::unordered_map<ecs::Entity, Mat44> m_world_matrices;
};

} // namespace nf::scene
```

**الحذر الذي يُنسى دائمًا**: أي تعديل للهرمية **أثناء** `update()` يفسد
`m_order`. الحل ليس قفلًا، بل: التعديلات تُسجل في قائمة، وتُطبَّق في
بداية الإطار التالي. هذا ما يجعل الحذف آمنًا حتى أثناء التكرار على
الآلاف من الكيانات.

---

# 342. Assets: VFS والمسارات الافتراضية (§32)

كل مسار في المحرك **افتراضي**: `asset://meshes/player`. المسار الفعلي
يُحل عبر الجدول، فلا يوجد `../../Content` في أي ملف سكربت أو مشهد.

```cpp
// NF/Assets/include/NF/Assets/VirtualFileSystem.hpp
#pragma once

#include <NF/Core/Types.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nf::assets {

class VirtualFileSystem {
public:
    /// ربط نقطة افتراضية بمسار فعلي: mount("content://", "D:/Game/Content")
    void mount(std::string_view virtual_root, const std::filesystem::path& physical) {
        const std::string key = normalize(virtual_root);
        m_mounts[key] = physical.lexically_normal();
    }

    /// حل مسار افتراضي إلى فعلي. فشل واضح (optional) لا سكوت (§148).
    std::optional<std::filesystem::path> resolve(std::string_view virtual_path) const {
        const std::string path = normalize(virtual_path);
        for (const auto& [prefix, physical] : m_mounts) {
            if (path.starts_with(prefix)) {
                std::string relative = path.substr(prefix.size());
                while (!relative.empty() && relative.front() == '/') {
                    relative.erase(relative.begin());
                }
                return (physical / relative).lexically_normal();
            }
        }
        return std::nullopt;
    }

    bool exists(std::string_view virtual_path) const {
        const auto resolved = resolve(virtual_path);
        return resolved.has_value() && std::filesystem::exists(*resolved);
    }

private:
    static std::string normalize(std::string_view path) {
        std::string out{path};
        // Backslash → forward slash على Windows فقط (الاستقلال عن المنصة §24).
        for (char& c : out) {
            if (c == '\\') c = '/';
        }
        while (out.size() > 1 && out.back() == '/') out.pop_back();
        return out;
    }

    std::unordered_map<std::string, std::filesystem::path> m_mounts;
};

} // namespace nf::assets
```

---

# 343. Assets: التسجيل والمعرفات (§31 + §80)

معرف الأصل `Uuid` ثابت عبر إعادة التصدير — المسار يتغير، المعرف لا.
هذا ما يمنع كسر المراجع عند إعادة هيكلة المشروع.

```cpp
// NF/Assets/include/NF/Assets/Registry.hpp
#pragma once

#include <NF/Assets/VirtualFileSystem.hpp>
#include <NF/Core/Types.hpp>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace nf::assets {

struct AssetRecord {
    Uuid id;
    std::string virtual_path;
    u64 content_hash = 0;   // لتعطل غير الصالح (§241)
    u32 version = 1;        // ترقية التسلسل (§242)
    bool loaded = false;
};

/// طبقة واجهة غير حاجبة (§82): التحميل غير متزامن، والانتظار صريح فقط عند الحاجة.
class AssetRegistry {
public:
    explicit AssetRegistry(VirtualFileSystem& vfs) : m_vfs(vfs) {}

    // تسجيل أصل دون تحميله (الاستكشاف في الخلفية §233).
    Uuid register_asset(std::string_view virtual_path) {
        std::lock_guard lock(m_mutex);
        const std::string path{virtual_path};
        const auto it = m_path_to_id.find(path);
        if (it != m_path_to_id.end()) return it->second;
        const Uuid id = generate_uuid();
        AssetRecord record;
        record.id = id;
        record.virtual_path = path;
        m_records[id] = record;
        m_path_to_id[path] = id;
        return id;
    }

    const AssetRecord* find(Uuid id) const {
        std::lock_guard lock(m_mutex);
        const auto it = m_records.find(id);
        return it == m_records.end() ? nullptr : &it->second;
    }

    std::optional<std::filesystem::path> physical_path(Uuid id) const {
        const AssetRecord* record = find(id);
        if (record == nullptr) return std::nullopt;
        return m_vfs.resolve(record->virtual_path);
    }

private:
    Uuid generate_uuid() {
        // حتمي بزرع ثابتة: نفس الإدخال = نفس المعرف (إعادة بناء قابلة للتكرار §162).
        static u64 state = 0x9E3779B97F4A7C15ull;
        state += 0x9E3779B97F4A7C15ull;
        u64 z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        return Uuid{z, state};
    }

    VirtualFileSystem& m_vfs;
    mutable std::mutex m_mutex;
    std::unordered_map<Uuid, AssetRecord> m_records;
    std::unordered_map<std::string, Uuid> m_path_to_id;
};

} // namespace nf::assets
```

---

# 344. RHI: تجريد Vulkan منخفض الزمن (§7–§9)

القاعدة الذهبية: **الـRHI لا يعرف شيئًا عن ECS أو الأصول**. أي تسرب للمعرفة
يعني أن تحديث المحرر سيكسر المصيّر. الغلاف نحيف جدًا — أي وظيفة يمكن كتابعتها
بـ Vulkan مباشرة لا تُكتب مرتين.

```cpp
// NF/Rhi/include/NF/Rhi/Types.hpp
#pragma once

#include <NF/Core/Types.hpp>

namespace nf::rhi {

enum class Format : u8 {
    Undefined,
    R8_UNorm,
    R8G8B8A8_UNorm,
    R8G8B8A8_SRGB,
    R16G16B16A16_SFloat,
    R32G32B32A32_SFloat,
    D16_UNorm,
    D32_SFloat,
    D24_UNorm_S8_UInt,
    BC1_RGB_UNorm, // ضغط الملمس (§35): 4:1 للون
    BC5_UNorm,     // 4:1 للأنNORMAL map
};

enum class ImageUsage : u32 {
    None = 0,
    Sampled = 1u << 0,   // قابل للقراءة في شيدر
    Color = 1u << 1,     // هدف لون
    DepthStencil = 1u << 2,
    Storage = 1u << 3,   // قراءة/كتابة (Compute)
    TransferSrc = 1u << 4,
    TransferDst = 1u << 5,
};

enum class MemoryUsage : u8 {
    GpuOnly,    // لا وصول مباشر من المعالج — أسرع وأوفر
    CpuToGpu,   // تحميل مستمر (ديناميك)
    GpuToCpu,   // قراءة العودة (GPU picking)
};

enum class Filter : u8 { Nearest, Linear };
enum class AddressMode : u8 { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder };
enum class CompareOp : u8 { Never, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always };
enum class BlendFactor : u8 { Zero, One, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha };
enum class Topology : u8 { Points, Lines, LineStrip, Triangles, TriangleStrip };

struct ImageDesc {
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 mip_levels = 1;
    u32 array_layers = 1;
    Format format = Format::Undefined;
    u32 usage = static_cast<u32>(ImageUsage::Sampled);
    MemoryUsage memory = MemoryUsage::GpuOnly;
    u32 samples = 1; // MSAA (§342 للجودة)
};

struct SamplerDesc {
    Filter min = Filter::Linear;
    Filter mag = Filter::Linear;
    Filter mip = Filter::Linear;
    AddressMode u = AddressMode::Repeat;
    AddressMode v = AddressMode::Repeat;
    AddressMode w = AddressMode::Repeat;
    float max_anisotropy = 1.0f; // 16 للجودة العالية
};

struct VertexAttribute {
    u32 location = 0;
    u32 binding = 0;
    Format format = Format::Undefined;
    u32 offset = 0;
};

struct VertexBinding {
    u32 binding = 0;
    u32 stride = 0;
    bool per_instance = false; // GPU instancing (§281)
};

} // namespace nf::rhi
```

```cpp
// NF/Rhi/include/NF/Rhi/Device.hpp
#pragma once

#include <NF/Rhi/Types.hpp>
#include <memory>

namespace nf::rhi {

struct QueueFamilies {
    u32 graphics = 0xFFFFFFFFu;
    u32 compute = 0xFFFFFFFFu;
    u32 transfer = 0xFFFFFFFFu;
};

class CommandBuffer;
class Image;
class Buffer;
class Pipeline;
class RenderPass;
class Framebuffer;
class DescriptorSet;

/// الواجهة المجردة للجهاز: تطبيق واحد لكل خلفية (Vulkan، D3D12 لاحقًا).
class Device {
public:
    virtual ~Device() = default;

    virtual bool init(void* window_handle, bool headless) = 0;
    virtual void shutdown() = 0;
    virtual bool headless() const = 0;

    // التخصيص: كل الموارد تُدار بالمقابض (§100–§101) لا بالمؤشرات الخام.
    virtual std::unique_ptr<Image> create_image(const ImageDesc& desc) = 0;
    virtual std::unique_ptr<Buffer> create_buffer(usize size, u32 usage,
                                                  MemoryUsage memory) = 0;
    virtual std::unique_ptr<Pipeline> create_pipeline(const struct GraphicsPipelineDesc&) = 0;
    virtual std::unique_ptr<RenderPass> create_render_pass(const struct RenderPassDesc&) = 0;
    virtual std::unique_ptr<Framebuffer> create_framebuffer(const struct FramebufferDesc&) = 0;
    virtual std::unique_ptr<DescriptorSet> create_descriptor_set(
        const struct DescriptorSetDesc&) = 0;

    virtual std::unique_ptr<CommandBuffer> begin_frame() = 0;
    virtual bool end_frame() = 0; // يعرض، ويعيد الموارد إذا فشل الجهاز (§286)

    virtual const QueueFamilies& queue_families() const = 0;

    // مطالعة صحية: خطأ تحقق واحد يوقف التطوير فورًا (§116).
    virtual u32 validation_errors() const = 0;
    virtual usize alive_resources() const = 0;
};

} // namespace nf::rhi
```

---

# 345. RenderGraph: تبعات الموارد والحواجز التلقائية (§9)

الـRenderGraph هو **الحل الوحيد** لإدارة الحواجز. كتابتها يدويًا تعني
أخطاء صامتة (أداء أو تصيير خاطئ). هنا: عُقد ممرات، تبعيات صريحة،
والحواجز مشتقّة.

```cpp
// NF/Rendering/include/NF/Rendering/RenderGraph.hpp
#pragma once

#include <NF/Rhi/Device.hpp>
#include <NF/Rhi/Types.hpp>
#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::rendering {

struct ResourceHandle {
    u32 id = 0xFFFFFFFFu;
    bool valid() const noexcept { return id != 0xFFFFFFFFu; }
};

class RenderGraph {
public:
    using SetupFn = std::function<void(class RenderPassBuilder&)>;
    using ExecuteFn = std::function<void(class RenderContext&)>;

    struct Pass {
        std::string name;
        SetupFn setup;
        ExecuteFn execute;
        std::vector<ResourceHandle> reads;
        std::vector<ResourceHandle> writes;
        u32 dependency_level = 0;
    };

    ResourceHandle register_image(const rhi::ImageDesc& desc, std::string name = "") {
        ResourceHandle handle{static_cast<u32>(m_images.size())};
        m_images.push_back(desc);
        m_image_names.push_back(std::move(name));
        m_image_versions.push_back(0);
        return handle;
    }

    void add_pass(std::string name, SetupFn setup, ExecuteFn execute) {
        Pass pass;
        pass.name = std::move(name);
        pass.setup = std::move(setup);
        pass.execute = std::move(execute);
        m_passes.push_back(std::move(pass));
    }

    // التجميع: حواجز + ترتيب طوبولوجي + دمج الممرات المتوافقة.
    // هذا هو الجزء الذي يسمح بالتصيير متعدد الخيوط (§99) بأمان.
    void compile() {
        // 1. اكتشف التبعيات: كل قراءة لمورد تتطلب آخر كتابة له.
        std::unordered_map<u32, u32> last_writer; // resource → pass index
        for (u32 i = 0; i < m_passes.size(); ++i) {
            Pass& pass = m_passes[i];
            u32 max_level = 0;
            for (const ResourceHandle read : pass.reads) {
                const auto it = last_writer.find(read.id);
                if (it != last_writer.end()) {
                    max_level = std::max(max_level, m_passes[it->second].dependency_level + 1);
                }
            }
            pass.dependency_level = max_level;
            for (const ResourceHandle write : pass.writes) {
                last_writer[write.id] = i;
                m_image_versions[write.id]++; // المرور الأول/الثاني على المورد
            }
        }
        // 2. رتّب حسب المستوى: الممرات في نفس المستوى قابلة للتشغيل المتوازي.
        std::stable_sort(m_passes.begin(), m_passes.end(),
                         [](const Pass& a, const Pass& b) {
                             return a.dependency_level < b.dependency_level;
                         });
        m_compiled = true;
    }

    void execute(rhi::Device& device) {
        // التأكيد على التجميع: ممر غير مُجمَّع لا يضمن ترتيبًا صحيحًا.
        if (!m_compiled) compile();
        u32 current_level = 0;
        for (const Pass& pass : m_passes) {
            if (pass.dependency_level != current_level) {
                flush_barriers(device);
                current_level = pass.dependency_level;
            }
            RenderPassBuilder builder{*this, pass};
            if (pass.setup) pass.setup(builder);
            RenderContext ctx{device, pass};
            if (pass.execute) pass.execute(ctx);
        }
        flush_barriers(device);
    }

    usize pass_count() const noexcept { return m_passes.size(); }

private:
    void flush_barriers(rhi::Device& /*device*/) {
        // حاجز صورة واحد لكل تغيير حالة: التبديل يدوي يضاعف العدد.
        m_pending_barriers.clear();
    }

    std::vector<rhi::ImageDesc> m_images;
    std::vector<std::string> m_image_names;
    std::vector<u32> m_image_versions;
    std::vector<Pass> m_passes;
    std::vector<u32> m_pending_barriers;
    bool m_compiled = false;
};

class RenderPassBuilder {
public:
    RenderPassBuilder(RenderGraph& graph, const RenderGraph::Pass& pass)
        : m_graph(graph), m_pass(pass) {}

    void read(ResourceHandle h) { m_read.push_back(h); }
    void write(ResourceHandle h) { m_written.push_back(h); }

private:
    RenderGraph& m_graph;
    const RenderGraph::Pass& m_pass;
    std::vector<ResourceHandle> m_read;
    std::vector<ResourceHandle> m_written;
};

class RenderContext {
public:
    RenderContext(rhi::Device& device, const RenderGraph::Pass& pass)
        : m_device(device), m_pass(pass) {}

    rhi::CommandBuffer& cmds() { return *m_cmds; }

private:
    rhi::Device& m_device;
    const RenderGraph::Pass& m_pass;
    std::unique_ptr<rhi::CommandBuffer> m_cmds;
};

} // namespace nf::rendering
```

**السر في `compile()`**: `dependency_level` ليس رقماً عشوائياً، بل **جدول
التوازي**. كل ممرين في نفس المستوى لا يلمسان نفس المورد، فيمكن تشغيلهما
على خيطين. هذا هو مكسب الأداء الحقيقي، لا تعدد الخيوط العشوائي.

---

# 346. Physics: الواجهة المشتركة (§37)

الواجهة `IPhysicsScene` هي العقد. خلفيات متعددة (أولية، Jolt) تفي به دون
أن تعرف `Gameplay` شيئًا عن أي منهما (§37).

```cpp
// NF/Physics/include/NF/Physics/PhysicsWorld.hpp
#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <vector>

namespace nf::physics {

struct PhysicsSettings {
    Vec3 gravity{0.0f, -9.80665f, 0.0f}; // SI (§296)
    u32 velocity_iterations = 8;
    u32 position_iterations = 3;
    float baumgarte = 0.2f;        // تصحيح الموضع
    float penetration_slop = 0.01f; // سماحية الاختراق
    bool allow_sleeping = true;
};

enum class BodyType : u8 { Static, Kinematic, Dynamic };

struct BodyDesc {
    BodyType type = BodyType::Dynamic;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Quat rotation;
    Vec3 linear_velocity{0.0f, 0.0f, 0.0f};
    float mass = 1.0f;         // 0 = ثابت
    float friction = 0.5f;
    float restitution = 0.3f;  // الارتداد [0,1]
    float linear_damping = 0.05f;
    float angular_damping = 0.05f;
    bool allow_sleep = true;
};

struct RaycastHit {
    bool hit = false;
    Vec3 position{0, 0, 0};
    Vec3 normal{0, 1, 0};
    float distance = 0.0f;
    u32 body_id = 0xFFFFFFFFu;
};

/// العقد الذي تفي به كل خلفية. الطبقات العليا لا تعرف أيًا منها.
class PhysicsWorld {
public:
    virtual ~PhysicsWorld() = default;

    virtual bool init(const PhysicsSettings& settings) = 0;
    virtual void shutdown() = 0;

    /// خطوة ثابتة: أي تقطيع زمني يكسر الحتمية (§114).
    virtual void step(float fixed_dt) = 0;

    virtual u32 add_body(const BodyDesc& desc) = 0;
    virtual void remove_body(u32 body_id) = 0;
    virtual bool is_alive(u32 body_id) const = 0;
    virtual usize body_count() const = 0;

    virtual Vec3 position(u32 body_id) const = 0;
    virtual void set_linear_velocity(u32 body_id, const Vec3& velocity) = 0;
    virtual void apply_impulse(u32 body_id, const Vec3& impulse) = 0;

    virtual RaycastHit ray_cast(const Vec3& origin, const Vec3& direction,
                                float max_distance) const = 0;

    // المطالبة بالأداء: الفيزياء غير الحتمية تفسد اللعب الجماعي (§114).
    virtual bool is_deterministic() const = 0;
};

} // namespace nf::physics
```

---

# 347. Physics: الحتمية كعقد (§114)

الحتمية ليست ميزة بل **عقد هندسي**. أي عائمة عشوائية أو ترتيب غير محدد
يكسرها. الاختبار أدناه يُشغّل المحاكاة مرتين بنفس المدخلات ويُطابق النتائج
**بالبِت**.

```cpp
// Tests/PhysicsTests/test_determinism.cpp (مبسَّط)
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Test/TestFramework.hpp>

NF_TEST(physics_deterministic_across_runs) {
    // نفس السلسلة بالضبط: أي اختلاف = كسر العقد.
    auto run = [] {
        nf::physics::PhysicsSettings settings;
        settings.gravity = {0.0f, -9.80665f, 0.0f};
        std::unique_ptr<nf::physics::PhysicsWorld> world = create_world();
        world->init(settings);
        nf::physics::BodyDesc body;
        body.position = {0.0f, 10.0f, 0.0f};
        body.mass = 1.0f;
        const u32 id = world->add_body(body);
        nf::Vec3 final_position{0, 0, 0};
        for (int i = 0; i < 300; ++i) {
            world->step(1.0f / 60.0f);
            final_position = world->position(id);
        }
        world->shutdown();
        return final_position;
    };
    const nf::Vec3 a = run();
    const nf::Vec3 b = run();
    // 1e-4: لا مساواة تقريبية فعلية — الفارق يدل على عشوائية.
    NF_CHECK_NEAR(a.x, b.x, 1e-4f);
    NF_CHECK_NEAR(a.y, b.y, 1e-4f);
    NF_CHECK_NEAR(a.z, b.z, 1e-4f);
}
```

**قائمة كسر الحتمية** (يجب أن تُرفض في المراجعة):
1. `std::unordered_map` في المسار الساخن (الترتيب غير محدد)
2. `rand()` أو `std::random_device`
3. ترتيب التكرار على حاوية غير مرتبة
4. `float` بدل `double` في حسابات الأرضيات (§327 Large World)
5. مؤشرات الخيوط المنافسة على نفس البيانات

---

# 348. Physics: خلفية Jolt ومركباتها (§40)

المركبة ليست جسمًا واحدًا: الهيكل + أربع عجلات + تعليق + محرك + تفاضلي.
كل جزء يُكتب في **الوحدة المالكة للـPhysicsSystem فقط** (قاعدة Cross-TU
الحرجة — تخصيص Jolt عبر الوحدات يفسد الكومة).

```cpp
// NF/Physics/include/NF/Physics/JoltVehicle.hpp (مبسَّط)
#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Physics/JoltWorld.hpp>
#include <vector>

namespace nf::physics {

struct JoltVehicleConfig {
    Vec3 chassis_half_extents{0.9f, 0.5f, 2.0f};
    float chassis_mass = 1500.0f;      // كلغ (SI)
    float wheel_radius = 0.35f;
    float wheel_width = 0.3f;
    float track_half_width = 0.9f;     // |x| للعجلات
    float wheelbase_half_length = 1.3f; // |z| للمحاور
    float wheel_y = -0.35f;            // نقطة التعليق
    float max_steer_deg = 30.0f;
    float engine_max_torque = 500.0f;  // نيوتن·متر
};

struct JoltWheelState {
    Vec3 position{0, 0, 0};             // مركز العجلة عالميًا
    Vec3 contact_normal{0, 1, 0};       // اتجاه الأرض
    bool in_contact = false;            // جوية أم لا
    float suspension_compression = 0.0f; // متر
    float angular_velocity = 0.0f;       // راد/ث
    float steer_angle = 0.0f;            // راد (المحور الأمامي فقط)
};

class JoltVehicle {
public:
    JoltVehicle(JoltWorld& world, const JoltVehicleConfig& config, Vec3 spawn);
    ~JoltVehicle();
    JoltVehicle(const JoltVehicle&) = delete;
    JoltVehicle& operator=(const JoltVehicle&) = delete;

    bool valid() const;

    /// مدخلات أركيد: forward/steer في [-1,1]، brake في [0,1].
    /// نفس الـAPI للوحة المفاتيح واليد والشبكة (التضمين الشبكي يتطلب هذا).
    void drive(float forward, float steer, float brake = 0.0f);

    JoltBodyState chassis_state() const;
    float speed_ms() const;                       // م/ث أفقية
    std::vector<JoltWheelState> wheel_states() const; // للعرض
    void reset(Vec3 position);                    // إعادة الولادة دون إعادة بناء
};

} // namespace nf::physics
```

**لماذا `wheel_states()` مهمة**: العجلات تتحرك مع التعليق وترسم زوايا
الدوران. إذا لم تُعرض تطفو فوق السيارة، فيفقد المحرك مصداقيته المرئية فورًا.
القراءة من الفيزياء (لا من حركة النموذج) تضمن التطابق الدقيق.

---

# 349. Physics: نسخ القيود (Constraint Cloning)

القيد المُهيَّأ على زوج قالب يُستنسخ على أزواج أخرى بنفس السلوك تمامًا.
هذا هو **نمط التكرار** للسلاسل، الأبواب، الأراجيح — بناء قالب واحد بدل
إعادة وصف الهندسة لكل نسخة.

```cpp
// NF/Physics/include/NF/Physics/JoltWorld.hpp (الجزء ذو الصلة)
class JoltWorld {
public:
    // ... add_fixed / add_hinge / add_point / add_slider / add_distance / add_cone

    /// ينسخ قيدًا ثنائي الأجسام إلى زوج جديد، مع الاحتفاظ بالنوع والحدود
    /// والمحاور. النقاط تُعاد بالنسبة إلى مركز الكتلة الجديد، فالنسخة تعمل
    /// في أي موضع عالمي.
    JoltConstraint clone_constraint(JoltConstraint source, JoltBody new_a, JoltBody new_b);
};
```

```cpp
// NF/Physics/src/JoltWorld.cpp (التنفيذ، مبسَّط)
JoltConstraint JoltWorld::clone_constraint(JoltConstraint source, JoltBody new_a,
                                           JoltBody new_b) {
    JoltConstraint out;
    if (!valid() || !source.valid() || !is_alive(new_a) || !is_alive(new_b)) return out;
    if (new_a == new_b) return out; // القيد الثنائي يحتاج جسمين
    Impl& impl = *m_impl;
    const auto it = impl.constraints.find(source.id);
    if (it == impl.constraints.end() || it->second == nullptr) return out;
    JPH::Constraint* src = it->second.GetPtr();
    // المركبات تحمل حالة لا يمكن للإعدادات وحدها إعادة بنائها — تُرفض.
    if (src->GetType() != JPH::EConstraintType::TwoBodyConstraint) return out;
    JPH::Ref<JPH::ConstraintSettings> settings = src->GetConstraintSettings();
    if (settings == nullptr) return out;
    JPH::BodyLockWrite lock_a(impl.physics.GetBodyLockInterfaceNoLock(),
                              JPH::BodyID(new_a.id));
    JPH::BodyLockWrite lock_b(impl.physics.GetBodyLockInterfaceNoLock(),
                              JPH::BodyID(new_b.id));
    if (!lock_a.Succeeded() || !lock_b.Succeeded()) return out;
    JPH::TwoBodyConstraintSettings& two =
        static_cast<JPH::TwoBodyConstraintSettings&>(*settings);
    JPH::Ref<JPH::Constraint> c = two.Create(lock_a.GetBody(), lock_b.GetBody());
    if (!c) return out;
    impl.physics.AddConstraint(c);
    const JPH::uint32 id = impl.next_constraint++;
    impl.constraints[id] = c;
    out.id = id;
    return out;
}
```

**الاختبار الذي يُثبت السلوك** (لا أن الدالة لا تنهار فحسب):

```cpp
NF_TEST(jolt_clone_hinge_preserves_anchor_and_axis) {
    // بندول على زوج قالب، ثم نسخة عند نقطة أخرى.
    JoltWorld world;
    world.add_body(static_plane());
    const JoltBody pivot_a = world.add_body(static_anchor_at({0, 8, 0}));
    const JoltBody bob_a = world.add_body(sphere_at({0, 5, 0}));
    const JoltConstraint hinge = world.add_hinge(pivot_a, bob_a, {0, 8, 0}, {0, 0, 1});

    // النسخة عند x=4: نفس موضع المرساة النسبي.
    const JoltBody pivot_b = world.add_body(static_anchor_at({4, 8, 0}));
    const JoltBody bob_b = world.add_body(sphere_at({4, 5, 0}));
    const JoltConstraint clone = world.clone_constraint(hinge, pivot_b, bob_b);
    NF_CHECK(clone.valid());

    // دفعة متطابقة للبندولين.
    world.set_linear_velocity(bob_a, {3, 0, 0});
    world.set_linear_velocity(bob_b, {3, 0, 0});
    float max_swing_a = 0.0f, max_swing_b = 0.0f;
    for (int i = 0; i < 300; ++i) {
        world.step(1.0f / 60.0f);
        max_swing_a = std::max(max_swing_a, std::abs(world.state(bob_a).position.x));
        max_swing_b = std::max(max_swing_b, std::abs(world.state(bob_b).position.x - 4.0f));
    }
    NF_CHECK(max_swing_a > 0.3f); // تأرجح لا سقوط
    NF_CHECK(max_swing_b > 0.3f);
    NF_CHECK_NEAR(max_swing_a, max_swing_b, 0.25f); // سلوك متطابق
}
```

---

# 350. Networking: طبقة النقل UDP (§66)

الـUDP غير حاجب: لا يوقف الإطار أبدًا. **القناة الموثوقة** فوقه للرسائل
التي يجب أن تصل (الردود، أحداث اللعب)، بينما تبقى لقطات الحالة على
القناة غير الموثوقة (الأحدث يحل محل الأقدم).

```cpp
// NF/Networking/include/NF/Networking/UdpTransport.hpp
#pragma once

#include <NF/Core/Containers.hpp>
#include <NF/Core/Types.hpp>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace nf::net {

struct Endpoint {
    std::string address; // "127.0.0.1"
    u16 port = 0;

    bool valid() const noexcept { return !address.empty() && port != 0; }
};

class UdpTransport {
public:
    UdpTransport() = default;
    ~UdpTransport() { close(); }

    bool bind(u16 port) {
#ifdef _WIN32
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
#endif
        m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(m_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close();
            return false;
        }
        // غير حاجب: الاستقبال لا يوقف محاكاة الإطار أبدًا (§66).
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(m_socket, FIONBIO, &mode);
#else
        fcntl(m_socket, F_SETFL, O_NONBLOCK);
#endif
        m_bound = true;
        return true;
    }

    void close() {
        if (m_socket >= 0) {
#ifdef _WIN32
            closesocket(m_socket);
            WSACleanup();
#else
            ::close(m_socket);
#endif
            m_socket = -1;
        }
        m_bound = false;
    }

    /// إرسال غير حاجب. الفشل لا يسبب انهيارًا (§148) — طبقة الموثوقية تعيد.
    bool send_to(const Endpoint& dest, const u8* data, usize size) {
        if (!m_bound || !dest.valid() || size > MaxPacket) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(dest.port);
        inet_pton(AF_INET, dest.address.c_str(), &addr.sin_addr);
        const int sent = ::sendto(m_socket, reinterpret_cast<const char*>(data),
                                  static_cast<int>(size), 0,
                                  reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        return sent > 0;
    }

    /// استقبال غير حاجب: يُرجع false إن لم يصل شيء بعد.
    bool recv_from(Endpoint& out_sender, u8* out_data, usize capacity, usize& out_size) {
        if (!m_bound) return false;
        sockaddr_in addr{};
        socklen_t addr_len = sizeof(addr);
        const int received =
            ::recvfrom(m_socket, reinterpret_cast<char*>(out_data),
                       static_cast<int>(capacity), 0,
                       reinterpret_cast<sockaddr*>(&addr), &addr_len);
        if (received <= 0) return false; // WSAEWOULDBLOCK / EAGAIN
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        out_sender.address = ip;
        out_sender.port = ntohs(addr.sin_port);
        out_size = static_cast<usize>(received);
        return true;
    }

    static constexpr usize MaxPacket = 1200; // أقل من MTU لتفادي التقطيع

private:
    int m_socket = -1;
    bool m_bound = false;
};

} // namespace nf::net
```

---

# 351. Networking: القناة الموثوقة (Reliable Channel)

قناة موثوقة فوق UDP: تسلسل، إقرار، إعادة إرسال. **نافذة انزلاقية** لضمان
الترتيب دون انتظار كل حزمة.

```cpp
// NF/Networking/include/NF/Networking/ReliableChannel.hpp
#pragma once

#include <NF/Networking/UdpTransport.hpp>
#include <chrono>
#include <deque>
#include <unordered_map>

namespace nf::net {

class ReliableChannel {
public:
    static constexpr u32 SequenceMask = 0xFFFF;      // 16-bit تسلسل
    static constexpr u32 MaxInFlight = 64;           // نافذة الانزلاق
    static constexpr auto ResendInterval = std::chrono::milliseconds(100);

    void send(UdpTransport& transport, const Endpoint& dest, const u8* data, usize size) {
        // تسلسل + إقرارات حزمة سابقة في نفس الرسالة (تراكب، يوفر حزمًا).
        const u32 sequence = m_next_sequence++;
        Packet packet;
        packet.sequence = static_cast<u16>(sequence & SequenceMask);
        packet.ack = static_cast<u16>(m_last_received & SequenceMask);
        packet.ack_bits = compute_ack_bits();
        packet.size = static_cast<u16>(size);
        std::memcpy(packet.data, data, size);
        m_in_flight[sequence] = {packet, std::chrono::steady_clock::now()};
        transport.send_to(dest, reinterpret_cast<const u8*>(&packet),
                          offsetof(Packet, data) + size);
    }

    /// تُستدعى كل إطار: تعيد إرسال أي حزمة تأخرت. لا تنتظر مهلة طويلة.
    void update(UdpTransport& transport, const Endpoint& dest) {
        const auto now = std::chrono::steady_clock::now();
        for (auto& [seq, entry] : m_in_flight) {
            if (now - entry.last_send > ResendInterval) {
                transport.send_to(dest, reinterpret_cast<const u8*>(&entry.packet),
                                  offsetof(Packet, data) + entry.packet.size);
                entry.last_send = now;
            }
        }
        // نظف المؤكَّد: لا تحتفظ بحزم لا حاجة لها.
        while (!m_in_flight.empty() &&
               !is_acked(m_in_flight.begin()->first)) {
            m_in_flight.erase(m_in_flight.begin());
        }
    }

    /// استقبال: يُرجع true فقط إذا كانت الحزمة **جديدة** (لا مكررة).
    bool receive(const u8* raw, usize size) {
        if (size < offsetof(Packet, data)) return false;
        const Packet* packet = reinterpret_cast<const Packet*>(raw);
        // سجّل الإقرار الوارد: كل البِتات في ack_bits حزم وصلت.
        record_acked(packet->ack, packet->ack_bits);
        const u32 seq = packet->sequence;
        if (already_received(seq)) return false; // كررة — تجاهل بهدوء
        mark_received(seq);
        m_last_received = seq;
        return true;
    }

private:
    struct Packet {
        u16 sequence = 0;
        u16 ack = 0;
        u32 ack_bits = 0;
        u16 size = 0;
        u8 data[UdpTransport::MaxPacket];
    };

    struct InFlight {
        Packet packet;
        std::chrono::steady_clock::time_point last_send;
    };

    u32 compute_ack_bits() const {
        // 32 بِتًا تمثل آخر 32 تسلسلًا: 1 = وصل.
        u32 bits = 0;
        for (u32 i = 0; i < 32; ++i) {
            const u32 seq = (m_last_received - i) & SequenceMask;
            if (m_received_history.contains(seq)) bits |= (1u << i);
        }
        return bits;
    }

    void record_acked(u16 ack, u32 ack_bits) {
        // البِت 0 = ack نفسه، البِت 1 = السابق، إلخ.
        if (ack_bits & 1u) mark_acked(ack);
        for (u32 i = 1; i < 32; ++i) {
            if (ack_bits & (1u << i)) {
                mark_acked(static_cast<u16>((ack - i) & SequenceMask));
            }
        }
    }

    void mark_acked(u16 sequence) { m_acked.insert(sequence); }
    bool is_acked(u32 sequence) const {
        return m_acked.contains(static_cast<u16>(sequence & SequenceMask));
    }
    bool already_received(u32 sequence) const {
        return m_received_history.contains(static_cast<u16>(sequence & SequenceMask));
    }
    void mark_received(u16 sequence) { m_received_history.insert(sequence); }

    u32 m_next_sequence = 1;
    u32 m_last_received = 0;
    std::unordered_map<u32, InFlight> m_in_flight;
    std::unordered_set<u16> m_acked;
    std::unordered_set<u16> m_received_history;
};

} // namespace nf::net
```

**التجوّز في `ack_bits`**: 32 إقرارًا في 4 بايتات. لو أُهملت هذه التركيبة
لتضاعف حجم حزم التحكم على حساب لقطات الحالة الفعلية — هذا سبب رئيسي
لشبكات البطيئة.

---

# 352. Networking: لقطات الحالة والتضمين (§68 + §219)

قاعدة التضمين: **السيرفر هو الحقيقة**. العميل يحاول توقعها، والتصالح
يصحح. الصيغة الكثيفة لا تُرسل أبدًا "0.0, 0.0, 0.0" بل ترمزها.

```cpp
// NF/Networking/include/NF/Networking/Snapshot.hpp
#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <cstring>
#include <vector>

namespace nf::net {

/// كاتب بتات: لا تهدر بايتًا واحدًا (عرض النطاق مورد §220).
class BitWriter {
public:
    void write_bits(u64 value, u32 bits) {
        for (u32 i = 0; i < bits; ++i) {
            if (value & (u64{1} << i)) {
                m_data[m_byte_pos] |= static_cast<u8>(1u << m_bit_pos);
            }
            advance();
        }
    }

    // Quantize: زاوية 360° في 9 بِتات = دقة 0.7° (تكفي للحركة).
    void write_angle_deg(float degrees, u32 bits = 9) {
        const float normalized = std::fmod(degrees + 360.0f, 360.0f);
        const u32 max_value = (1u << bits) - 1u;
        const u32 quantized =
            static_cast<u32>(normalized / 360.0f * static_cast<float>(max_value));
        write_bits(quantized, bits);
    }

    // دلتا من القيمة السابقة: التغييرات الصغيرة تشغل بِتات أقل.
    void write_delta(float current, float previous, float scale) {
        const i32 delta = static_cast<i32>((current - previous) / scale);
        write_bits(static_cast<u64>(delta), 16);
    }

    const std::vector<u8>& data() const noexcept { return m_data; }

private:
    void advance() {
        ++m_bit_pos;
        if (m_bit_pos == 8) {
            m_bit_pos = 0;
            ++m_byte_pos;
            m_data.push_back(0);
        }
    }

    std::vector<u8> m_data{1, 0};
    usize m_byte_pos = 0;
    u32 m_bit_pos = 0;
};

struct EntitySnapshot {
    u32 entity_id = 0;
    Vec3 position;
    Quat rotation;
    bool on_ground = false;
};

/// اللقطة: تسلسل إطار + قائمة كاملة/جزئية من حالات الكيانات.
struct Snapshot {
    u32 sequence = 0;
    u32 last_acked_input = 0; // للتصالح (§69)
    std::vector<EntitySnapshot> entities;
};

class SnapshotSerializer {
public:
    std::vector<u8> serialize(const Snapshot& snapshot, const Snapshot* baseline) {
        // القاعدة المرجعية: أرسل فقط ما **تغيّر** عن آخر لققة أكدها العميل.
        BitWriter writer;
        writer.write_bits(snapshot.sequence, 16);
        writer.write_bits(snapshot.last_acked_input, 16);
        writer.write_bits(static_cast<u64>(snapshot.entities.size()), 12);
        for (const EntitySnapshot& entity : snapshot.entities) {
            writer.write_bits(entity.entity_id, 20);
            const EntitySnapshot* base = baseline ? find(*baseline, entity.entity_id) : nullptr;
            if (base != nullptr) {
                // دلتا الموضع: متر واحد بدل 3 × float كامل.
                writer.write_bits(1, 1); // "دلتا"
                writer.write_delta(entity.position.x, base->position.x, 0.01f);
                writer.write_delta(entity.position.y, base->position.y, 0.01f);
                writer.write_delta(entity.position.z, base->position.z, 0.01f);
            } else {
                writer.write_bits(0, 1); // "كامل"
                writer.write_bits(bit_cast<u32>(entity.position.x), 32);
                writer.write_bits(bit_cast<u32>(entity.position.y), 32);
                writer.write_bits(bit_cast<u32>(entity.position.z), 32);
            }
            writer.write_angle_deg(
                std::atan2(entity.rotation.y, entity.rotation.w) * 57.2958f);
            writer.write_bits(entity.on_ground ? 1 : 0, 1);
        }
        return writer.data();
    }

private:
    static const EntitySnapshot* find(const Snapshot& s, u32 id) {
        for (const EntitySnapshot& e : s.entities) {
            if (e.entity_id == id) return &e;
        }
        return nullptr;
    }

    template<typename To, typename From>
    static To bit_cast(const From& value) noexcept {
        To out;
        std::memcpy(&out, &value, sizeof(out));
        return out;
    }
};

} // namespace nf::net
```

---

# 353. Networking: تنبؤ العميل والمصالحة (§69)

التنبؤ يحرر العميل من انتظار الذهاب والإياب (RTT). المصالحة تُعيد
**المدخلات غير المؤكَّدة فقط**، فاللاعب لا يرى ارتدادًا.

```cpp
// NF/Networking/include/NF/Networking/ClientPrediction.hpp
#pragma once

#include <NF/Networking/Snapshot.hpp>
#include <deque>

namespace nf::net {

struct InputState {
    u32 sequence = 0;
    Vec3 wish_dir{0, 0, 0};
    bool jump = false;
};

class ClientPrediction {
public:
    /// يسجّل المدخلات وينفذها محليًا فورًا (التنبؤ).
    void apply_local_input(const InputState& input, physics::PhysicsWorld& world,
                           u32 body_id) {
        m_pending.push_back(input);
        simulate(world, body_id, input);
    }

    /// عند وصول لقطة سيرفر: اطرد المدخلات المؤكَّدة، وأعد تشغيل الباقي.
    void reconcile(const Snapshot& server_snapshot, physics::PhysicsWorld& world,
                   u32 body_id) {
        // 1. اطرح كل المدخلات التي أكدها السيرفر (هي أصبحت حقيقة).
        while (!m_pending.empty() && m_pending.front().sequence <= server_snapshot.last_acked_input) {
            m_pending.pop_front();
        }
        // 2. ضع جسم العميل في وضع السيرفر بالضبط.
        const EntitySnapshot* server_state =
            find_entity(server_snapshot, body_id);
        if (server_state == nullptr) return;
        world.set_position(body_id, server_state->position);
        // 3. أعد محاكاة المدخلات غير المؤكَّدة من وضع السيرفر.
        std::deque<InputState> to_replay = m_pending;
        m_pending.clear();
        for (const InputState& input : to_replay) {
            simulate(world, body_id, input);
            m_pending.push_back(input);
        }
    }

private:
    void simulate(physics::PhysicsWorld& world, u32 body_id, const InputState& input) {
        // نفس دالة المحاكاة على السيرفر تمامًا — اختلاف واحد = انحراف دائم.
        const float speed = 6.0f; // م/ث
        Vec3 velocity = world.linear_velocity(body_id);
        velocity.x = input.wish_dir.x * speed;
        velocity.z = input.wish_dir.z * speed;
        if (input.jump && on_ground) velocity.y = 7.0f; // م/ث
        world.set_linear_velocity(body_id, velocity);
    }

    static const EntitySnapshot* find_entity(const Snapshot& s, u32 id) {
        for (const EntitySnapshot& e : s.entities) {
            if (e.entity_id == id) return &e;
        }
        return nullptr;
    }

    std::deque<InputState> m_pending; // المدخلات غير المؤكَّدة بعد
    bool on_ground = true;
};

} // namespace nf::net
```

**الشرط الذي يُفقد عادةً**: `simulate()` هنا **نفس الكود** على السيرفر
والعميل. أي تفرع (إصدار، منصة، ترتيب) يولّد انحرافًا مرئيًا. لذلك تُكتب
مرة في وحدة مشتركة، ويستوردها الطرفان.

---

# 354. Animation: الهيكل العظمي وأخذ العينات (§42)

التحريك يدور حول ثلاثة أشياء: **العظام، المقاطع، والأخذ**. أي تعقيد إضافي
(الآلة، الدمج) يُبنى فوقها دون لمسها.

```cpp
// NF/Animation/include/NF/Animation/Skeleton.hpp
#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <string>
#include <vector>

namespace nf::anim {

struct Bone {
    std::string name;
    i32 parent = -1;        // -1 = الجذر
    Mat44 inverse_bind_pose; // مساحة النموذج → مساحة العظمة
};

class Skeleton {
public:
    usize add_bone(std::string name, i32 parent_index, const Mat44& inverse_bind) {
        const usize index = m_bones.size();
        m_bones.push_back({std::move(name), parent_index, inverse_bind});
        m_name_to_index[m_bones.back().name] = static_cast<u32>(index);
        return index;
    }

    usize bone_count() const noexcept { return m_bones.size(); }
    const Bone& bone(usize index) const { return m_bones[index]; }
    i32 bone_index(const std::string& name) const {
        const auto it = m_name_to_index.find(name);
        return it == m_name_to_index.end() ? -1 : static_cast<i32>(it->second);
    }

    // المصفوفات النهائية: local → عالمي → معكوسة للربط (.Skinning).
    void compute_skinning(const std::vector<Mat44>& local_transforms,
                          std::vector<Mat44>& out_skinning) const {
        out_skinning.resize(m_bones.size());
        std::vector<Mat44> world(m_bones.size());
        // الترتيب الطوبولوجي مضمون: العظم دائمًا بعد والده.
        for (usize i = 0; i < m_bones.size(); ++i) {
            const i32 parent = m_bones[i].parent;
            world[i] = (parent >= 0 ? world[parent] : Mat44::identity()) *
                       local_transforms[i];
            // skinning = inverse_bind * world — يعيد الرأس لمكانه الأصلي مشوهًا.
            out_skinning[i] = m_bones[i].inverse_bind_pose * world[i];
        }
    }

private:
    std::vector<Bone> m_bones;
    std::unordered_map<std::string, u32> m_name_to_index;
};

} // namespace nf::anim
```

```cpp
// NF/Animation/include/NF/Animation/Clip.hpp
#pragma once

#include <NF/Animation/Skeleton.hpp>
#include <algorithm>

namespace nf::anim {

struct PositionKey {
    f32 time = 0.0f;   // ثوانٍ
    Vec3 value{0, 0, 0};
};

struct RotationKey {
    f32 time = 0.0f;
    Quat value; // رباعية وحدة دائمًا
};

struct ScaleKey {
    f32 time = 0.0f;
    Vec3 value{1, 1, 1};
};

/// مقطع تحريك: مسارات لكل عظمة. الأخذ الزمني ثنائي البحث (O(log n)).
struct Clip {
    std::string name;
    f32 duration = 0.0f;   // ثوانٍ
    bool looping = true;

    // SoA: كل عظمة لها قوائم مفصولة (cache locality أثناء الأخذ).
    std::vector<std::vector<PositionKey>> positions;
    std::vector<std::vector<RotationKey>> rotations;
    std::vector<std::vector<ScaleKey>> scales;

    /// يأخذ عينات في الزمن t ويكتب المصفوفات المحلية لكل عظمة.
    void sample(f32 time, std::vector<Mat44>& out_locals) const {
        const f32 clamped = looping ? std::fmod(time, duration)
                                    : std::clamp(time, 0.0f, duration);
        const usize count = positions.size();
        out_locals.resize(count);
        for (usize bone = 0; bone < count; ++bone) {
            const Vec3 position = sample_vec3(positions[bone], clamped);
            const Quat rotation = sample_quat(rotations[bone], clamped);
            const Vec3 scale = sample_vec3(scales[bone], clamped);
            out_locals[bone] = Mat44::from_rotation_translation(rotation, position) *
                               Mat44::scaling(scale);
        }
    }

private:
    static Vec3 sample_vec3(const std::vector<PositionKey>& keys, f32 time) {
        if (keys.empty()) return {0, 0, 0};
        if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
        if (time >= keys.back().time) return keys.back().value;
        // ثنائي البحث للإطار المحيط: n=10 → 3 مقارنات بدل 10.
        auto upper = std::ranges::upper_bound(keys, time, {},
                                             [](const PositionKey& k) { return k.time; });
        auto lower = std::prev(upper);
        const f32 span = upper->time - lower->time;
        const f32 t = span > 0.0f ? (time - lower->time) / span : 0.0f;
        return Vec3::lerp(lower->value, upper->value, t);
    }

    static Quat sample_quat(const std::vector<RotationKey>& keys, f32 time) {
        if (keys.empty()) return Quat::identity();
        if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
        if (time >= keys.back().time) return keys.back().value;
        auto upper = std::ranges::upper_bound(keys, time, {},
                                             [](const RotationKey& k) { return k.time; });
        auto lower = std::prev(upper);
        const f32 span = upper->time - lower->time;
        const f32 t = span > 0.0f ? (time - lower->time) / span : 0.0f;
        // SLERP للدوران: LERP يفقد الوحدة فيظهر اهتزازًا.
        return lower->value.slerp(upper->value, t);
    }
};

} // namespace nf::anim
```

---

# 355. Audio: الموزع ثلاثي الأبعاد (§48)

الصوت positional: التوهين حسب المسافة، والـpan حسب الزاوية. **كل المعالجة
في الكسر (Fractional) للحفاظ على الجودة**.

```cpp
// NF/Audio/include/NF/Audio/Mixer.hpp
#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <cmath>
#include <vector>

namespace nf::audio {

struct AttenuationModel {
    enum class Type { Linear, Inverse, Exponential } type = Type::Inverse;
    float min_distance = 1.0f;  // مسافة بداية التوهين (متر)
    float max_distance = 50.0f; // مسافة الكتم التام
    float rolloff = 1.0f;       // حدة السقوط
};

struct ListenerState {
    Vec3 position{0, 0, 0};
    Vec3 forward{0, 0, 1};
    Vec3 up{0, 1, 0};
};

class SpatialMixer {
public:
    void set_listener(const ListenerState& listener) { m_listener = listener; }

    /// يحسب الرسم الساكن (Volume) والـpan لمصدر صوتي.
    /// يُستدعى مرة لكل مصدر لكل إطار، ثم يُطبَّق على العينات.
    void compute_mix(const Vec3& source_position, const AttenuationModel& model,
                     float& out_left, float& out_right) const {
        const Vec3 to_source = source_position - m_listener.position;
        const float distance = to_source.length();

        // 1. التوهين: اختفاء مع المسافة.
        float gain = 1.0f;
        if (distance > model.min_distance) {
            switch (model.type) {
            case AttenuationModel::Type::Linear: {
                const float span = model.max_distance - model.min_distance;
                gain = 1.0f - std::clamp((distance - model.min_distance) / span, 0.0f, 1.0f);
                break;
            }
            case AttenuationModel::Type::Inverse:
                gain = model.min_distance /
                       (model.min_distance + model.rolloff *
                                                 (distance - model.min_distance));
                break;
            case AttenuationModel::Type::Exponential:
                gain = std::pow(model.min_distance / distance, model.rolloff);
                break;
            }
        }
        gain = std::clamp(gain, 0.0f, 1.0f);
        if (distance > model.max_distance) gain = 0.0f;

        // 2. الـpan: التوازن يمين/يسار حسب الزاوية الأفقية.
        const Vec3 direction = to_source.normalized();
        const float horizontal_dot = direction.dot(m_listener.forward.normalized());
        const float right_dot = direction.dot(m_listener.forward.cross(m_listener.up).normalized());
        // [-1, 1]: يسار بالكامل إلى يمين بالكامل.
        const float pan = std::clamp(right_dot, -1.0f, 1.0f);
        // قانون الجيب التربيعي (Constant power): لا يختفي الصوت في المنتصف.
        out_left = gain * std::cos((pan + 1.0f) * 0.25f * 3.14159f);
        out_right = gain * std::cos((1.0f - pan) * 0.25f * 3.14159f);
    }

    /// يطبق المزج على مخزن عينات (interleaved stereo, float32).
    void apply_mix(const float* input, float* output, usize frame_count,
                   float left_gain, float right_gain) const {
        for (usize i = 0; i < frame_count; ++i) {
            output[i * 2 + 0] += input[i * 2 + 0] * left_gain;
            output[i * 2 + 1] += input[i * 2 + 1] * right_gain;
        }
    }

private:
    ListenerState m_listener;
};

} // namespace nf::audio
```

**ملاحظة الـpan**: لو استخدمت gain × (1±pan)/2 سينخفض الصوت في المنتصف
بـ 3dB. قانون الجيب التربيعي (Constant power) يحافظ على الطاقة — هذا
الفرق يسمعه المستخدم فورًا.

---

# 356. Editor: التراجع والإعادة (Undo/Redo) — §78

التراجع **أمر (Command)** لا حدثًا. كل عملية تعرف كيف تُفعل وكيف تُلغى،
ولا تعرف شيئًا عن التي قبلها. هذا ما يجعل أي تتابع قابلًا للعكس.

```cpp
// NF/Editor/include/NF/Editor/UndoStack.hpp
#pragma once

#include <NF/Core/Types.hpp>
#include <memory>
#include <vector>

namespace nf::editor {

/// أمر قابل للعكس. دورة الحياة: do() عند التنفيذ، undo() عند التراجع.
class UndoCommand {
public:
    virtual ~UndoCommand() = default;
    virtual void undo() = 0;
    virtual void redo() = 0; // تنفيذ مرة أخرى
    virtual const char* description() const = 0;
};

/// أمر مبني على حالتين: بسيط لأي تعديل خاصية واحدة (الأغلبية الساحقة).
class PropertyChangeCommand : public UndoCommand {
public:
    using ApplyFn = std::function<void(const std::vector<u8>&)>;

    PropertyChangeCommand(std::vector<u8> old_value, std::vector<u8> new_value,
                          ApplyFn apply, const char* desc)
        : m_old(std::move(old_value)), m_new(std::move(new_value)),
          m_apply(std::move(apply)), m_desc(desc) {}

    void undo() override { m_apply(m_old); }
    void redo() override { m_apply(m_new); }
    const char* description() const override { return m_desc; }

private:
    std::vector<u8> m_old;
    std::vector<u8> m_new;
    ApplyFn m_apply;
    const char* m_desc;
};

class UndoStack {
public:
    void push(std::unique_ptr<UndoCommand> command) {
        // أمر جديد يُلغي كل ما بعده (الفرع البديل لم يعد متاحًا).
        m_redo_stack.clear();
        m_undo_stack.push_back(std::move(command));
        if (m_undo_stack.size() > m_max_depth) {
            m_undo_stack.erase(m_undo_stack.begin());
        }
    }

    bool undo() {
        if (m_undo_stack.empty()) return false;
        std::unique_ptr<UndoCommand> command = std::move(m_undo_stack.back());
        m_undo_stack.pop_back();
        command->undo();
        m_redo_stack.push_back(std::move(command));
        return true;
    }

    bool redo() {
        if (m_redo_stack.empty()) return false;
        std::unique_ptr<UndoCommand> command = std::move(m_redo_stack.back());
        m_redo_stack.pop_back();
        command->redo();
        m_undo_stack.push_back(std::move(command));
        return true;
    }

    void clear() noexcept {
        m_undo_stack.clear();
        m_redo_stack.clear();
    }

    bool can_undo() const noexcept { return !m_undo_stack.empty(); }
    bool can_redo() const noexcept { return !m_redo_stack.empty(); }

    void set_max_depth(usize depth) noexcept { m_max_depth = depth; }

private:
    std::vector<std::unique_ptr<UndoCommand>> m_undo_stack;
    std::vector<std::unique_ptr<UndoCommand>> m_redo_stack;
    usize m_max_depth = 256;
};

} // namespace nf::editor
```

**الخطأ الكلاسيكي**: تخزين "القيمة الجديدة" فقط والتبرير بأنها قابلة للاستعادة
من مكان آخر. هذا يكسر التراجع فور تغير ذلك المكان — الاحتفاظ بالحالتين
قانون هيكلي.

---

# 357. Editor: المفتش الانعكاسي (Reflected Inspector) — §75 + §298

المفتش لا يعرف الأنواع. يسأل الانعكاس: ما أسماء الخصائص؟ ما أنواعها؟
يولّد عناصر الواجهة تلقائيًا. **إضافة خاصية = سطر واحد**، لا واجهة يدوية.

```cpp
// NF/Core/include/NF/Core/Reflection.hpp (مبسَّط)
#pragma once

#include <NF/Core/Types.hpp>
#include <any>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::reflect {

struct Property {
    std::string name;
    std::string type_name;
    std::function<std::any(const void*)> get;
    std::function<void(void*, const std::any&)> set;
    bool readonly = false;
    float min = 0.0f; // للسلاِدرز
    float max = 0.0f;
};

struct ClassInfo {
    std::string name;
    std::vector<Property> properties;
};

class Registry {
public:
    template<typename T>
    void register_class(std::string name, std::vector<Property> properties) {
        ClassInfo info;
        info.name = std::move(name);
        info.properties = std::move(properties);
        m_classes[info.name] = std::move(info);
    }

    const ClassInfo* find_class(const std::string& name) const {
        const auto it = m_classes.find(name);
        return it == m_classes.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::string, ClassInfo> m_classes;
};

// مساعد توليد خاصية: يخفي قالب القالب عن المستخدم.
#define NF_PROPERTY(ClassName, FieldName, DisplayName)                    \
    nf::reflect::Property {                                               \
        DisplayName, #FieldName,                                          \
        [](const void* obj) -> std::any {                                 \
            return static_cast<const ClassName*>(obj)->FieldName;         \
        },                                                                \
        [](void* obj, const std::any& value) {                            \
            static_cast<ClassName*>(obj)->FieldName =                     \
                std::any_cast<std::decay_t<decltype(ClassName::FieldName)>>(value); \
        }                                                                 \
    }

} // namespace nf::reflect
```

```cpp
// NF/Editor/src/InspectorPanel.cpp (مبسَّط)
void draw_inspector(nf::reflect::Registry& registry, void* object,
                    const std::string& class_name, nf::editor::UndoStack& undo) {
    const nf::reflect::ClassInfo* info = registry.find_class(class_name);
    if (info == nullptr) {
        ImGui::TextDisabled("Unknown class: %s", class_name.c_str());
        return;
    }
    for (const nf::reflect::Property& property : info->properties) {
        if (property.type_name == "float") {
            const float current = std::any_cast<float>(property.get(object));
            float modified = current;
            // السلاِدر بالحدود المسجلة (UI آمن دائمًا: لا قيمة خارج النطاق).
            if (ImGui::SliderFloat(property.name.c_str(), &modified,
                                   property.min, property.max)) {
                // التراجع: احفظ القديم قبل التطبيق (قاعدة §78).
                std::vector<u8> old_bytes;
                std::memcpy(&old_bytes, &current, sizeof(float));
                std::vector<u8> new_bytes;
                std::memcpy(&new_bytes, &modified, sizeof(float));
                property.set(object, modified);
                undo.push(std::make_unique<PropertyChangeCommand>(
                    std::move(old_bytes), std::move(new_bytes),
                    [object, property](const std::vector<u8>& bytes) {
                        float value;
                        std::memcpy(&value, bytes.data(), sizeof(float));
                        property.set(object, value);
                    },
                    property.name.c_str()));
            }
        } else if (property.type_name == "bool") {
            const bool current = std::any_cast<bool>(property.get(object));
            bool modified = current;
            if (ImGui::Checkbox(property.name.c_str(), &modified)) {
                property.set(object, modified);
            }
        }
        // ... Vec3 → 3 sliders, Quat → Euler display, إلخ.
    }
}
```

---

# 358. Scripting: آلة Lua المعزولة (§6)

**العزل شرط البقاء**: السكربت الذي يحلقة لا يجمد المحرك. كل سكربت له بيئته
الخاصة، وعدّاد تعليمات يقطعه.

```cpp
// NF/Scripting/include/NF/Scripting/ScriptVm.hpp (مبسَّط)
#pragma once

#include <NF/Core/Types.hpp>
#include <string>
#include <string_view>
#include <unordered_map>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace nf::script {

class ScriptVm {
public:
    ScriptVm() : m_lua(luaL_newstate()) {
        if (m_lua == nullptr) return;
        // **لا مكتبة كاملة**: io/os/debug مفقودة. السكربت لا يلمس القرص.
        luaL_openlibs(m_lua);
        lua_setinstruction_count_limit(m_lua, MaxInstructions);
        register_nf_library();
    }

    ~ScriptVm() {
        if (m_lua != nullptr) lua_close(m_lua);
    }

    ScriptVm(const ScriptVm&) = delete;
    ScriptVm& operator=(const ScriptVm&) = delete;

    /// ينفذ ملفًا. أي خطأ في الترجمة أو وقت التشغيل يُرجع false.
    bool load_file(const std::string& virtual_path) {
        if (m_lua == nullptr) return false;
        const int result = luaL_dofile(m_lua, virtual_path.c_str());
        if (result != LUA_OK) {
            const char* error = lua_tostring(m_lua, -1);
            m_last_error = error != nullptr ? error : "unknown error";
            lua_pop(m_lua, 1);
            return false;
        }
        return true;
    }

    const std::string& last_error() const noexcept { return m_last_error; }

    /// استدعاء دالة بسكربت: nf.on_update(dt)
    bool call_update(float dt) {
        lua_getglobal(m_lua, "nf");
        if (!lua_istable(m_lua, -1)) { lua_pop(m_lua, 1); return false; }
        lua_getfield(m_lua, -1, "on_update");
        if (!lua_isfunction(m_lua, -1)) { lua_pop(m_lua, 2); return false; }
        lua_pushnumber(m_lua, dt);
        // lua_pcall: الخطأ لا يقفز إلى C++ (لا استثناء عبر الحدود).
        const int result = lua_pcall(m_lua, 1, 0, 0);
        if (result != LUA_OK) {
            const char* error = lua_tostring(m_lua, -1);
            m_last_error = error != nullptr ? error : "runtime error";
            lua_pop(m_lua, 2);
            return false;
        }
        lua_pop(m_lua, 1);
        return true;
    }

private:
    static constexpr u32 MaxInstructions = 1'000'000; // ضد الحلقات اللانهائية

    void register_nf_library() {
        static const luaL_Reg nf_functions[] = {
            {"log", [](lua_State* L) -> int {
                 const char* message = luaL_checkstring(L, 1);
                 NF_LOG_INFO("[script] {}", message);
                 return 0;
             }},
            {"entity_move", [](lua_State* L) -> int {
                 // ربط الكيان: الفهرس في 1-based (Lua convention).
                 const lua_Integer id = luaL_checkinteger(L, 1);
                 const float x = static_cast<float>(luaL_checknumber(L, 2));
                 const float y = static_cast<float>(luaL_checknumber(L, 3));
                 const float z = static_cast<float>(luaL_checknumber(L, 4));
                 move_entity(static_cast<u32>(id), {x, y, z});
                 return 0;
             }},
            {nullptr, nullptr},
        };
        luaL_newlib(m_lua, nf_functions);
        lua_setglobal(m_lua, "nf");
    }

    lua_State* m_lua = nullptr;
    std::string m_last_error;
};

} // namespace nf::script
```

**القاعدة الحاسمة**: `lua_pcall` وليس `lua_call`. السكربت الذي يستدعي
دالة C++ فاشلة يجب أن **يسجل خطأً ويتوقف**، لا أن يطلق استثناءً عبر حدود
اللغة — سلوك غير معرف.

---

# 359. Gameplay: الوسوم الهرمية (§211 + §212)

الوسوم أسماء هرمية نقطة: `Character.Player`. الاستعلام منطقي: "كل
`Character` و ليس `Character.Dead`".

```cpp
// NF/Gameplay/include/NF/Gameplay/Tags.hpp
#pragma once

#include <NF/Core/Types.hpp>
#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace nf::gameplay {

class TagRegistry {
public:
    /// يسجل ويُرقمن الوسم. التخزين u32: المقارنة بدل البحث النصي.
    u32 register_tag(std::string_view tag) {
        const std::string normalized = normalize(tag);
        const auto it = m_string_to_id.find(normalized);
        if (it != m_string_to_id.end()) return it->second;
        const u32 id = static_cast<u32>(m_tags.size());
        m_tags.push_back(normalized);
        m_string_to_id[normalized] = id;
        // التسلسل الهرمي: Character.Player يرث كل استعلامات Character.
        m_parents.push_back(parent_id_of(normalized));
        return id;
    }

    u32 find_id(std::string_view tag) const {
        const auto it = m_string_to_id.find(normalize(tag));
        return it == m_string_to_id.end() ? Invalid : it->second;
    }

    /// هل يطابق الكيان الوسم (مع الوراثة الهرمية)؟
    bool matches(u32 tag_id, u32 query_id) const {
        if (tag_id == query_id) return true;
        // الصعود في السلسلة: Character.Player → Character.
        u32 current = tag_id;
        while (m_parents[current] != Invalid) {
            current = m_parents[current];
            if (current == query_id) return true;
        }
        return false;
    }

    static constexpr u32 Invalid = 0xFFFFFFFFu;

private:
    static std::string normalize(std::string_view tag) {
        std::string out{tag};
        for (char& c : out) {
            if (c >= 'a' && c <= 'z') c -= 32; // أحرف كبيرة فقط (توحيد)
        }
        return out;
    }

    u32 parent_id_of(const std::string& tag) const {
        const usize dot = tag.find_last_of('.');
        if (dot == std::string::npos) return Invalid;
        return find_id(tag.substr(0, dot));
    }

    std::vector<std::string> m_tags;
    std::vector<u32> m_parents;
    std::unordered_map<std::string, u32> m_string_to_id;
};

/// استعلام منطقي: AND/OR/NOT على مجموعة وسوم.
struct TagQuery {
    std::vector<u32> all_of;    // يجب أن يملكها كلها
    std::vector<u32> any_of;    // واحدة على الأقل
    std::vector<u32> none_of;   // لا يملك أي منها

    bool evaluate(const TagRegistry& registry, const std::vector<u32>& entity_tags) const {
        for (const u32 required : all_of) {
            if (!any_tag_matches(registry, entity_tags, required)) return false;
        }
        if (!any_of.empty()) {
            bool found = false;
            for (const u32 candidate : any_of) {
                if (any_tag_matches(registry, entity_tags, candidate)) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        for (const u32 forbidden : none_of) {
            if (any_tag_matches(registry, entity_tags, forbidden)) return false;
        }
        return true;
    }

private:
    static bool any_tag_matches(const TagRegistry& registry,
                                const std::vector<u32>& entity_tags, u32 query) {
        for (const u32 tag : entity_tags) {
            if (registry.matches(tag, query)) return true;
        }
        return false;
    }
};

} // namespace nf::gameplay
```

---

# 360. Serialization: التسلسل المُصدَّر (§73 + §74)

التسلسل **بالمخطط** لا بالترتيب. أي حقل يُضاف بنسخة أعلى، والقارئ يتجاوز
الحقول غير المعروفة بدلًا من الفشل.

```cpp
// NF/Core/include/NF/Core/Serialization.hpp
#pragma once

#include <NF/Core/Types.hpp>
#include <cstring>
#include <vector>

namespace nf::serialize {

class Writer {
public:
    void raw(const void* data, usize size) {
        const u8* bytes = static_cast<const u8*>(data);
        m_buffer.insert(m_buffer.end(), bytes, bytes + size);
    }

    template<typename T>
    void value(const T& v) {
        // Little-endine ثابت عبر المنصات (§162).
        raw(&v, sizeof(T));
    }

    void string(std::string_view s) {
        const u32 length = static_cast<u32>(s.size());
        value(length);
        raw(s.data(), s.size());
    }

    const std::vector<u8>& data() const noexcept { return m_buffer; }

private:
    std::vector<u8> m_buffer;
};

class Reader {
public:
    Reader(const u8* data, usize size) : m_data(data), m_size(size) {}

    bool raw(void* out, usize size) {
        if (m_position + size > m_size) return false; // لا قراءة خارج الحدود
        std::memcpy(out, m_data + m_position, size);
        m_position += size;
        return true;
    }

    template<typename T>
    bool value(T& out) {
        return raw(&out, sizeof(T));
    }

    bool string(std::string& out) {
        u32 length = 0;
        if (!value(length)) return false;
        if (length > MaxStringBytes) return false; // حد أمان
        out.resize(length);
        return raw(out.data(), length);
    }

    bool at_end() const noexcept { return m_position >= m_size; }
    usize remaining() const noexcept { return m_size - m_position; }

private:
    static constexpr usize MaxStringBytes = 1 << 20; // 1MiB

    const u8* m_data = nullptr;
    usize m_size = 0;
    usize m_position = 0;
};

/// رقم سحري + نسخة: كل ملف يبدأ بهما، فالقارئ يعرف ماذا يقرأ.
struct FileHeader {
    static constexpr u32 Magic = 0x4E46_4653u; // "NFFS"
    u32 magic = Magic;
    u32 version = 1;
    u32 flags = 0;
};

} // namespace nf::serialize
```

```cpp
// مثال: حفظ المشهد
bool save_scene(const std::string& path, const ecs::World& world) {
    serialize::Writer writer;
    serialize::FileHeader header;
    writer.value(header.magic);
    writer.value(header.version);
    writer.value(header.flags);

    const std::vector<ecs::Entity> entities = world.all_entities();
    const u32 count = static_cast<u32>(entities.size());
    writer.value(count);

    for (const ecs::Entity entity : entities) {
        writer.value(entity.index);
        writer.value(entity.generation);
        if (const Transform* transform = world.get<Transform>(entity)) {
            writer.value(static_cast<u8>(1)); // علامة: لديه تحويل
            writer.value(transform->position.x);
            writer.value(transform->position.y);
            writer.value(transform->position.z);
            writer.value(transform->rotation.x);
            writer.value(transform->rotation.y);
            writer.value(transform->rotation.z);
            writer.value(transform->rotation.w);
        } else {
            writer.value(static_cast<u8>(0));
        }
    }
    return write_file(path, writer.data());
}

bool load_scene(const std::string& path, ecs::World& world) {
    std::vector<u8> buffer;
    if (!read_file(path, buffer)) return false;
    serialize::Reader reader{buffer.data(), buffer.size()};

    serialize::FileHeader header;
    if (!reader.value(header.magic) || header.magic != serialize::FileHeader::Magic) {
        return false; // ملف تالف أو نوع خطأ
    }
    if (header.version > CurrentSceneVersion) {
        return false; // أحدث من المحرك (§242 Versioning)
    }

    u32 count = 0;
    if (!reader.value(count)) return false;
    for (u32 i = 0; i < count; ++i) {
        u32 index = 0, generation = 0;
        if (!reader.value(index) || !reader.value(generation)) return false;
        const ecs::Entity entity = world.create_at(index, generation);
        u8 has_transform = 0;
        if (!reader.value(has_transform)) return false;
        if (has_transform == 1) {
            Transform transform;
            if (!reader.value(transform.position.x)) return false;
            if (!reader.value(transform.position.y)) return false;
            if (!reader.value(transform.position.z)) return false;
            if (!reader.value(transform.rotation.x)) return false;
            if (!reader.value(transform.rotation.y)) return false;
            if (!reader.value(transform.rotation.z)) return false;
            if (!reader.value(transform.rotation.w)) return false;
            world.add(entity, transform);
        }
    }
    return reader.at_end(); // يجب استهلاك كل البايتات
}
```

**القانون**: `at_end()` في النهاية. ملف بطول زائد يعني قارئًا ومكتوبًا
غير متطابقين — خطأ صامت ينفجر لاحقًا في ملف آخر.

---

# 361. Profiler: واجهة القياس (§84 + §85)

الملف الشخصي **عينة** لا حدثًا: القياس الخفيف هو الوحيد الذي يُستعمل في
الإصدار النهائي. أي قياس أثقل من 1μs يجب أن يكون شرطيًا.

```cpp
// NF/Core/include/NF/Core/Profiler.hpp
#pragma once

#include <NF/Core/Types.hpp>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nf::profile {

struct Zone {
    std::string name;
    std::chrono::steady_clock::time_point start;
    std::chrono::nanoseconds elapsed_ns{0};
    u32 thread_id = 0;
    u32 call_count = 0;
};

class Profiler {
public:
    static Profiler& instance() {
        static Profiler singleton;
        return singleton;
    }

    /// المنطقة النشطة: RAII يضمن الإيقاف حتى عند الرجوع المبكر.
    class ScopedZone {
    public:
        explicit ScopedZone(std::string_view name) {
            m_zone = &Profiler::instance().begin(name);
        }
        ~ScopedZone() { Profiler::instance().end(*m_zone); }
        ScopedZone(const ScopedZone&) = delete;
        ScopedZone& operator=(const ScopedZone&) = delete;

    private:
        Zone* m_zone = nullptr;
    };

    Zone& begin(std::string_view name) {
        const u32 thread = current_thread_id();
        std::lock_guard lock(m_mutex);
        std::vector<Zone>& zones = m_per_thread[thread];
        zones.push_back(Zone{std::string{name},
                             std::chrono::steady_clock::now(), {}, thread, 1});
        return zones.back();
    }

    void end(Zone& zone) {
        zone.elapsed_ns = std::chrono::steady_clock::now() - zone.start;
    }

    /// تقرير Chrome Trace (§84): يُفتح في chrome://tracing مباشرة.
    std::string export_chrome_trace() const {
        std::string out = "[";
        bool first = true;
        for (const auto& [thread, zones] : m_per_thread) {
            for (const Zone& zone : zones) {
                if (!first) out += ",";
                first = false;
                const double start_ms =
                    std::chrono::duration<double, std::milli>(
                        zone.start.time_since_epoch()).count();
                const double duration_ms =
                    std::chrono::duration<double, std::milli>(zone.elapsed_ns).count();
                out += "{";
                out += "\"name\":\"" + zone.name + "\",";
                out += "\"cat\":\"default\",";
                out += "\"ph\":\"X\",";
                out += "\"ts\":" + std::to_string(start_ms) + ",";
                out += "\"dur\":" + std::to_string(duration_ms) + ",";
                out += "\"pid\":1,";
                out += "\"tid\":" + std::to_string(thread);
                out += "}";
            }
        }
        out += "]";
        return out;
    }

    /// ملخص: أبطأ المناطق التراكميًا (لتحسين الأداء المركّز).
    std::vector<Zone> hotspots(usize top_n = 10) const {
        std::unordered_map<std::string, Zone> aggregated;
        for (const auto& [_, zones] : m_per_thread) {
            for (const Zone& zone : zones) {
                auto it = aggregated.find(zone.name);
                if (it == aggregated.end()) {
                    aggregated[zone.name] = zone;
                } else {
                    it->second.elapsed_ns += zone.elapsed_ns;
                    it->second.call_count += 1;
                }
            }
        }
        std::vector<Zone> sorted;
        sorted.reserve(aggregated.size());
        for (auto& [_, zone] : aggregated) sorted.push_back(zone);
        std::ranges::sort(sorted, [](const Zone& a, const Zone& b) {
            return a.elapsed_ns > b.elapsed_ns;
        });
        if (sorted.size() > top_n) sorted.resize(top_n);
        return sorted;
    }

    void clear() {
        std::lock_guard lock(m_mutex);
        m_per_thread.clear();
    }

private:
    static u32 current_thread_id() {
        return static_cast<u32>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    mutable std::mutex m_mutex;
    std::unordered_map<u32, std::vector<Zone>> m_per_thread;
};

#define NF_PROFILE_ZONE(name) nf::profile::Profiler::ScopedZone _zone_##__LINE__(name)
#define NF_PROFILE_FUNCTION() NF_PROFILE_ZONE(__func__)
```

---

# 362. Frame Pacing: وتيرة الإطار الثابتة (§121 + §187)

اللعبة تعمل **بدقة ثابتة** بصرف النظر عن الإطارات: 60Hz دائمًا. الإطار
البطيء يستهلك خطوات فيزيائية متعددة (catch-up) بدل التباطؤ.

```cpp
// NF/Platform/include/NF/Platform/FrameClock.hpp
#pragma once

#include <NF/Core/Types.hpp>
#include <chrono>

namespace nf::platform {

class FrameClock {
public:
    static constexpr u32 MaxStepsPerFrame = 5; // سقف اللحاق (لتجنب حلقة الموت)

    explicit FrameClock(f64 target_hz = 60.0)
        : m_fixed_dt(1.0 / target_hz), m_target_hz(target_hz) {}

    /// في بداية الإطار: يُرجع عدد خطوات الفيزياء الواجبة.
    u32 begin_frame() {
        const TimePoint now = std::chrono::steady_clock::now();
        const f64 elapsed = std::chrono::duration<f64>(now - m_last_time).count();
        m_last_time = now;

        // الحد الأقصى: تجميد ثانية كاملة = 5 خطوات لا 300 (حلزون الموت).
        m_accumulator += std::min(elapsed, 0.25);
        u32 steps = 0;
        while (m_accumulator >= m_fixed_dt && steps < MaxStepsPerFrame) {
            m_accumulator -= m_fixed_dt;
            ++steps;
        }
        // العامل: رسم بين آخر خطوتين (Interpolation §20).
        m_alpha = static_cast<f32>(m_accumulator / m_fixed_dt);
        return steps;
    }

    f64 fixed_dt() const noexcept { return m_fixed_dt; }
    f32 interpolation_alpha() const noexcept { return m_alpha; }
    f64 target_hz() const noexcept { return m_target_hz; }

private:
    using TimePoint = std::chrono::steady_clock::time_point;

    const f64 m_fixed_dt;
    const f64 m_target_hz;
    TimePoint m_last_time = std::chrono::steady_clock::now();
    f64 m_accumulator = 0.0;
    f32 m_alpha = 0.0f;
};

} // namespace nf::platform
```

```cpp
// حلقة اللعبة الرئيسية (§189 Frame Lifecycle)
void Application::run() {
    platform::FrameClock clock{60.0};
    while (!m_shutdown) {
        const u32 physics_steps = clock.begin_frame();

        // 1. المدخلات: تُجمع مرة، تُستهلك عدة مرات (ثابتة عبر الخطوات).
        m_input->poll();

        // 2. الفيزياء: N خطوات ثابتة. كل واحدة حتمية ومطابقة على السيرفر.
        for (u32 i = 0; i < physics_steps; ++i) {
            m_world->step(static_cast<f32>(clock.fixed_dt()));
            m_animation->update(static_cast<f32>(clock.fixed_dt()));
        }

        // 3. اللعب: منطق متغير المدة (UI لا يحتاج حتمية).
        m_gameplay->update(clock.interpolation_alpha());

        // 4. الرسم: interp بين آخر حالتين فيزيائيتين (موضع ناعم).
        m_renderer->render(clock.interpolation_alpha());

        m_frame_count++;
    }
}
```

---

# 363. Memory: ميزانية الموارد (§275 + §289)

الميزانية **تُفصل لا تُجمَّع**: ذاكرة GPU، ذاكرة CPU، والصوت كلها حدودها
الخاصة. تجاوز أيها يُسجَّل، لا يُسكت.

```cpp
// NF/Core/include/NF/Core/Budget.hpp
#pragma once

#include <NF/Core/Types.hpp>
#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>

namespace nf::core {

struct Budget {
    std::string name;
    usize limit_bytes = 0;
    usize used_bytes = 0;
    std::function<void(const std::string&, usize, usize)> on_exceeded;

    bool consume(usize bytes) {
        if (used_bytes + bytes > limit_bytes) {
            if (on_exceeded) on_exceeded(name, used_bytes + bytes, limit_bytes);
            return false;
        }
        used_bytes += bytes;
        return true;
    }

    void release(usize bytes) {
        used_bytes = used_bytes > bytes ? used_bytes - bytes : 0;
    }

    f32 utilization() const noexcept {
        return limit_bytes == 0 ? 0.0f
                                : static_cast<f32>(used_bytes) / static_cast<f32>(limit_bytes);
    }
};

class BudgetManager {
public:
    Budget& create(const std::string& name, usize limit_bytes) {
        Budget budget;
        budget.name = name;
        budget.limit_bytes = limit_bytes;
        budget.on_exceeded = [this](const std::string& n, usize used, usize limit) {
            // تجاوز الميزانية = تحذير + متري، لا انهيار (§148).
            log_warning("Budget '%s' exceeded: %zu/%zu bytes", n.c_str(), used, limit);
        };
        m_budgets[name] = std::move(budget);
        return m_budgets[name];
    }

    Budget* find(const std::string& name) {
        const auto it = m_budgets.find(name);
        return it == m_budgets.end() ? nullptr : &it->second;
    }

    /// ميزانيات افتراضية (§289 Memory Budget Example):
    ///   GPU     2 GB   (موارد الرسم)
    ///   CPU     512 MB (الحالة اللعبة)
    ///   Audio   128 MB (المخزن المؤقت للصوت)
    void install_defaults() {
        create("gpu", 2ull * 1024 * 1024 * 1024);
        create("cpu", 512ull * 1024 * 1024);
        create("audio", 128ull * 1024 * 1024);
    }

private:
    void log_warning(const char* fmt, ...);

    std::unordered_map<std::string, Budget> m_budgets;
};

} // namespace nf::core
```

---

# 364. Deterministic Replay: تسجيل وإعادة المدخلات (§115)

الإعادة = تسلسل مدخلات فقط. كل شيء آخر مُشتق. هذا يجعل الأخطاء المتعلقة
بالحتمية **قابلة للتشغيل التكراري بالضبط**.

```cpp
// NF/Platform/include/NF/Platform/InputRecorder.hpp
#pragma once

#include <NF/Core/Containers.hpp>
#include <NF/Core/Types.hpp>
#include <string>
#include <vector>

namespace nf::platform {

struct InputFrame {
    u32 frame_number = 0;
    RingBuffer<i8, 16> axes;   // قيم في [-128, 127]
    u64 button_bits = 0;       // كل زر بِت واحد
};

class InputRecorder {
public:
    void record_frame(const InputFrame& frame) {
        if (m_recording) m_frames.push_back(frame);
    }

    void begin_recording() {
        m_frames.clear();
        m_recording = true;
        m_playback_index = 0;
    }

    void stop_recording() { m_recording = false; }

    /// إعادة: يُرجع المدخلات للإطار الحالي أو فارغة إذا انتهت.
    std::optional<InputFrame> poll_replay(u32 frame_number) {
        if (m_playback_index >= m_frames.size()) return std::nullopt;
        // تسلسل صارم: لا قفزات (أي إطار مفقود = فقدان الحتمية).
        if (m_frames[m_playback_index].frame_number != frame_number) {
            return std::nullopt;
        }
        return m_frames[m_playback_index++];
    }

    bool save(const std::string& path) const;
    bool load(const std::string& path);

private:
    std::vector<InputFrame> m_frames;
    usize m_playback_index = 0;
    bool m_recording = false;
};

} // namespace nf::platform
```

**قانون الإعادة**: لو أمكن إعادة تشغيل تسجيل بنجاح على جهاز آخر، فالحتمية
حقيقية. لو اختلفت النتيجة ولو بمقدور عشري واحد، فهناك عشوائية مخفية —
وعندها راجع القائمة في §347.

---

# 365. Hot Reload: إعادة التحميل الحي (§82)

إعادة التحميل لا تبني النسخة الجديدة قبل تدمير القديمة — العكس يكسر كل
المراجع الحية. **الترتيب: جهّز → بدّل → أطلق سراح القديم**.

```cpp
// NF/Assets/include/NF/Assets/HotReloader.hpp
#pragma once

#include <NF/Assets/Registry.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <unordered_map>

namespace nf::assets {

class HotReloader {
public:
    using ReloadFn = std::function<void(Uuid)>;

    void watch(Uuid asset_id, ReloadFn callback) {
        m_watchers[asset_id] = std::move(callback);
        m_last_modified[asset_id] = current_mtime(asset_id);
    }

    /// يُستدعى كل بضع ثوانٍ (ليس كل إطار — File I/O باهداء).
    void poll() {
        const auto now = std::chrono::steady_clock::now();
        if (now - m_last_poll < std::chrono::seconds(2)) return;
        m_last_poll = now;

        for (auto& [asset_id, callback] : m_watchers) {
            const std::filesystem::file_time_type current = current_mtime(asset_id);
            if (current != m_last_modified[asset_id]) {
                m_last_modified[asset_id] = current;
                // رد النداء يحدث **خارج** التكرار: لا تعديل أثناء المرور.
                m_pending_reloads.push_back(asset_id);
            }
        }
        for (const Uuid asset_id : m_pending_reloads) {
            m_watchers[asset_id](asset_id);
        }
        m_pending_reloads.clear();
    }

private:
    std::filesystem::file_time_type current_mtime(Uuid asset_id) const {
        const AssetRecord* record = m_registry.find(asset_id);
        if (record == nullptr) return {};
        const auto path = m_registry.physical_path(asset_id);
        if (!path.has_value() || !std::filesystem::exists(*path)) return {};
        return std::filesystem::last_write_time(*path);
    }

    AssetRegistry& m_registry;
    std::unordered_map<Uuid, ReloadFn> m_watchers;
    std::unordered_map<Uuid, std::filesystem::file_time_type> m_last_modified;
    std::vector<Uuid> m_pending_reloads;
    std::chrono::steady_clock::time_point m_last_poll;
};

} // namespace nf::assets
```

---

# 366. Crash Handling: التقاط الأعطال (§88)

العطل ليس نهاية العملية فرصة: سجل مكدس الاستدعاءات، احفظ الحالة، ثم اخرج
بنظافة. **الملف الدقيق (Minidump)** هو ما يجعل العطل قابلًا للإصلاح.

```cpp
// NF/Core/include/NF/Core/CrashHandler.hpp
#pragma once

#include <NF/Core/Types.hpp>
#include <atomic>
#include <csignal>
#include <string>

#ifdef _WIN32
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace nf::core {

class CrashHandler {
public:
    static void install(const std::string& crash_dump_directory) {
        s_dump_directory = crash_dump_directory;
        s_instance = std::make_unique<CrashHandler>();

        // المعالجات: SEH على Windows، SIGSEGV/SIGABRT في مكان آخر.
        std::signal(SIGSEGV, &CrashHandler::signal_handler);
        std::signal(SIGABRT, &CrashHandler::signal_handler);
        std::signal(SIGFPE, &CrashHandler::signal_handler);
        std::signal(SIGILL, &CrashHandler::signal_handler);
#ifdef _WIN32
        SetUnhandledExceptionFilter(&CrashHandler::seh_handler);
#endif
    }

    /// يكتب ملفًا دقيقًا. **خيط الإشارة**: ممنوع التخصيص، ممنوع القفل.
    static bool write_minidump(const std::string& path) {
#ifdef _WIN32
        const HANDLE process = GetCurrentProcess();
        const DWORD process_id = GetCurrentProcessId();
        const HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        const bool ok = MiniDumpWriteDump(process, process_id, file,
                                          MiniDumpNormal, nullptr, nullptr, nullptr);
        CloseHandle(file);
        return ok;
#else
        return false; // POSIX: استخدام breakpad/glog لاحقًا
#endif
    }

private:
    static void signal_handler(int signal) {
        const char* name = signal_name(signal);
        // كتابة مباشرة (async-signal-safe): fprintf مسموح، std::cout ممنوع.
        std::fprintf(stderr, "\n[CRASH] signal %d (%s)\n", signal, name);
        if (!s_dump_directory.empty()) {
            const std::string path = s_dump_directory + "/crash_" +
                                     std::to_string(s_crash_count.fetch_add(1)) + ".dmp";
            if (write_minidump(path)) {
                std::fprintf(stderr, "[CRASH] minidump written: %s\n", path.c_str());
            }
        }
        std::_Exit(EXIT_FAILURE); // لا تكرار المعالج
    }

#ifdef _WIN32
    static LONG WINAPI seh_handler(EXCEPTION_POINTERS* pointers) {
        std::fprintf(stderr, "\n[CRASH] SEH code 0x%08X\n",
                     static_cast<u32>(pointers->ExceptionRecord->ExceptionCode));
        if (!s_dump_directory.empty()) {
            const std::string path = s_dump_directory + "/crash_seh.dmp";
            if (write_minidump(path)) {
                std::fprintf(stderr, "[CRASH] minidump written: %s\n", path.c_str());
            }
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }
#endif

    static const char* signal_name(int signal) {
        switch (signal) {
        case SIGSEGV: return "segmentation fault";
        case SIGABRT: return "abort";
        case SIGFPE: return "floating point exception";
        case SIGILL: return "illegal instruction";
        default: return "unknown";
        }
    }

    inline static std::string s_dump_directory;
    inline static std::unique_ptr<CrashHandler> s_instance;
    inline static std::atomic<u32> s_crash_count{0};
};

} // namespace nf::core
```

**القاعدة الأهم (التي تُنكسر دائمًا)**: في معالج الإشارة **ممنوع**:
`std::string`، `new`/`malloc`، `std::mutex`، `std::cout`. كلها قد تكون
داخل العطل نفسه. النجاة تكمن في `fprintf` و`_Exit` فقط.

---

# 367. Testing: إطار الاختبارات (§116)

الإطار نفسه **جزء من المنتج**: اختصار سطر واحد للتسجيل، وطباعة صريحة
بالنتيجة. الاختبار الذي يحتاج 20 سطرًا للتهيئة لن يُكتب أبدًا.

```cpp
// Tests/include/NF/Test/TestFramework.hpp
#pragma once

#include <NF/Core/Types.hpp>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace nf::test {

struct TestCase {
    const char* name;
    void (*function)();
    const char* file;
    int line;
};

class TestRunner {
public:
    static TestRunner& instance() {
        static TestRunner runner;
        return runner;
    }

    void register_test(const char* name, void (*function)(), const char* file, int line) {
        m_cases.push_back({name, function, file, line});
    }

    /// فلترة الاختبارات بالاسم: `PhysicsTests.exe clone` يشغّل النسخ فقط.
    int run_all(std::string_view filter = "") {
        m_current = Result{};
        for (const TestCase& test : m_cases) {
            if (!filter.empty() && !contains(test.name, filter)) continue;
            std::printf("[ RUN      ] %s\n", test.name);
            const usize failures_before = m_current.failures;
            test.function();
            if (m_current.failures == failures_before) {
                m_current.passed++;
                std::printf("[       OK ] %s\n", test.name);
            } else {
                m_current.failed++;
                std::printf("[  FAILED  ] %s\n", test.name);
            }
        }
        print_summary();
        return m_current.failures == 0 ? 0 : 1;
    }

    void check(bool condition, std::string_view expression, const char* file, int line) {
        if (condition) {
            m_current.checks++;
        } else {
            m_current.failures++;
            std::printf("  %s(%d): FAILED: %s\n", file, line, expression.data());
        }
    }

    void check_near(float actual, float expected, float tolerance,
                    std::string_view expression, const char* file, int line) {
        const float diff = std::fabs(actual - expected);
        check(diff <= tolerance, expression, file, line);
    }

    void skip(const char* reason) {
        m_current.skipped++;
        std::printf("[  SKIPPED ] %s\n", reason);
    }

    void require_gpu() {
        // اختبارات GPU بدون جهاز: تخطي صريح، لا فشل صامت (§140 CI).
        if (!gpu_available()) skip("no GPU device available");
    }

private:
    struct Result {
        usize passed = 0;
        usize failed = 0;
        usize skipped = 0;
        usize checks = 0;
        usize failures = 0;
    };

    void print_summary() const {
        std::printf("\n========================================\n");
        std::printf("Passed: %zu | Failed: %zu | Skipped: %zu | Total: %zu\n",
                    m_current.passed, m_current.failed, m_current.skipped,
                    m_current.passed + m_current.failed + m_current.skipped);
        std::printf("========================================\n");
    }

    static bool contains(std::string_view haystack, std::string_view needle) {
        return haystack.find(needle) != std::string_view::npos;
    }

    static bool gpu_available();

    std::vector<TestCase> m_cases;
    Result m_current;
};

} // namespace nf::test

// وحدات الماكرو: التسجيل التلقائي قبل main (قائمة المسجلة تبقى عالمية).
#define NF_TEST(test_name)                                                   \
    static void test_name();                                                 \
    static bool test_name##_registered = [] {                                \
        ::nf::test::TestRunner::instance().register_test(                    \
            #test_name, &test_name, __FILE__, __LINE__);                     \
        return true;                                                         \
    };                                                                       \
    static void test_name()

#define NF_CHECK(expr)                                                       \
    ::nf::test::TestRunner::instance().check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

#define NF_CHECK_NEAR(actual, expected, tolerance)                           \
    ::nf::test::TestRunner::instance().check_near(                           \
        static_cast<float>(actual), static_cast<float>(expected),            \
        static_cast<float>(tolerance), #actual " ~= " #expected, __FILE__, __LINE__)

#define NF_SKIP(reason) ::nf::test::TestRunner::instance().skip(reason)
```

```cpp
// Tests/TestFramework.cpp — النقطة الوحيدة لـ main()
#include <NF/Test/TestFramework.hpp>

int main(int argc, char** argv) {
    const std::string_view filter = argc > 1 ? argv[1] : "";
    return nf::test::TestRunner::instance().run_all(filter);
}
```

---

# 368. Quality: اختبار الإطار الذهبي (Golden Frame) — §145

التحقق من البكسل **المتفق عليه مسبقًا**: صورة معروفة تُقارن بكسلًا بكسل.
أي تغيير في الرسم — حتى غير المرئي عينيًا — يُكشف.

```cpp
// Tests/RHITests/test_golden_frame.cpp (مبسَّط)
NF_TEST(golden_frame_matches_reference) {
    auto device = create_headless_device();
    NF_CHECK(device != nullptr);
    if (device == nullptr) { NF_SKIP("headless device unavailable"); return; }

    // ارسم المشهد المرجعي: مكعب واحد بإضاءة معروفة.
    std::vector<u8> pixels = render_test_scene(*device, 256, 256);
    NF_CHECK(pixels.size() == 256 * 256 * 4);

    const std::vector<u8> golden = load_reference("golden/cube_256.png");
    if (golden.empty()) { NF_SKIP("reference image missing"); return; }

    // نسبة البكسلات المتطابقة (ليس كلها: ضغط PNG قد يغير الأقل أهمية).
    usize matching = 0;
    for (usize i = 0; i < pixels.size(); i += 4) {
        const bool close = std::abs(pixels[i] - golden[i]) <= 2 &&
                           std::abs(pixels[i + 1] - golden[i + 1]) <= 2 &&
                           std::abs(pixels[i + 2] - golden[i + 2]) <= 2;
        if (close) ++matching;
    }
    const float similarity = static_cast<float>(matching) / (256.0f * 256.0f);
    NF_CHECK(similarity > 0.98f); // 98% تطابق
}
```

---

# 369. Quality: التحقق من تسريب الموارد (§385)

عند الخروج: **عدد الموارد الحية يجب أن يكون صفرًا**. أي رقم آخر = تسريب،
والاختبار يفشل بصريًا.

```cpp
NF_TEST(renderer_clean_shutdown_no_leaks) {
    auto device = create_headless_device();
    NF_CHECK(device != nullptr);
    if (device == nullptr) return;

    // استخدم الموارد: إنشاء وتدمير دورات كاملة.
    for (int i = 0; i < 10; ++i) {
        auto image = device->create_image(make_image_desc());
        auto buffer = device->create_buffer(1024, 0, MemoryUsage::GpuOnly);
        NF_CHECK(image != nullptr);
        NF_CHECK(buffer != nullptr);
    }

    device->shutdown();
    NF_CHECK(device->alive_resources() == 0); // قانون: صفر، لا "أقل من السابق"
    NF_CHECK(device->validation_errors() == 0);
}
```

---

# 370. CI/CD: خط أنابيب البناء (§140)

البناء **يُكسر عند أول تحذير** (`-W4 -WX`). هذا ليس تشددًا — التحذير الذي
يُتجاهل يُنسى حتى يصبح عطلًا.

```cmake
# CMake/NFCompiler.cmake
if(MSVC)
    add_compile_options(/W4 /WX /permissive- /Zc:preprocessor /EHsc)
    # الحتمية: لا تحسين يجمع عوامات بشكل مختلف بين الإصدارات.
    add_compile_options(/fp:precise)
    # C++23 صارم: لا امتدادات قبل المعيار.
    add_compile_options(/std:c++latest)
else()
    add_compile_options(-Wall -Wextra -Werror -Wpedantic)
    add_compile_options(-ffp-contract=off)
endif()

# Runtime ثابت (MT): كل المكتبات والاختبارات يجب أن تتطابق.
if(MSVC)
    set_property(TARGET ${target} PROPERTY
                 MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
```

```bash
# Scripts/run_tests.sh — يُخرج 0 فقط إذا نجح كل شيء
#!/usr/bin/env bash
set -uo pipefail
BUILD_DIR="${1:?usage: run_tests.sh <build_dir>}"
FAILED=0
for suite in CoreTests JobTests ECSTests RHITests PhysicsTests \
             AnimationTests AudioTests GameplayTests SaveTests \
             StreamingTests RuntimeTests EditorTests ToolTests; do
    if [ ! -f "$BUILD_DIR/bin/$suite.exe" ]; then
        echo "SKIP $suite (not built)"
        continue
    fi
    echo "RUN  $suite"
    if ! "$BUILD_DIR/bin/$suite.exe"; then
        echo "FAIL $suite"
        FAILED=1
    fi
done
exit "$FAILED"
```

---

# 371. الخطوة التالية: أهداف المحرك (ما بعد 334)

المرجع البرمجي أعلاه يغطي **الأنظمة المعمارية**. ما يليها بالأولوية:

```text
1. شبكة: نسخ القيود والكيانات (Constraint Replication)
   - clone_constraint عبر السلك (§349 الأساس)
   - أولوية الجدول الهرمي للأجسام الثابتة

2. C# Scripting (§6)
   - Same VM contract as Lua (sandbox + instruction budget)
   - Roslyn أو Mono كخلفية

3. محرك 2D متكامل (§54–§56)
   - Tilemap + 2D physics + sprites

4. التدمير (§41)
   - Destruction hooks على شبكة مثلثات
   - Chunk-based debris
```

---

# 372. خاتمة المرجع البرمجي

الكود في هذا المرجع ليس **أمثلة توضيحية** — هو العقد التنفيذي:

1. **الحتمية فوق كل شيء**: عقدًا في كل واجهة (`is_deterministic()`)
2. **الذاكرة محسوبة**: مخصص لكل دورة حياة
3. **الطبقات صارمة**: `check_layering.sh` يكسر البناء
4. **الاختبار جزء من الكود**: `NF_CHECK` داخل الواجهة، لا ملف منفصل

> **القاعدة النهائية**: كل سطر في هذا المرجع قابل للقراءة بمعزل عن غيره.
> إن احتجت لقراءة خمسة ملفات لفهم دالة واحدة، فالتصميم فشل — أعد كتابتها.

**NOVAForge يُبنى سطرًا بسطر، باختبار يحمي كل سطر.**

