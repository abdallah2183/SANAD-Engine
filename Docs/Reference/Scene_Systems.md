# Scene systems reference

[Documentation](../README.md) · [العربية](Scene_Systems.ar.md)

Scene-format and runtime notes retained from the development reference. For the released package and verification results, see [0.9.0](../Releases/0.9.0.md).

## GPU profiling

The development tree closes the RHI timing gap with **real Vulkan timestamp
queries**. It is deliberately opt-in for games: normal gameplay records no
timestamps and pays no per-frame profiling cost.

- **Editor:** profiling is on by default as a development tool. The Performance
  panel shows `GPU (timestamp)`, a decaying average, and one row per RenderGraph
  pass. On a device without timestamp support it falls back to the existing
  submit-to-fence estimate (`gpu_us`/`gpu_avg_us`).
- **Player:** pass `--gpu-timing` (or set `NF_PLAYER_GPU_TIMING=1`) to print the
  last completed frame and its per-pass breakdown at exit. Without it no report
  is printed; a scene that explicitly asks for GPU-aware dynamic resolution may
  still enable frame timestamps internally.
- **Dynamic resolution can use it:** `dynamic_resolution_timing=gpu` makes the
  frame-time governor consume real GPU frame time instead of CPU `dt`, with an
  automatic fallback to CPU when no fresh sample exists. It needs frame timing
  only — never the per-pass drains — and the cooldown stays on wall time.
- **Two independent renderer switches:** frame timestamps and per-pass
  timestamps. `Runtime::set_gpu_profiling_enabled(true)` enables both and
  re-applies the request after every renderer init; direct `Renderer3D` users
  can keep only the frame timing if per-pass drains are too intrusive.
- **Honest numbers:** results are sampled one in-flight slot late, so the first
  frame has no sample. Per-pass timing inserts a `BottomOfPipe` at each pass
  boundary, which serialises the GPU; a timed run is useful for attribution but
  is not a free measurement of an untimed frame. RenderGraph barriers stay
  outside each pass's duration.
- **Tests:** `RHITests` covers a real timestamped GPU transfer, availability
  polling, RenderGraph pass brackets, a three-frame `Renderer3D` integration
  run, and the governor's GPU-time/CPU-cooldown rules. `RuntimeTests` proves a
  scene enables frame timing without profiling and that a stale sample falls
  back to CPU. See [GPU profiling](../../Docs/GPU_Profiling.md) for the query layout,
  API, and limitations.

```bat
:: development capture; NFPlayer also accepts NF_PLAYER_GPU_TIMING=1
build\Release\bin\NFPlayer.exe --project Games\TreeWorld\TreeWorld.nfproj --gpu-timing
```

### Frame pacing (Phase 41)

The editor's Performance panel and `NFPlayer --frame-stats` report a rolling
120-frame window: average FPS, 1% low, frame-time jitter, and a CPU/GPU
bottleneck verdict. It measures raw wall time, so presentation/vsync waiting is
visible; add `--gpu-timing` for the GPU average and verdict. This is telemetry
only and is separate from DynamicResolution. See
[frame pacing](../../Docs/Frame_Pacing.md).

```bat
build\Release\bin\NFPlayer.exe --project Games\TreeWorld\TreeWorld.nfproj --frame-stats --gpu-timing
```

### Layered damage, blasts, shot events, statuses and combat memory (Phases 50-56)

The development tree turns one damage number into a layered hit. Hit zones are
opt-in per target (`Health: ... zones=true head=2 limb=0.75`), armor is a real
second pool (`armor=40 armor_max=40 absorb=0.6`), and a weapon's `pierce=` can
bypass part of it. Damage types (`damage_type=ballistic|explosive|fire`) meet
per-type resistance (`resist_fire=0.4`). The order is fixed and tested:
falloff, zone, resistance, armor, health. `nf.impact(i)` carries `zone`,
`armor_absorbed`, `damage_type` and `splash`.

