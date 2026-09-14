# Phase 9 — Animation and Audio

**Status:** complete
**Written:** 2026-09-14
**Completed:** 2026-09-14
**Predecessor:** Phase 8 (Physics + Deterministic Simulation), complete and verified (366/0/1)

---

## 1. Why animation and audio, and why together

**Why animation and audio.** The design document's Year-2 plan (§259) is *Physics + Animation +
Audio + Tools*. Phase 7 delivered Tools; Phase 8 delivered Physics. The remaining two Year-2
items are Animation and Audio, and they share a structural property that makes them natural
companions: both are **sampled state producers** that sit alongside the existing transform and
physics pipelines without coupling to them. Animation produces bone transforms; audio produces
source/listener positions and gain. Both are stepped by the runtime, serialized in the scene,
and exposed in the editor.

**Why together.** Building them in the same phase means the ECS component pattern, the scene
serialization format, the runtime `update()` integration, the editor inspector layout, and the
packaged-player acceptance are all done once with both components in mind rather than twice.

**Why not a third-party animation runtime.** The same reasoning as Phase 8's "why not Jolt":
vendoring Ozz Animation or a similar runtime is a decision with real consequences. Phase 9
builds the **engine-facing API and a self-contained skeletal animation system**, with the backend
behind a seam. The design document's §42–43 list skeleton, clips, blend trees, state machines,
blend spaces, IK, retargeting, and root motion as eventual features; this phase delivers the first
four and leaves the rest as explicit non-goals.

**Audio backend.** The README already plans "Audio (MiniAudio integration)". MiniAudio is a
single-header library that is genuinely lightweight (one file, no dynamic deps) and is the right
long-term choice. However, the engine-facing API is designed first and tested headless (no
hardware device required), with MiniAudio as an optional backend that can be loaded behind the
seam. This keeps CI green on headless runners and lets the audio math (attenuation, mixing,
3D positioning) be tested without a sound card.

---

## 2. What is being built

| Layer | Contents |
|---|---|
| **Skeleton** | `Skeleton`: hierarchy of bones with parent indices, local-space rest transforms, names. `compute_world_transforms()` walks the hierarchy and produces world-space matrices. |
| **Clips** | `AnimationClip`: array of tracks. Each track = bone index + keyframes (translation Vec3, rotation Quat, scale Vec3) with timestamps. `sample(time, out_local)`. |
| **Poses** | `Pose`: per-bone local transforms + computed world transforms. `compute_world(skeleton)`. |
| **Sampling** | Keyframe interpolation: lerp translation/scale, slerp rotation. `sample()` finds the bracketing pair and interpolates. |
| **Blending** | N-way blend with weights (normalized). Additive blend (difference pose + base). `blend(a, b, weight)`. |
| **Player** | `AnimationPlayer`: play/pause/stop/set_time/set_speed, looping (none/loop/ping-pong), current time, current clip. |
| **State Machine** | `AnimationStateMachine`: states = clips, transitions with conditions, cross-fade duration, parameter set (float/int/bool). `update(dt)` returns blended pose. |
| **Audio Core** | `AudioDevice` (abstract, headless-safe), `AudioSource` (clip data + play state), `AudioBus` (mixer), `AudioListener` (position + orientation). |
| **Audio Math** | Linear/inverse/exponential distance attenuation curves. 3D pan/gain calculation. All testable without hardware. |
| **ECS** | `AnimationComponent` (skeleton + player + state machine), `AudioComponent` (source + bus + 3D flag). Runtime steps both on their clocks. Scene serialization round-trip. |
| **Tools** | Editor inspector panels for both components. Template scene gains an animated entity. Packaged player runs an animated scene. |

---

## 3. Determinism

Animation is not a simulation in the physics sense — it is a function of time, not of state.
But it must still be **bit-identical for the same inputs**: the same clip sampled at the same time
must produce the same transforms regardless of frame rate, substep count, or allocation order.

