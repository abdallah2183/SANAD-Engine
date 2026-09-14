# Phase 8 — Physics and Deterministic Simulation

**Status:** complete
**Written:** 2026-09-13
**Completed:** 2026-09-13
**Predecessor:** Phase 7 (`nf new` → `nf build` → `NFPlayer`), complete and verified

---

## 1. Why physics, and why not Jolt

**Why physics.** Phase 7 closed the shipping loop, so the engine can now build and run a game. It
still cannot *simulate* one. `Physics`, `Animation`, `Audio` and `Scripting` are all empty
directories, which means the design document's vertical slice (§260 — player, character, enemies,
physics, animation, audio) is impossible, and the editor's Play button is a snapshot with nothing to
run. Physics is the first item of the document's own solo Year-2 plan (§259: *Physics + Animation +
Audio + Tools* — Tools arrived in Phase 7), and it is the most foundational of the four: gameplay,
animation and networking all sit on top of it.

**Why not Jolt.** The README plans "Physics (Jolt/PhysX integration)", and Jolt is reachable and
would be the right long-term choice. Vendoring ~100k lines of third-party source into this
repository is a decision with real consequences — it changes the dependency story, the build
requirements, and what "this engine's code" means — and it is not a decision to make on someone's
behalf. So Phase 8 builds the **engine-facing API and a self-contained deterministic solver**, with
the backend behind a seam that a Jolt implementation can satisfy without touching the component
model, the serialization, the editor or the runtime.

Concretely: `PhysicsWorld` is the only type the engine talks to, and it exposes bodies as
generation-checked handles. Swapping in Jolt means writing a second `PhysicsWorld` implementation.
The seam is deliberately the same shape as the RHI's (an interface plus one backend), because that
seam is the thing this codebase already does well.

**Honest limits of the self-written solver.** See §7. It is a real solver — sequential impulses with
warm starting, friction and sleeping — but it is not a Jolt replacement, and the non-goals say so.

---

## 2. What is being built

| Layer | Contents |
|---|---|
| **Math** | `Quat` conjugate/inverse/rotate/from_matrix; `Vec3` component-wise min/max/abs. Added to `Core/Math` rather than duplicated, because physics is not the only consumer. |
| **Shapes** | `Sphere`, `Box` (oriented), `Plane`. AABB, inertia tensor, mass properties. |
| **Bodies** | `RigidBody`: position, orientation, linear/angular velocity, mass and *inverse* world inertia, damping, static/dynamic/kinematic, sleep state. |
| **Broadphase** | Uniform spatial hash grid producing candidate pairs. Replaceable behind a one-function seam. |
| **Narrowphase** | sphere–sphere, sphere–box, box–plane, box–box (SAT + incident-face clipping). Emits manifolds: normal, up to four contact points, penetration depths. |
| **Solver** | Sequential impulses, warm starting, Coulomb friction, restitution, Baumgarte position correction, sleeping. |
| **Clock** | `FixedTimestep`: accumulator with max substeps and an interpolation alpha. Deterministic regardless of frame rate. |
| **Integration** | `RigidBodyComponent` / `ColliderComponent` in the ECS; `Runtime` steps physics on the fixed clock and writes transforms back; scene serialization round-trip. |
| **Tools** | Editor inspector for the components, collider debug draw, acceptance steps; the packaged `NFPlayer` runs a physics scene. |

---

## 3. Determinism — the property everything else depends on

The point of a fixed timestep is not smoothness, it is that **the same inputs produce the same
state**. That is what makes physics testable, replayable, and eventually networkable.

Design rules that follow:

- The simulation steps at a fixed `dt` with a bounded substep count. A frame that takes 200 ms does
  not produce a 200 ms physics step; it produces N fixed steps.
- Nothing in the step reads wall-clock time, frame count, or allocation order.
- Bodies are stored in a stable order and solved in a stable order. The broadphase may reorder
  candidates internally but must emit a deterministic sequence.