A weapon can own a radial blast (`blast_radius=4 blast_damage=45
blast_min=0.2`). Damage falls linearly to the edge, distance is measured to the
nearest point of a box, walls block the blast, factions still apply, and the
direct victim is not hit twice. Every other victim gets its own `splash=true`
impact event.

`ShotEvent` is the other half: one event per pellet, with the muzzle origin,
the post-spread direction and the pellet index. `nf.shot_count()` /
`nf.shot(i)` let a game draw a muzzle flash or a tracer even when the shot
misses.

A hit can start a damage-over-time status (`status=burn status_dps=4
status_time=3 status_tick=0.5`). Burning, bleeding and corrosion tick at a fixed
cadence through the same resistance and armor path; a partial final tick pays
exactly what is left. `nf.apply_status()` / `nf.status_remaining()` /
`nf.clear_status()` expose it to Lua.

Combat memory survives the frame: a damage ledger records the last attacker,
totals and killer, and `retaliate=true` lets an agent prefer that attacker when
it is still a valid target. Near-miss shots build suppression, slowing the turn,
narrowing the fire cone and holding the trigger above 0.75. Lua reads it through
`nf.last_attacker()` / `nf.killer()` / `nf.damage_taken()` / `nf.suppression()`.
See [Combat_Damage.md](../../Docs/Combat_Damage.md).

```text
Health: max=100 health=100 armor=40 armor_max=40 absorb=0.6 zones=true head=2 limb=0.75 resist_explosive=0.2
Weapon: mode=semi rpm=60 magazine=1 damage=20 range=120
        delivery=projectile speed=35 gravity=9.81 damage_type=explosive
        blast_radius=4 blast_damage=45 blast_min=0.2
```

### Combat feedback (`Feedback:`, Phases 56 & 59)

Events are only data until something draws them. The `Feedback:` line opts a
scene into the visible and audible half: one muzzle flash and one gunshot per
volley, one tracer per pellet, and one scorch decal plus one impact sound per
real surface hit. Status ticks and splash victims are excluded, because they are
not a new round landing.

```text
Feedback: flash_time=0.06 flash_size=0.26 tracer_time=0.1 tracer_length=7
          tracer_width=0.03 impact_time=8 impact_size=0.18
          shot_volume=0.6 impact_volume=0.4
          blast_time=0.16 blast_size=0.9
          impact_volume_explosive=0.75 impact_volume_fire=0.55
          light_intensity=3 light_radius=7 light_r=1 light_g=0.72 light_b=0.35
          max_flashes=48 max_tracers=96 max_impacts=192 max_sounds=16 max_lights=4
```

Phase 59 closed the five gaps 0.7.0 declared:

- **The flash lights the world.** Every live muzzle or blast flash also
  contributes a transient point light, so a shot brightens the wall it is fired
  at instead of only adding an emissive cube. `light=false` restores the 0.7.0
  look exactly; the light takes whatever renderer slots the scene's own point
  lights leave free and dies with the cube it came from.
- **An explosion looks like one.** An explosive impact spawns a second, larger
  and slower blast flash at the point of impact — once per round, never once per
  splash victim, so a grenade in a crowd is one fireball.
- **Impact sound follows the damage type.** Lead on a wall, a blast and fire are
  three different procedural one-shots with their own gain, because the impact
  event has carried `damage_type` since Phase 52 and one click for all three
  made them sound identical.
- **The caps are scene keys.** 48 flashes / 96 tracers / 192 marks / 16 sounds
  were literals; they are now authorable, with `max_lights` for the muzzle light.
- **There is an inspector section.** Combat feedback was the last component
  authorable only by hand-editing the scene file. The panel attaches, edits,
  validates against the loader's own ranges and detaches it. Play mode and
  entity copies carry the component too, so a scene no longer loses its feedback
  when it enters Play.

There is no new render pass and no audio asset. Flashes and tracers are
render-only objects on a shared procedural cube, the scorch is a projected
decal, and the sounds are generated one-shots with a fixed seed. The runtime
keeps the live voices across audio blocks, applies the scene's occlusion and bus
volumes, and retires a voice when it finishes. A scene with no `Feedback:` line
draws and mixes exactly what it did before, and the validator names a key whose
value is out of range. `Ballistics.nfscene` ships opted in.