Design rules:
- Sampling is pure: `clip.sample(time, out)`. No mutable state in the clip.
- Interpolation uses the engine's existing `Quat::slerp` and `Vec3::lerp`, which are deterministic.
- The state machine's update is deterministic: transitions are evaluated in declaration order,
  and the first matching transition wins.
- Audio math (attenuation, pan) is pure and deterministic.

---

## 4. Work breakdown

| # | Work | Acceptance | Status |
|---|---|---|---|
| **W1** | Skeleton + clip + pose | `AnimationTests`: skeleton world transforms match hand-computed hierarchy; clip sampling at t=0 returns first keyframe; at t=end returns last; `Pose::compute_world` produces correct world matrices | ✓ 5 tests |
| **W2** | Sampling + blending + player | `AnimationTests`: interpolated mid-keyframe matches expected lerp/slerp; N-way blend weights sum to 1; additive blend produces correct delta; player loops correctly; ping-pong reverses | ✓ 19 tests |
| **W3** | State machine + cross-fade | `AnimationTests`: state transitions fire on condition change; cross-fade blends two clips over the fade duration; default state is entered on reset | ✓ 7 tests |
| **W4** | Audio core + math | `AudioTests`: distance attenuation curves match expected values at known distances; 3D pan/gain calculation is correct; mixer sums sources correctly; headless device returns silence without error | ✓ 20 tests |
| **W5** | ECS + runtime + serialization | `RuntimeTests`: a scene with an AnimationComponent and AudioComponent serializes round-trip; scene save/load preserves all component fields; **and the components are actually stepped** — a transform moves after `Runtime::update()`, a paused clip does not, a generated tone reaches the bus output, and the scene round-trip still animates | ✓ 8 tests (33 total) |
| **W6** | Editor, player, docs, CI | Suite updated to 439/0/1 across 11 suites; README and CI updated; `run_tests.sh` includes AnimationTests and AudioTests | ✓ see below |

**Final test suite:** 439 passed / 0 failed / 1 skipped (440 total).

**Acceptance (executed 2026-09-14):**
```
Full build clean under /W4 /WX
AnimationTests: 38/0/0
AudioTests: 27/0/0
RuntimeTests: 33/0/0 (8 animation/audio tests: 3 serialization round-trips
                       + 5 that assert the runtime actually steps them)
All 11 suites: 439/0/1
```

### 4.1 Closing the integration gap

The first pass of this phase built the module layer only: `AnimationComponent` and
`AudioComponent` existed, serialized, and had passing unit tests, but nothing ever *called*
`AnimationPlayer::update()` or `AudioBus::mix_source()` outside a test. `Runtime::update()` was
four calls and none of them touched animation or audio, so a shipped frame had a static pose and
silence, and the suite could not tell — every runtime test asserted a serialization round-trip,
which is invariant under "the subsystem is never stepped".

The integration pass added:

| Piece | What was missing | What now exists |
|---|---|---|
| Runtime stepping | `Runtime::update()` never sampled animation or mixed audio | `step_physics(dt)` → `step_animation(dt)` → `step_audio(dt)` → `propagate_transforms(world)` |
| Real samples | The scene loader created an empty `AnimationClip{}` (duration 0, no tracks) for every `Animation:` line, and `AudioBuffer` had no producer | `make_procedural_clip()` (spin/bob) and `make_tone_buffer()` (PCM sine) |
| Placement | Assigning the root bone's world matrix would snap an entity to the rig origin | `AnimationComponent` stores an authored base offset; `Runtime::rebase_animation()` re-anchors it so animation is applied *relative* to the entity's transform |
| Playback state | `AnimationPlayer::update()` returns early unless `PlayState::Playing`, which `set_clip()` sets — a loaded scene would sit at t=0 forever | The loader calls `player.play()` when the scene does not say `paused=true` |
| Editor | Zero animation/audio references in `Editor/src/ui/Panels.cpp` | `set_animation` / `set_audio` on `EditorApp` plus two inspector panels; `set_transform` calls `rebase_animation()` so hand-edits are not immediately undone |
| Acceptance | The template scene was still Phase 8's 5-entity physics scene; the player asserted nothing about animation | Template scene gains entity 6 (mesh + `Animation:` + `Audio:`); the player tracks max transform deviation and fails the run if an animated entity never moves |
| Tests | Round-trip-only assertions | `Tests/RuntimeTests/test_runtime_anim_audio.cpp` — 5 tests that fail when the stepping is removed (confirmed by reverting it) |

