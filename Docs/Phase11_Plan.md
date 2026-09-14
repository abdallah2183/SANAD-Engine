# Phase 11 — Foundation: break the layering inversions and pay down the debt

**Status**: **in progress 2026-09-14** — W1 ✅, W2 ✅, W3 ✅, W4 ✅, W5 (seam) ✅, W6 partial. All five work items landed; W6 (verify/CI/docs) is the remainder.
Suite: **534 passed / 0 failed / 1 skipped across 14 suites** (was 515/0/1 across 13).
**Design doc reference**: §2 (core principles: "clean separation between systems", "Render/Game World separation", "dedicated-server path"), §246 R6 (world streaming), §247 E5.

## Progress

| W | Item | Status |
|---|---|---|
| W1 | Break `Assets → Rendering`, unify the mesh format | **done** — AssetManager is CPU-pure (VFS+registry+parse+cache); `MeshUpload` lives in NFRendering; Runtime owns the GPU upload via `make_static_mesh`+`StaticMesh::upload`. Assets links `NFCore NFJobs` only; layering guard + Rule 0 verified. 534/0/1 held. |
| W2 | Break `Rendering → ECS/Scene` | **done** — one function moved, `Rendering` now links `NFCore NFRHI NFJobs`, guard + Rule 0 |
| W3 | Delete the dead RHI handle types | **done** — 6 structs removed, build clean |
| W4 | Benchmark thresholds | **done** — baseline measured, ceiling asserted, Rule 0 verified |
| W5 | World streaming: the seam | **done** — grid + policy + 19 tests, Rule 0 verified |
| W6 | Verify, CI, docs | **partial** — layering guard (W1+W2 rules live) and all suite guards are in CI |

## Motivation

Phases 5–10 each added a subsystem. None of them paid down the structural debt underneath, and every
one of them made it more expensive to fix. This phase adds **no new feature area** — it removes three
things that are actively wrong and lands one seam that the design document calls a "day one" principle.

The case is not aesthetic. Each item below is a trap that has already cost real time or will block a
stated project goal:

1. **`Engine/Assets` depends on `NFRendering`.** A low-level asset library (VFS, registry, cooked
   loaders) pulls in the renderer, and therefore the RHI. Consequence: anything that only needs assets
   links a graphics stack. `AssetTests` links `NFRHI NFRendering` for this reason alone.
2. **`Engine/Rendering` depends on `NFEcs` and `NFScene`.** The renderer cannot be used without the
   ECS. Consequence: no offscreen baking tool, no asset preview, and the **dedicated-server path**
   (a stated core principle) is structurally blocked, because a server has no business compiling a
   renderer to walk a scene graph.
3. **Two `.nfmesh` formats.** `assets::MeshAsset` writes `NFME` (what the cooker produces and the
   runtime consumes); `rendering::mesh_asset` writes `NFM1`. They are not interchangeable, and the
   cooker rejects `NFM1`. This has already cost time — there is a comment in
   `Tests/AssetTests/test_project_cooker.cpp` warning about it.
4. **Six dead RHI handle types** (`RHI.hpp:401-406`), zero references anywhere. The device returns
   `std::unique_ptr<Buffer>` / `Texture` / `Pipeline` instead. Two resource models, one of them
   abandoned, is how the historical SIGSEGV-class bugs start.
5. **Benchmark thresholds are unasserted.** A benchmark that reports a number nobody checks is a
   measurement, not a test.
6. **World streaming does not exist.** §246 R6 and the design document's own "from day one" framing.
   The project/mount abstraction from Phase 7 is the seam it should hang off, and it is now three
   phases old.

## Scope

### In scope (W1–W5)
1. Break `Assets → Rendering`; unify the mesh format onto `NFME`.
2. Break `Rendering → ECS/Scene`; move the extraction bridge above both.
3. Delete the dead RHI handle types.
4. Measure a benchmark baseline and assert a bound.
5. Land the world-streaming **seam** (not the full feature).

### Explicitly out of scope
- World streaming's full feature set: LOD, priority queues, a memory budget, seamless distance
  streaming. W5 lands the seam plus one working implementation, and says so.
- A dedicated-server build target. W1–W2 remove the structural blocker; actually producing a
  server binary is a later phase.
- Any new gameplay/rendering/editor feature.
- Rewriting the ECS into archetype storage (§247 E3) — a separate, much larger decision.

## Work Breakdown

### W1 — Break `Assets → Rendering`, unify the mesh format ✅ DONE