### NPCs that see and hear (`Perception:`, Phase 60)

`ai::Perception` — four senses, a detection meter per sense, a linger window, a
last known position — has been unit-tested since Phase 22 and was reachable from
nothing: no scene could declare a sensor, and no frame fed it a cast or an event.
One optional line binds it to an entity:

```text
Perception: sight=30 fov=60 gain=2 loss=0.6 linger=3
            hearing=18 touch=1.5 pain=true
            noise=0 visible=1 radius=0.5 eye=1.5 combat=true
```

- **Sight** is the authored sense: range, cone half-angle, how fast the meter
  fills, how fast it drains, and how long a target stays *known* after every
  sense goes quiet — that last one is the search window.
- **Hearing** goes around corners; a gunshot is a noise the frame publishes from
  its own shot events, held for a lifetime so a single report can actually be
  noticed. **Touch** is contact. **Pain** ignores distance entirely: being shot
  tells you someone is out there.
- **Pain is addressed, not broadcast.** A wound is routed to the victim's own
  cast only, because the damage sense is neither attenuated nor gradual — a
  shared stimulus would make every NPC in the level know who shot one of them.
- **A sensor is never in its own cast**, so `most_threatening()` cannot answer
  "me".
- **An agent with `combat=true` (the default) only engages what it has
  accumulated**, aims at the last known position after losing sight, and holds
  fire while it cannot sense. `combat=false` keeps the pre-Phase-60 rule exactly:
  a clear ray is enough.
- **Scripts read their own memory**: `nf.aware(id, gen)`, `nf.sensing(id, gen)`,
  `nf.last_known(id, gen)` and `nf.forget(id, gen)`. The sensor is always the
  entity the script runs on.
- The editor has a full **Perception** section, and the validator calls a
  `combat=true` agent with every sense switched off an *error* — that agent can
  never acquire a target, which is a level that does not work rather than one
  that merely looks off.

There is no new render pass and no new asset: the whole phase is the seam between
code that already existed and the frame that feeds it.

### NPCs that decide (`Brain:` / `BrainAction:`, Phase 61)

Perception answers "what do I know"; this answers "what do I want to do about
it". `ai::UtilitySystem` and its response curves have been unit-tested since
Phase 22 and were reachable from nothing — no scene could name a consideration,
no frame turned the world into the numbers a consideration is a function of:

```text
Brain: rate=0.25
BrainAction: name=engage  input=aware    curve=linear weight=2
BrainAction: name=flee    input=health   curve=inverse weight=1
BrainAction: name=advance input=distance curve=quadratic slope=-1 output_offset=1
BrainAction: name=hold    input=threat   curve=threshold hold_fire=true
```

- **Seven inputs, computed by the frame**: `aware`, `sensing` (from the senses),
  `distance` to the current target, `health` and `ammo` as fractions, `threat`
  (suppression) and `enemies` (how many actors it is aware of). A name the frame
  has no number for is refused by name — a consideration that can never win is a
  typo, not a strategy.
- **The engine does not invent what an action means.** It scores the authored
  considerations in insertion order (ties to the earlier one), publishes the
  winner's name, and stops there: "advance", "flank" and "call for help" are a
  game's vocabulary. Read it from Lua with `nf.decision()` → name, score.
- **The one effect it does apply is the authored `hold_fire`**: the combat agent
  holds its trigger while that action wins and keeps tracking — a guard that has
  decided not to shoot still watches what it is looking at.
- **The clock is the only state.** A brain re-decides when its authored period
  elapses (`rate=0` every frame) and the decision holds in between; a brain that
  re-decides every frame flickers between two nearly equal utilities.
- The **validator** names the list-level mistakes: nothing to consider, two
  considerations with the same name, a list where every weight is zero, and
  numbers outside the module's range. The editor has a full **Brain** section
  with one row per consideration.

