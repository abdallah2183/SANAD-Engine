# Phase 29 — Jolt vehicles in the frame, the scene and the editor (Step 11)

The last open vehicle item (Phase 16 built the Jolt backend, Phase 25 left
"Jolt vehicles" unwired: they need a dedicated Jolt world plus static-geometry
sync, and only the hand-written VehicleDemo drove them). This phase closes it
the way Phase 25 closed the particle/cloth gap and Phase 28 the navmesh gap:
**the scene declares, the frame steps, the editor authors** — no game code
touches the vehicle API.

## Why a second world

The deterministic solver has no `VehicleConstraint`, so cars cannot live in
`m_physics`. They get their own `JoltWorld` — the same "dedicated world"
pattern destruction uses for shards, for the same reason: one world's bodies
must never be simulated twice. Dynamics stay with the engine solver; only
`Static` rigid bodies sync into the vehicle world.

## The scene line

```
Vehicle: chassis(0.9,0.5,2.0) mass=1500 wheel_radius=0.35 wheel_width=0.3
         track=0.9 wheelbase=1.3 wheel_y=-0.35 steer_deg=30 torque=500
         susp_min=0.3 susp_max=0.5 susp_freq=1.5 susp_damping=0.5
         tire_peak=1.2 tire_limit=1.0 wheel_inertia=0.9 angular_damping=0.2
         brake_torque=1500 handbrake_torque=4000 enabled=true
```

* Every key is optional; an impossible one rejects the whole line with a
  warning naming the key (the contract every component line keeps).
* Live inputs (`throttle/steer/brake/handbrake`) and the `JoltVehicle`
  pointer are never parsed or written — same rule as Character's
  `wish_dir/jump`. Copy/merge/clone scrub them to parked-null.
* `VehicleComponent` gains `enabled` (the "parked showroom" state), honoured
  by `VehicleSystem::startup` like every other component flag.

## The frame

`bind_scene_vehicles()` on adopt (fresh world → statics from world-space
transforms after an explicit propagate → `VehicleSystem::startup`; teardown
for a scene with no enabled car), and `step_vehicles(dt)` on the 1/60 fixed
clock right after `step_physics`: drive inputs in, Jolt solves, then one
trailing sync so Transforms render where the last step ended (drive() only
arms inputs, so the re-apply is idempotent). `dt = 0` freezes parked cars.

`VehicleSystem::update` now iterates in entity-id order (it was an
unordered_map walk — two runs of a two-car scene drove different cars
first). `reset_vehicle` uprights in place (the scene persists no spawn pose);
`vehicle_wheel_states` exposes per-wheel pose/steer/spin/contact for whoever
renders wheels (the demo covers that; the frame does not parent wheel
meshes — deferred on purpose).

## The editor

`attach/set/detach_vehicle` (validated at the door with the loader's bounds),
a full inspector section (20-number tune + live debug drive + reset), an
Add-Component entry, EN/AR keys, a stats line, and synchronous re-bind on all
six nav mutators and all vehicle mutators. Play re-binds from authored
components (a session begins from scene state, not bind-time poses); Stop
restores, scrubs and re-binds.

## Defect the wiring exposed (a real one)

PlayMode's two copiers (`clone_scene`, `copy_entity_all`) knew nothing about
`NavMesh`/`NavAgent`/`VehicleComponent`: a play/stop cycle **deleted** the
author's volume, walkers and cars from the edit scene. Both copiers now carry
config and scrub session state; `editor_play_stop_preserves_the_car` and
`editor_play_stop_preserves_navigation` pin it.

## Result (measured 2026-10-13)

`RuntimeTests` 135/135 (`test_runtime_vehicles.cpp` ×11), `EditorTests`
270/270 (`test_vehicle_editor.cpp` ×4) — clean `/W4 /WX` build. Full sweep:
**26 suites, 1863 passed / 0 failed / 2 skipped** (pre-existing environment
gates). `Content/Scenes/Vehicle.nfscene` demos it through adopt + update().

## Deferred on purpose

* Renderer-parented wheel meshes (`vehicle_wheel_states` is the seam).
* Dynamic Jolt bodies as vehicle obstacles (statics only today).
* Vehicle replication over the wire (`VehicleSnapshot` already in NFNetworking).

## Follow-ups landed

* **Step 12 — visible wheels**: `Runtime::build_render_world` injects four
  render objects per bound car from `vehicle_wheel_states` (render-only, the
  shard pattern — no ECS entities, nothing serialized or picked). Spin is
  integrated in `step_vehicles` on the fixed-step clock; pose is
  `T * (R_chassis * R_steer * R_spin) * S` via `compose_quat_scale_mat4`.
  RuntimeTests 138/138.
* **Step 13 — solid moving-obstacle mirrors**: scene bodies the engine solver
  simulates (`Dynamic`/`Kinematic`) reach the vehicle world as TRACKED
  kinematic mirrors — new `JoltWorld::set_pose` (teleport + activate) and real
  `Kinematic` support in `add_body`. One-way coupling, solver determinism
  untouched. PhysicsTests 218/218, RuntimeTests 140/140.
* **Step 14 — vehicle replication glue**: `Runtime` speaks engine types only
  (`VehicleNetRecord` = `net_id` + `JoltBodyState`); `assign_vehicle_net_id` /
  `collect_vehicle_net_states` / `apply_vehicle_net_states` with the ghost
  writeback mirroring `VehicleSystem::update` exactly, a freshness gate, and a
  "never over a car this world simulates" guard. The wire format
  (`NFVS` codec) stays the session owner's. RuntimeTests 143/143. Full
  sweep: **26 suites, 1871 passed / 0 failed / 2 skipped**.

All three Phase 29 deferrals are closed.