> **⚠️ Re-scoped 2026-09-14 after reconnaissance. This is the largest item in the phase, not the
> smallest.** The reviews called it a "layering inversion", which reads like a header leak. It is not.

**What the dependency actually is.** `Engine/Assets/include/NF/Assets/AssetManager.hpp` includes
`<NF/RHI/RHI.hpp>`, takes an `rhi::IGraphicsDevice&` in its constructor, and uses it in exactly two
places — both mesh GPU uploads:

```cpp
// AssetManager.cpp:92-94  (sync path)          // AssetManager.cpp:169-171  (async path)
if (m_device) {                                 if (m_device) {
    auto mesh = handle->asset->to_static_mesh(...);  auto mesh = handle->asset->to_static_mesh(...);
    if (mesh && mesh->upload(*m_device)) {           if (mesh && mesh->upload(*m_device)) {
```

`MeshHandle` carries both halves: `std::shared_ptr<MeshAsset> asset` (CPU) and
`std::shared_ptr<rendering::StaticMesh> mesh` (GPU).

**So this is a missing responsibility boundary, not a stray include.** `AssetManager` does two jobs —
load/parse/cache CPU assets, *and* upload them to the GPU — and the second one **already has a home**:
`rendering::MeshLibrary` exposes `add()` / `replace()` / `get()` / `upload_all(device)` and is already
owned by `Runtime` as `m_mesh_library`. The upload path exists twice.

**The fix, and why it is large:**

- `AssetManager` becomes pure CPU: VFS + registry + parse + cache. It loses the device, the
  `NF/RHI/RHI.hpp` include, and `MeshHandle::mesh`. `MeshAsset` keeps the bytes and the `NFME`
  (de)serialization; no `rendering::` type appears in its interface.
- The `StaticMesh` conversion moves into `Engine/Rendering` (`MeshUpload`), which may depend on
  Assets — the correct direction.
- **40 `AssetManager` construction sites** across `Engine/`, `Editor/`, `Tools/`, `Tests/`, `Samples/`
  change, because the constructor signature changes. Every consumer of `MeshHandle::mesh` changes.
- `Runtime::sync_meshes_from_assets` collapses onto `MeshLibrary`, which removes the duplication.
- `rendering::mesh_asset` (`NFM1`) is deleted; its two call sites in
  `Tests/RHITests/test_3d_renderer.cpp` switch to `assets::MeshAsset`. **One format, one writer, one
  reader.**

**Suggested sequencing.** W3 and W4 below are cheap, independent, and touch nothing W1 touches. Land
them first so the phase banks real progress before the API migration, and so W1 can be done as one
uninterrupted change rather than interleaved with it. Splitting W1 across a partially-migrated build
would leave the tree not building, which is worse than not starting it.

Acceptance (unchanged): `grep -r "NF/Rendering/" Engine/Assets/` and
`grep -r "NF/RHI/" Engine/Assets/` both return nothing; `Assets` links only `NFCore NFJobs`; the
cooker, the runtime scene load, and the RHI mesh tests all still pass.

**Verified 2026-09-14.** `check_layering.sh` reports `ok` for both Assets rules; `Assets/CMakeLists.txt`
DEPENDS is `NFCore NFJobs`; `build/verify` is clean; full suite **534/0/1** (AssetTests 45, RHITests 69,
RuntimeTests 33 all intact); Basic3D logs `StaticMesh 'Cube' uploaded (24 verts, 36 indices)` and renders
60 frames; editor headless reports `Scene geometry drawn OK` + `Validation errors: 0`. **Rule 0:**
re-adding `#include <NF/Rendering/StaticMesh.hpp>` to `MeshAsset.hpp` is caught by the guard (exit 1,
exact line printed); reverting returns to PASS. The two `.nfmesh` formats collapsed to one
(`NFME`/`assets::MeshAsset`); `rendering::mesh_asset` (`NFM1`) is deleted.

### W2 — Break `Rendering → ECS/Scene`
Also two files:

```
Engine/Rendering/include/NF/Rendering/Extraction.hpp -> <NF/ECS/ECS.hpp>
Engine/Rendering/src/Extraction.cpp                  -> <NF/Scene/Transform.hpp>
```

Extraction is a **bridge**, and a bridge belongs above both sides. The renderer should consume plain
data, not a world.

- `Rendering` keeps `RenderWorld` and gains a builder that takes plain inputs (an array of
  `(transform, mesh handle, material handle, bounds)` records).
- The ECS-walking part moves to a new `Engine/Scene/SceneExtraction.*`. `Scene` already depends on
  `ECS`, so that direction is legal, and the call site is `Runtime` — which is where "walk the game
  world, build a render world" belongs anyway.