### NPCs with a state machine (`States:` / `State:` / `Transition:`, Phase 62)

Utility scoring answers "which consideration pulls hardest right now"; a machine
answers "what am I doing", where the *sequence* is the behaviour — patrol,
suspicious, chase, search, return. `ai::StateMachine` has been unit-tested since
Phase 22 and, like the other two, reachable from nothing:

```text
States: rate=0.1 initial=patrol
State: name=patrol hold_fire=true
State: name=alert
State: name=return
Transition: from=patrol to=alert  when=aware
Transition: from=alert  to=return when=health below=true at=0.3
Transition: from=return to=patrol when=distance at=4 below=true
```

- **A guard is one sentence, not an expression language**: one of the same seven
  inputs the brain reads, compared against `at`. `when=aware`,
  `when=health below=true at=0.3` and `when=distance at=6 below=true` are the
  four sentences a patrol actually needs, and a format that could compute more
  would need a parser, a scope and an error model.
- **The order is the decision**: transitions are evaluated in line order and the
  first guard that holds wins — the module's documented tie-break, and a test
  pins it.
- **The validator is where the cross-references are checked** (the loader reads
  one line at a time): an edge to a state nothing declares is an *error* — the
  machine would drop it and the NPC would never leave a state — and an
  unreachable state, a bad `initial` and duplicate names are reported.
- **`hold_fire` on a state** vetoes the trigger, exactly as it does on a brain
  action; `nf.state()` returns the state a script is running in, and the editor
  has a full **State machine** section with a row per state and per edge.

### A ground the AI can walk (`Terrain:` / `NavLink:`, Phase 63)

The navigation bake used to assume a flat floor. One `Terrain:` line replaces it
with the same deterministic height field the player sees:

```text
Terrain: res=128 area=90 height=24 freq=0.01 octaves=4 seed=3
         material=content://Materials/Grass.nfmat
```

- **One field, two consumers**: the terrain mesh and the navigation bake sample
  the same noise (or `heightmap=` image), so a hill the player climbs is a hill
  the agent paths over. A scene without the line keeps the flat floor it always
  had, byte for byte.
- **Links for what a bake cannot infer**: `NavLink: from=-6,12.42,14
  to=4,13.46,-12` snaps both ends onto the mesh and drops (counts, and warns in
  the validator) a link that ends off it; `one_way=true` makes a drop one-way.

### Patrol routes on it (`NavRoute:` / `NavPoint:`, Phase 64)

A `NavAgent:` walked to one goal and stopped. A route turns the same walker into
a patrol without a script:

```text
NavAgent: speed=3.2 goal_x=0 goal_y=0 goal_z=0 arrive=0.4
NavRoute: wait=1 loop=true ping_pong=false
NavPoint: pos=-14,14.15,14
NavPoint: pos=13,12.08,-13 wait=2.5
NavPoint: pos=10,12.32,10
```

- **The points are destinations, not a spline**: the runtime asks the navmesh for
  a path to each in order, so a route bends around whatever the bake says is
  walkable. Y must sit near the baked surface (`NavMesh::locate` allows the
  agent's climb tolerance); a point off the mesh cannot be reached.
- **Policy is three words**: `loop=true` wraps after the last point,
  `ping_pong=true` reverses at both ends (and wins if both are set), and neither
  stops the walker at the last point — `finished`. `wait=` is seconds paused at
  each point; a point's own `wait=` overrides it, and an absent `wait=` (stored
  as `-1`) inherits the route's.
- **The goal does not disappear**: a route with fewer than two points,
  `enabled=false`, or a removed component falls back to the authored
  `NavAgent:` goal from wherever the walker stands, and the validator warns
  about the short route instead of leaving a silent no-op.
- **The cursor is session state**: point index, direction and the remaining
  pause live beside the component, never in the scene. Saving mid-patrol saves
  the authored route, and a script reads the live cursor with `nf.route()` →
  `index, count, wait, finished` (nil when the entity is goal-driven); Phase 65
  adds `held` as a fifth value and Phase 66 appends `investigating` as the sixth.