Rule 0 was applied to the new tests: with `step_animation(dt)` and `step_audio(dt)` commented out
and the tree rebuilt, `RuntimeTests` drops to 29/4 and the suite to 435/4/1. The fifth new test
(`runtime_audio_silent_without_sources`) passes in both states by design — it is the control that
proves the mixer is not simply emitting a constant.

---

## 5. Acceptance

The phase is done when this is green, in addition to the existing suite and acceptance:

```bash
nf new Demo --name Demo          # the template scene gains an animated entity
nf build --project Demo/Demo.nfproj
cd Demo/dist && ./NFPlayer --frames 120 --validation
# expect: exit 0, the animated entity's transforms have changed,
#         0 validation errors, 0 leaks
```

Plus the existing physics acceptance must still pass.

**Executed 2026-09-14** (packaged `NFPlayer`, 1280x720, validation on, NVIDIA RTX 5060 Ti):
```
Runtime: entity 5 has no skeleton data; using a single-bone root rig for procedural clip 'spin'
Runtime: physics world created with 2 bodies
Runtime: scene loaded 'content://Scenes/Main.nfscene' with 6 entities
Rendered 120 frames (meshes=1)
Animated entities: 1 (max transform deviation over 120 frames: 174.93932)
Audio sources mixed (peak): 1 (0.19999997)      # volume 0.4 x tone amplitude 0.5
Validation errors: 0
Alive RHI objects before shutdown: 0
=== NOVAForge Runtime exited cleanly ===            # exit 0
```

---

## 6. Risks

| Risk | Mitigation |
|---|---|
| Skeleton hierarchy math is error-prone (parent ordering, local vs world) | Hand-computed test skeleton with known transforms; assert world matrices to 1e-5 |
| Quaternion interpolation (slerp vs nlerp, shortest-path) | Use the existing `Quat::slerp` from Phase 8's math work; test shortest-path correction |
| State machine cross-fade can produce pose mismatches if skeletons differ | Assert same skeleton for all clips in a state machine |
| Audio on headless CI | AudioDevice has a null backend that returns silence; all math is tested without hardware |
| Scene format bloat with animation data | Animation clips are assets (UUID-referenced), not inlined in the scene; the scene stores only the component state |
| Editor panel complexity | Follow the existing InspectorCache pattern from Phase 8 physics panels |

---

## 7. Explicit non-goals

- **No IK, retargeting, root motion, or motion warping.** These are §42 features for a later phase.
- **No blend trees or blend spaces.** State machine with cross-fade is enough for a vertical slice.
- **No animation editor (timeline, keyframe editing).** §248 A8 is a future editor phase.
- **No audio streaming, reverb, occlusion, ambisonics, or voice.** §48 features for a later phase.
- **No audio effects chains or audio graph editor.** §49 is a future phase.
- **No multi-threaded animation or audio update.** Single-threaded for now; the job system exists
  but parallel skinning/mixing is not the bottleneck at this scope.
- **No GPU skinning.** CPU skinning only; the render path gets the final pose as a mesh override.
- **No MIDI or procedural audio generation.**
- **No audio import pipeline (WAV/OGG/FLAC).** The audio clip format is defined and the math works;
  actual file import is a future asset-pipeline task. A trivial PCM format is used for testing.