- A `state_hash()` over positions and orientations makes determinism **assertable**: the same scene
  stepped with different frame patterns must produce an identical hash.

## 4. Work breakdown

| # | Work | Acceptance | Status |
|---|---|---|---|
| **W1** | Math helpers | `CoreTests`: rotate a vector by a quaternion, conjugate undoes a rotation, `from_matrix(to_matrix(q))` round-trips, component-wise min/max/abs | ✓ 8 tests |
| **W2** | Shapes + mass properties | `PhysicsTests`: box inertia matches the closed form, AABB of a rotated box is tight, degenerate shapes rejected | ✓ 15 tests |
| **W3** | Broadphase | `PhysicsTests`: candidate pairs are a superset of brute-force overlaps and a subset of all pairs, on random configurations | ✓ 13 tests |
| **W4** | Narrowphase | `PhysicsTests`: penetration depth and normal direction per shape pair; box–box face contact yields ≥3 points (a single point makes boxes wobble) | ✓ 26 tests |
| **W5** | Solver | `PhysicsTests`: a box dropped on a plane comes to rest at exactly its half-height; a 3-box stack stays stacked for 600 steps; a bouncing ball's apex decreases monotonically; no NaN | ✓ 18 tests |
| **W6** | Fixed timestep + world | `PhysicsTests`: 60×1 ms steps and 1×60 ms step produce the same hash; substep count is capped; a dead body handle is rejected | ✓ 9 + 5 determinism tests |
| **W7** | ECS + runtime + serialization | `RuntimeTests`: a scene with physics steps headless and its transforms change; scene save/load preserves the components | ✓ 5 physics serialization tests |
| **W8** | Editor, player, docs, CI | Acceptance gains physics steps; the packaged player runs a physics scene with 0 validation errors and 0 leaks | ✓ see below |

**Final test suite:** 366 passed / 0 failed / 1 skipped (367 total).

**Acceptance (executed 2026-09-13):**
```
nf new Demo --name Demo
nf build --project Demo/Demo.nfproj
cd Demo/dist && ./NFPlayer --project Demo/dist/Demo.nfproj --frames 120 --headless --validation
# exit 0, physics world created with 2 bodies, 5 entities loaded, 0 validation errors, 0 leaks
```

## 5. Acceptance

The phase is done when this is green, in addition to the existing suite and acceptance:

```bash
nf new Demo --name Demo          # the template scene gains a falling body
nf build --project Demo/Demo.nfproj
cd Demo/dist && ./NFPlayer --frames 120 --validation
# expect: exit 0, the body has moved and come to rest, 0 validation errors, 0 leaks
```

Plus a determinism test that is the real proof: the same scene, stepped with wildly different frame
patterns, produces an identical state hash.

## 6. Risks

| Risk | Mitigation |
|---|---|
| Box–box manifolds are the hardest part and easy to get subtly wrong | Test penetration depth and normal direction per axis; require ≥3 contact points for face contacts; test a resting box's height to 1e-4 |
| Stacking instability (jitter, sinking) | Warm starting plus Baumgarte correction plus a sleep threshold; assert a 3-box stack is unchanged after 600 steps |
| Determinism broken by iteration order | Stable body ordering, no hash-map iteration in the step, and a hash test across step patterns |
| Scope creep into a Jolt replacement | §7 is binding |
| Physics in the render loop wrecks frame pacing | The fixed clock owns stepping; the render path only reads interpolated transforms |

## 7. Explicit non-goals

- **Not a Jolt replacement.** No continuous collision detection, no joints/constraints beyond
  contacts, no convex hulls beyond box, no soft bodies, no cloth, no character controller.
- No capsule or cylinder shapes in this phase.
- No rotation-only (gyroscopic) integration terms beyond the standard inertia update.
- No multi-threaded solver.
- No networking or rollback — determinism is built for it, but not exercised.
- No editor gizmos for colliders beyond a wireframe debug draw.