- The inspector has a **Patrol Route** section with one row per point. Editing a
  position re-paths the live agent; changing the point count or the
  loop/ping-pong policy restarts the patrol at its first point.

### Patrols that stop and look (`hold=`, `resume=`, Phase 65)

A patrolling walker can let its senses interrupt the route:

```text
NavRoute: wait=1 loop=true hold=sensing resume=1.5
Perception: sight=36 fov=120 gain=2.5 loss=0.6 linger=3
Faction: team=Blue
```

- **Two policies**: `hold=aware` pauses while the walker's perception still KNOWS
  another live target, so the `linger` window keeps it standing after the target
  disappears. `hold=sensing` pauses only while a sense is reading one this frame.
  `hold=off` is the default and is exactly the Phase 64 patrol.
- **Who counts as a threat**: another entity the walker can perceive. If the
  walker has an enabled non-empty `Faction: team=`, only a different non-empty
  team counts; a walker with no team treats anyone it notices as a threat. A
  target whose `Health:` is at zero is skipped, so a corpse does not freeze a
  patrol.
- **`resume=` is post-sighting hysteresis**: after the gate opens the walker keeps
  standing for that many seconds (default 0), then continues the same route
  point. The key is written only when non-zero, so a route that never opted in
  keeps its exact Phase 64 line.
- **Holding is a pause, not a new route**: the authored `wait=` freezes too, the
  debug overlay marks the agent arrived and draws its route red, and releasing
  resumes the same point. The hold state is session state and is never saved.
- **One frame of latency**: navigation steps before perception in the frame, so
  the gate reads the senses from the previous step. That is a deliberate
  one-frame delay, documented rather than hidden.
- **Lua and editor**: `nf.route()` returns `index, count, wait, finished, held`;
  the inspector has the policy combo and resume field, and the validator warns
  about a gate with no enabled `Perception:` or a `resume=` with no `hold=`.
  `Content/Scenes/Terrain.nfscene` demonstrates it: WalkerA (Blue) patrols with
  `hold=sensing resume=1.5` and WalkerB is Red.

### Searching the last known position (`investigate=`, Phase 66)

A held patrol can do more than resume: it can walk to where the threat was last
known, scan, and then continue the route.

```text
NavRoute: wait=1 loop=true hold=sensing resume=1.5 investigate=3
```

- **`investigate=` seconds**: after the hold and its `resume=` hysteresis end,
  the walker leaves the route for the strongest noticed threat's last known
  position, scans there for that long, then continues. Omitted or zero is
  exactly the Phase 65 route, and the key is written only when non-zero.
- **The search replaces a pause**: if the hold interrupted a waypoint pause, the
  scan takes its place and the route advances to the next point afterwards; if
  it interrupted a leg, the walker re-paths to the same route point. The
  authored `wait=` is frozen while held and never replayed after a scan.
- **A new sighting restarts the search**: while scanning, a fresh sighting
  freezes the clock, updates the remembered point, and re-arms the detour for
  after the new hold clears.
- **Deterministic target**: the strongest currently noticed threat's memory is
  used; a tie keeps the earlier cast entry.
- **`hold=off` clears it**: switching the policy off releases the hold and drops
  any pending search, which is the Phase 65 behaviour exactly. The validator
  warns about `investigate=` with no `hold=`.
- **Lua, editor and overlay**: `nf.route()` appends `investigating` as its sixth
  value, the inspector exposes the seconds field and a live readout, and the
  debug overlay paints the detour yellow. `Content/Scenes/Terrain.nfscene`
  demonstrates the full sequence on WalkerA.

### Squads that search together (`squad=`, Phase 67)

A search can be a squad action instead of a solo one:

```text
NavRoute: wait=1 loop=true hold=sensing resume=1.5 investigate=3 squad=guards
```

- **`squad=<token>`**: one case-sensitive token, at most 64 characters. Every
  patrol with the same token answers a mate's investigation even if it never saw
  the threat itself; a walker needs the token, its own `investigate>0` and a
  `hold=` policy, not its own sighting.
