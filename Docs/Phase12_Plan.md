# Phase 12 — Complete world streaming

**Status**: COMPLETE. All four items landed, verified and committed.
Suite: **552 passed / 0 failed / 1 skipped across 14 suites** (was 543/0/1).
ctest 14/14; layering guard 4/4; editor headless 45 OK / 0 FAILED /
validation 0 / alive 0; samples exit 0; `nf` end-to-end (new/build/verify/
package/run: 60 frames, physics + animation + audio live, validation 0).

Phase 11/W5 landed the seam (grid + policy + merge + Runtime wiring) and
explicitly deferred four things: async loading, load priority, a memory
budget, and distance-based LOD. A chunk load currently parses the file,
clones entities, kicks meshes and rebuilds physics **synchronously inside
`update()`** — a large chunk is a frame hitch by construction. This phase
finishes the feature without changing its policy: the WorldStreamer contract
(sync `LoadFn` → bool, retry on false) already supports non-blocking loads,
and `RenderObject::lod` has been waiting for a selector since v0.1.

## Work items

### W1 — Async chunk pipeline (no policy change)`Runtime` stages chunk loads on the JobSystem and commits on the main
thread — the ImportQueue pattern (worker touches only its staged block).
- `LoadFn` becomes non-blocking: no job → resolve path on the main thread
  and dispatch a worker (read + `load_scene_from_physical` into a temp
  Scene), return false; job finished → commit via `merge_loaded_scene`
  (the merge split out of the file wrapper), return true; job failed →
  error, false (streamer retries later per contract).
- In-flight cap (2): further wanted chunks wait. Unwanted-on-completion
  jobs are discarded, never committed.
- Workers never touch the VFS, the live world, or `this` (job + physical
  path captures only) — safe if the Runtime dies or streaming is disabled
  mid-flight. Inline fallback when the JobSystem is not initialized.
- ECS component type ids are process-global mutable state on first touch:
  the parser builds Worlds on workers now, so `enable_streaming` pre-warms
  every parsed component type on the main thread first. Without this, a
  first-touch registration on a worker races the main thread.

### W2 — Load priority (observable ordering)
Dispatch follows `wanted_chunks()` order (nearest-first, already sorted) and
skips loaded + in-flight coords. Decision helper
`WorldStreamer::next_wanted_loads(exclude, cap)` so the ordering is
unit-testable without threads (drive state with an always-failing LoadFn,
which populates nothing but exercises the wanted set).

### W3 — Memory budget (max resident chunks, farthest-first)
`WorldStreamer::set_max_loaded_chunks` (0 = unlimited). After the unload
pass, while over budget, evict the farthest loaded chunk **outside the load
radius** — never inside it, or the chunk reloads next update and the budget
thrashes. Chunks inside the hysteresis band are evictable (that is the
point of the budget); chunks inside the load radius are must-keep. Eviction
flows through the unload handler and counts, like any unload. Runtime
passthrough + observables.

### W4 — LOD by distance
Pure `select_lod(distance, lod_count, bands)` in Rendering (empty bands →
lod 0, so single-LOD content is bit-identical). `Renderer3D` owns the bands
(defaults beyond any test scene: no behavior change) and stamps the
effective lod in render prep; depth + gbuffer passes consume it. `GpuPicker`
takes the same bands so picked == rendered (defaults to authored lod when
absent). Integration test: 2-LOD mesh (full LOD0, empty LOD1) renders lit
near, dark far.

### W5 — Verify, docs
Full suite, layering guard, samples, headless acceptance, `nf` end-to-end.
Rule 0 on the new behavior (force sync path, force over-budget, force far
LOD) where cheap.

## Explicitly out of scope
LOD cross-fading, predictive prefetching outside the hysteresis band,
per-chunk memory accounting in bytes (budget counts chunks), a dedicated
camera-following volume policy (volumes stay explicit), texture streaming.

## Verification notes (what the tests caught)
- **Missing chunks burned worker slots.** VFS resolve maps paths without
  checking existence, so every missing chunk in range dispatched a worker
  that failed on open — per update, forever. Existence is now a cheap main-
  thread stat before dispatch (TOCTOU still handled gracefully); the threaded
  cap test caught the churn via a leftover in-flight job.
- **Budget eviction is unloading**: it runs even with loading disabled
  (`max_loads_per_update == 0`), which a first version got wrong (early
  return skipped it) and a test caught.
- **New tests**: 4 streamer policy (order ×2, budget ×2), 2 Runtime wiring
  (async load/unload with physics rebuild, in-flight cap on real worker
  threads via an RAII JobSystem guard), 2 `select_lod` pure, 1 render
  integration (2-LOD mesh: lit near, dark far), 1 picker parity (misses
  under bands what the frame does not draw).