- `Engine/Rendering/CMakeLists.txt` drops `NFEcs NFScene`.

**As built.** The inversion was one function, not the whole file: `extract_render_objects`. The two
`game::GameWorld` functions in the same header use a *rendering* type and stayed put. The ECS bridge
moved to `NF/Runtime/SceneExtraction.hpp` — the top layer, which is the only place that legitimately
knows about the ECS, the scene graph and the render world at once.

Seven call sites changed. `RHITests` and `HeadlessProbe` now link `NFScene NFEcs NFRuntime` explicitly
instead of inheriting them through `NFRendering`; that is more honest, because those tests walk a real
ECS to build a render world — they are integration tests, not renderer tests.

Acceptance met: `grep -rnE 'NF/(ECS|Scene)/|(ecs|scene)::' Engine/Rendering/` returns nothing;
`Rendering` links only `NFCore NFRHI NFJobs`; suite unchanged at the time (515/0/1).

`Scripts/check_layering.sh` enforces it, wired into CI right after the build so a reintroduced
inversion fails in seconds naming the offending line. It strips comment-only lines before matching —
the comments explaining *why* the dependency was removed necessarily name the removed types, and
flagging them would punish the documentation that stops the regression returning.

Rule 0 verified: adding `#include <NF/ECS/ECS.hpp>` back to `Rendering/Extraction.hpp` makes the guard
print the line and exit 1.

### W3 — Delete the dead RHI handle types ✅ DONE
Six structs, zero references:

```cpp
struct BufferHandle    { u32 id = u32_max; ... };   // RHI.hpp:401
struct TextureHandle   { ... };                      // 402
struct PipelineHandle  { ... };                      // 403
struct ShaderModuleHandle { ... };                   // 404
struct RenderPassHandle   { ... };                   // 405
struct FramebufferHandle  { ... };                   // 406
```

**Done 2026-09-14.** The device returns owning `unique_ptr`s (`create_buffer`, `create_texture`,
`create_pipeline`, ...), so the handles were an abandoned second resource model. All six are deleted,
replaced by a comment recording what they were and why they are gone — the next person to want an
indirection should find the reasoning rather than a blank.

Acceptance: build clean under `/W4 /WX`; full suite unchanged at 515/0/1.

### W4 — Benchmark thresholds ✅ DONE
`Tests/ECSTests/test_benchmarks.cpp` reported four numbers and asserted only that the world ended up
empty.

**Done 2026-09-14.** Baseline measured on this machine (MSVC 14.51.36231, `build/verify` — a Debug build
with `/Od`, so these are Debug numbers), nanoseconds per entity:

| | create | query | add/remove | destroy |
|---|---|---|---|---|
| 10K | 547 | 555 | 471 | 243 |
| 100K | 528 | 552 | 473 | 237 |
| 1M | 530 | 831 | 767 | 418 |

A ceiling of **10,000 ns/entity** (~12× the worst) is now asserted per phase, with the baseline table
and the reasoning in the test's own comment. The bound is deliberately loose: it catches an order-of-
magnitude regression — a query that quietly became O(n²), storage that started copying — and does not
flake on a throttled CI runner. A tighter bound would be more sensitive *and* flaky, and a flaky test
gets muted.

Rule 0 verified: setting the ceiling to 1 ns/entity makes both always-on benchmarks fail with
`benchmark regression: create took 563 ns/entity, ceiling is 1`, and the 1M case still SKIPs.

### W5 — World streaming: the seam
§246 R6, and the design document's "from day one" principle. Scoped as a **seam plus one working
implementation**, not the full feature.

- `StreamingVolumeComponent` — an AABB around an entity; the set of loaded chunks is derived from it.
- `WorldStreamer` — owns the loaded/unloaded state, resolves a chunk id to a scene path through the
  existing `project://` mount, and loads/unloads via the existing scene loader on the job system.
- `Runtime::step_streaming(dt)` — a hysteresis radius (load at R, unload at R + margin) so an entity
  oscillating on the boundary does not thrash. Observable counts: loaded chunks, load requests
  issued, unload requests issued.
- Chunks are ordinary `.nfscene` files under `content://Chunks/`. No new format.
- Explicitly deferred and documented: LOD, priority, a memory budget, and distance-based refinement.

**As built.** Split three ways so each half fails for its own reason:

* `NF/Scene/StreamingVolume.hpp` — the data and the grid: `ChunkCoord`, `StreamingVolume`,
  `chunk_at`, `chunk_center`, `distance_to_chunk`, `chunks_in_radius`, plus a `std::hash<ChunkCoord>`.