- **Only own searches broadcast**: a call born from a walker's own senses is
  published to its squad. A search that answered a call stays silent, so calls
  cannot amplify from mate to mate.
- **Who answers, and for how long**: a member answers the oldest live call it has
  not answered—walking to the same last known position, scanning for **its own**
  `investigate=` seconds, then resuming its own route. Finishing a scan records
  the call as answered, so a still-running call cannot pull the same walker back.
- **One frame of recruitment latency**: the board is rebuilt at the top of each
  navigation step from the previous step's state. A mate joins one frame after
  the call starts, and a broadcaster that stops searching, dies or loses its
  route stops recruiting immediately.
- **Opt-in exactly**: an empty token is the solo Phase 66 route. The key is
  written only when non-empty, so older scenes round-trip byte-identically, and
  the validator warns about `squad=` without `investigate=`. The token is
  session-free scene data; no search state is ever saved.
- **Inspector and demo**: the patrol section has a Squad field that appears with
  the search and clears when the gate is switched off. `Terrain.nfscene` gives
  WalkerA and WalkerC `squad=guards`; Red WalkerB is the shared threat either
  Blue walker can call the other to search. `nf.route()` keeps its six values —
  the squad is authored policy, not a new cursor.

### Squads that share their contacts (`Squad:`, Phase 68)

The Phase 67 board shares a **search**. A radio shares a **contact**:

```text
Squad: token=guards radio=45
```

- **`token=`** is required and is one case-sensitive word of at most 64
  characters, matched exactly. An empty token is inert and writes no line, so a
  scene without the line behaves exactly as it did before.
- **`radio=`** is the link range in metres, `[0, 10000]`, default 30. A delivery
  needs the distance to fit **both** radios (`distance <= min(reporter,
  receiver)`), so a short-range scout cannot shout past its own antenna.
- **What is shared**: each frame every enabled member reports the live targets
  its OWN senses are reading this frame, with the last position it knows. A
  corpse is not a contact, a same-token mate is never a contact, and a member on
  a real team reports only hostile teams — the same social filter the hold uses.
  A member with no team reports anyone it notices.
- **What a receiver gets**: awareness and the last known position, plus its own
  `linger` search window. It does **not** get a sense reading, detection, or the
  personal-perception bit. It can turn, hold on `hold=aware`, and investigate; it
  cannot fire at what it has not sensed, and `hold=sensing` ignores the report.
- **No relays**: reports are snapshotted before anything is delivered, so a
  report cannot spread twice in one frame, and only personally sensed contacts
  are reported at all. A search born from a report is silent on the Phase 67
  board (`search_id=0`), so a rumour cannot recruit a second mate.
- **One frame of latency**: navigation runs before perception, and the radio is
  exchanged at the start of the perception step from the previous step's
  memories. A shared contact therefore arms a hold or investigation on the next
  navigation step, exactly like a sighting.
- **Never replaces a personal search**: a radio report may arm a fresh
  investigation, but it does not restart or replace one already running, and a
  radio-born pending search never downgrades an armed personal one.
- **Inspector and demo**: the inspector has a Squad radio section (token, range,
  enabled) with attach/apply/detach and the same validation as the loader. The
  validator warns about an empty, over-long or spaced token, a range outside
  `[0, 10000]`, `radio=0`, and a radio with no enabled `Perception:`.
  `Terrain.nfscene` gives WalkerA and WalkerC `token=guards radio=45`; WalkerC
  uses `hold=aware`, so a report from WalkerA stops it even though its own sight
  is shorter, and Red WalkerB is the contact both can share.

### Squads that call out a lost contact (`lost=`, Phase 69)

A live report says "I see them now". A callout says "I saw them there, and I
have lost them":

```text
Squad: token=guards radio=45 lost=2
```

- **`lost=`** is the receiver's memory window in seconds, `[0, 10000]`, default
  0. Zero keeps Phase 68 exactly: no callout is sent or accepted. Both radios
  have to opt in, and the receiver's own sight `linger` remains a floor, so a
  longer personal search window is never shortened.
