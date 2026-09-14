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