* `NF/Runtime/WorldStreamer.hpp` — the policy: range, hysteresis, per-update load cap, and the
  load/unload callback pair. It does no IO and owns no entities, which is what makes the behaviour
  testable without a filesystem or a GPU.
* `Runtime` supplies the handlers. Wiring the real chunk loader (a scene-merge into the live world) is
  the one piece still outstanding; the seam is complete and exercised.

Three decisions that are not obvious:

1. **Distance to the chunk's AABB, not to its centre.** Using the centre makes a chunk's effective
   range depend on where inside it the player stands, so a chunk can fall out of range while they are
   still standing on it.
2. **`chunk_at` floors, it does not truncate.** Truncation maps both `-0.5` and `+0.5` into cell 0, so
   the grid is asymmetric about the origin.
3. **`chunks_in_radius` returns nearest-first, tie-broken by coordinate.** This one was found by a
   failing test rather than designed: the first version sorted by coordinate, and because the load cap
   then deferred whatever sat at high x, the streamer would load the far side of the region before the
   ground under the player. Order is load-bearing wherever a cap exists.

`effective_unload_radius()` clamps an inverted band (`unload < load`) up to `load`. Without it, an
inverted band unloads the chunk that was just loaded, every frame, forever.

19 tests in `Tests/StreamingTests/test_streaming.cpp`, split between the grid geometry and the policy,
driven by a recording fake.

Rule 0 verified: making `effective_unload_radius()` return `load_radius` (hysteresis removed) fails
`streamer_hysteresis_keeps_a_chunk_between_the_two_radii` and, precisely, `streamer_does_not_thrash_on_a_boundary`
on `unload_calls != 0`.

### W6 — Verify, CI, docs
- Full Rule 0 pass on W1–W5: each fix reverted, the corresponding test confirmed to fail.
- CI: assert the new `StreamingTests` suite, and assert the layering invariants directly —
  a `grep` guard that fails the build if `Engine/Assets` ever includes `NF/Rendering/` or
  `Engine/Rendering` ever includes `NF/ECS/`/`NF/Scene/`. **That is the only thing that keeps this
  phase's work from silently regressing**, and it is cheap.
- README + this plan marked complete.

## Findings that were not in the plan

**A third duplicate type: two `Mat4`s with the same name and different layouts.** Found while moving
W2's extraction code.

| | `nf::Mat4` (`Engine/Core/include/NF/Core/Math.hpp`) | `nf::rendering::Mat4` (`Engine/Rendering/include/NF/Rendering/Camera.hpp`) |
|---|---|---|
| storage | `f32 m[4][4]` | `float m[16]` |
| convention | row-major | column-major |
| translation slot | `m[3][0..2]` | `m[12..14]` |

`RenderObject::world` is the rendering one, which is why `ro.world.m[12] = x` compiles and is correct.
The danger is not that either is wrong — it is that they are *interchangeable-looking*. A
`reinterpret_cast` or a `memcpy` between them silently transposes every transform in the scene, and
nothing in the type system objects. The rendering module also has its own `Vec3` for the same reason,
which is why `Runtime.cpp` carries a `from_rendering(const rendering::Vec3&)` converter.

This is the same class of defect as the two `.nfmesh` formats and the two euler conventions, and it
belongs in the same cleanup. It is **not** fixed here because it needs the same treatment W1 needs — one
type, one owner — and doing it half-way would be worse than leaving it named.

**A `Runtime::load_scene` that replaces the world cannot stream.** W5's seam is complete, but the
handler that actually loads a chunk needs to *merge* a scene into the live world rather than replace
it, and that operation does not exist yet. It belongs next to the scene loader, where the component
list already lives, so the merge cannot drift from the parser.

## Risks

- **W1/W2 touch the build graph.** `CMakeLists.txt` dependency changes can produce link errors that
  only appear in targets that were getting the dependency transitively. Mitigation: build every target
  after each change, not just the affected one.
- **W2 changes a hot path.** Extraction runs per frame. The move must not add an allocation per frame;
  the plain-data builder takes a caller-owned buffer, as the current one does.
- **W5 can be gold-plated.** Mitigation: the scope is the seam plus one implementation; anything that
  looks like "and also LOD" is deferred by default.
- **A grep-based layering guard is crude.** It catches includes, not transitive dependencies through
  link libraries. It is still worth having: it catches the exact regression that created the problem.

## Non-goals (revisited)

No new feature area. No archetype ECS. No dedicated-server binary. No streaming LOD or memory budget.
No second mesh format, ever.