- **When it fires**: the step a contact this member PERSONALLY sensed stops
  being sensed, the member queues exactly one callout with the last position it
  held. A target that dies is a kill, not a lost contact; a contact that arrived
  only over the radio never enters the bookkeeping, so the anti-relay rule does
  not change.
- **One frame later**: the callout is delivered at the start of the next radio
  exchange with the same filters as a live report (token, both ranges, faction,
  corpse, self, same-token mates). It is awareness and a last known position,
  never a sense and never detection.
- **Navigation**: a lost callout obeys the Phase 68 rules. It can hold or arm a
  radio-born search, it stays silent on the Phase 67 board (`search_id=0`), and
  it never restarts or replaces a personal investigation.
- **Inspector, validator and demo**: the Squad radio section has a lost-window
  field, the validator warns about a value outside `[0, 10000]`, and Terrain's
  WalkerA and WalkerC use `lost=2`.

### Radios that can be jammed (`Jammer:`, Phase 70)

A field degrades every squad radio inside it:

```text
Jammer: radius=9 strength=0.5
```

- **`radius=`** is the field reach in metres, `[0, 10000]`, default 12.
  **`strength=`** is the fraction of an antenna's range removed inside it,
  `[0, 1]`, default 1. `enabled=false`, `radius=0` or `strength=0` is inert, and
  a scene without the line is exactly the Phase 69 radio.
- **Who is affected**: any member of a squad radio (live report or lost callout)
  whose world position is inside the field. The strongest covering field wins;
  fields do not stack, and there is no team filter — a jammer jams its owner's
  antenna too.
- **How delivery changes**: an antenna inside a field keeps `1 - strength` of
  its range, and a delivery still needs BOTH jammed antennas to reach. A field
  over either end of a link can cut it; `strength=1` silences a radio completely.
- **What is not jammed**: the Phase 67 `squad=` search board, personal senses,
  detection, and lost tracking. Jamming only decides whether a radio delivery
  arrives this frame; it never changes what a member sees or remembers by itself.
- **Demo, inspector and validator**: Terrain's `RadioJammer` entity is a 9 m,
  50% field in the walkers' path, so the Blue squad loses half its radio reach
  near it. The inspector has a Radio jammer section with attach/apply/detach, the
  loader refuses values outside the bounds, and the validator names an
  out-of-range field, an inert field and a field with no Transform.

### Radios that only hear where they face (`cone=`, Phase 71)

A directional antenna talks into a cone, not in every direction:

```text
Squad: token=guards radio=45 lost=2 cone=90
```

- **`cone=`** is the horizontal antenna half-angle in degrees, `[0, 180]`,
  default 0. Zero is omnidirectional and keeps the Phase 70 radio exactly; a
  positive value makes this end directional. A scene without the key round-trips
  and behaves byte for byte as before.
- **Who has to face whom**: a delivery needs the receiver inside the reporter's
  cone **and** the reporter inside the receiver's cone when that end is
  directional. The test composes with the Phase 68 range and Phase 70 jamming:
  all three gates must pass, and `cone=0` at either end removes only its own
  direction check.
- **Where it points**: along the entity's Transform yaw (`rot_y`), or along a
  `CombatAgent`'s current aim when it has one — the same rule the entity's own
  senses already use. The check is horizontal; a ridge or a floor does not stop a
  radio link, so only direction and reach decide delivery.
- **What it changes**: live reports and lost callouts obey the same cones, with
  the same one-frame latency. A cone never touches the Phase 67 `squad=` board,
  personal senses, detection, or lost tracking, and a received report is still
  knowledge that is never relayed.
- **Demo, inspector and validator**: Terrain's WalkerC uses `cone=150`, so its
  radio is directional without breaking the Phase 68 hold demo. The inspector's
  Squad radio section has a 0–180° cone slider with attach/apply/detach, the
  loader refuses a value outside the bounds, and the validator names an
  out-of-range cone.
