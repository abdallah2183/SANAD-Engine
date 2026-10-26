# AGENTS.md — SANAD Engine

Read this before changing anything. It is the contract for coding agents
working in this repository.

## The one rule that matters

**SANAD is a native C++23 + Vulkan game engine. It is not a web project.**

When the user says "make a game", they mean a **SANAD project** built with the
`nf` CLI — a `.nfproj`, a text `.nfscene`, Lua gameplay scripts, and the native
player. They do **not** mean:

- `index.html`, CSS, JavaScript, React, Vite, or a browser canvas
- `package.json`, npm, a local web server, or Electron
- a Unity/Godot/Unreal project
- a Python/Pygame game

If you create any of those, you have failed the task even if the files look
polished.

## Where everything is

| Thing | Path |
|:---|:---|
| Engine source | `Engine/` |
| Editor source | `Editor/` |
| CLI source | `Tools/BuildTool/main.cpp` (`nf`) |
| Game templates | `Templates/{Default,ThirdPerson,FPSStarter,Platformer2D}` |
| Docs | `Docs/` |
| Built binaries | `build/Release/bin/` |
| Demo project | `MyFirstGame/` (only if present) |

## Build once

```bat
build_nf.bat Release
```

This discovers Visual Studio with `vswhere`, configures CMake + Ninja, and
builds `SANADEditor.exe`, `NFPlayer.exe`, `nf.exe`, tools, samples, and tests.
There are no machine-specific paths in the script.

If you only need the CLI and player, the same build still produces them:

```
build\Release\bin\nf.exe
build\Release\bin\NFPlayer.exe
build\Release\bin\SANADEditor.exe
```

## Make a game (the actual workflow)

From the engine repository root:

```bat
:: 1. create a playable project
build\Release\bin\nf.exe new Games\MyGame --name MyGame --template ThirdPerson

:: 2. edit the scene and Lua
::    Games\MyGame\Content\Scenes\Main.nfscene
::    Games\MyGame\Content\Scripts\player.lua

:: 3. build and run
build\Release\bin\nf.exe build --project Games\MyGame\MyGame.nfproj
build\Release\bin\nf.exe run   --project Games\MyGame\MyGame.nfproj

:: 4. verify without a window (CI/agent-friendly)
build\Release\bin\nf.exe run --project Games\MyGame\MyGame.nfproj --frames 120 --headless --validation
```

Acceptance is a command that **exits 0** and a log that says
`Validation errors: 0`; a folder of files is not acceptance. When the player
opens a window, run it with `--frames 300` so it closes by itself.

`nf new` writes an `AGENTS.md` into every project with the same rules and a
copy-ready Lua example. Read it when working inside a project.

## What to edit for gameplay

```
Games/MyGame/
  MyGame.nfproj
  Content/
    Scenes/Main.nfscene      entities + components (plain text)
    Scripts/player.lua       update(dt) gameplay
    Materials/*.nfmat        base_color, metallic, roughness
    Input/bindings.nfinput   optional: which key/pad means which action
  Cache/                     cooked output (git-ignored)
  dist/                      packaged player (git-ignored)
```

- Attach a script to an entity with:
  `Script: lang=lua path=content://Scripts/player.lua`
- A third-person camera is one line, and the player can aim it:
  `Camera: … follow=Player pivot=0,1.45,0 distance=5.2 yaw=180 pitch=-13 align=90`.
  Aiming is scene data — `mouse`, `looksens`, `sticksens`, `pitchmin`,
  `pitchmax`, `invert_y`, `zoommin`, `zoommax`, `zoomstep`, `align`. Every key
  is optional; `mouse=0` restores the old fixed orbit.
- Keep `entity_count` in the scene header equal to the number of `entity:`
  blocks.
- A gun is one line, and it fires along the camera the player aims with:
  `Weapon: mode=auto rpm=600 magazine=30 reserve=120 damage=18 range=120
  spread=0.4 recoil=0.8`. The entity the ACTIVE camera follows is the player,
  so its weapon is the one the trigger drives (`fire` = LMB/RB, `reload` =
  R/Y); every other weapon ticks but only fires when a script calls
  `nf.weapon_fire()`. Hitscan tests the scene's colliders as world boxes.
  Add `delivery=projectile speed=220 gravity=9.81` for a round with real flight
  time and drop; its lifetime is `range / speed`, and each frame sweeps the
  segment it travelled so it cannot tunnel through a wall.
  `damage_type=ballistic|explosive|fire` selects the damage channel (ballistic
  is the default and is not written), and `pierce=0..1` is the fraction of a hit
  that ignores armor. Add
  `blast_radius=4 blast_damage=45 blast_min=0.2` for a radial blast at the
  impact point: damage falls linearly to `blast_min` at the edge, walls block
  it, the direct victim takes only the direct hit, and every other victim in
  range gets its own `splash=true` impact event. A hit can also apply a
  damage-over-time status: `status=burn status_dps=4 status_time=3
  status_tick=0.5` (or `bleed` / `corrode`). The same status is applied by the
  direct hit and by each splash victim; a dead or invulnerable target is not
  ignited.
- A side is one optional line: `Faction: team=Blue`. Entities on the same
  non-empty team cannot damage each other; `friendly_fire=true` on the SHOOTER
  opts in. No faction, or an empty team, is neutral and preserves the old
  behaviour. A combat agent whose target is on its own team holds fire, and the
  validator warns about it.
- Something to shoot at needs hit points:
  `Health: max=100 regen=2 regen_delay=4`. Damage clamps at zero; the engine
  does not delete a dead entity — a script reads `nf.health()` and decides.
  Armor is a second pool: `armor=40 armor_max=40 absorb=0.6` spends armor before
  health, and `pierce=` on the weapon bypasses part of it. Hit zones are opt-in:
  `zones=true head=2 limb=0.75` reads the top 18% of the collider as a head and
  the bottom 28% as limbs. Without `zones=true` every hit is a body hit, so old
  scenes keep their exact damage. Per-type resistance is a fraction in `0..1`:
  `resist_ballistic=0.2 resist_explosive=0.1 resist_fire=0.4` reduces that type
  after the zone and before armor; absent keys mean zero.
- An active status can be authored or restored with
  `Status: burn=4,3,0.5 bleed=2,5,1` (dps, seconds, tick). The runtime ticks it
  through the same resistance/armor path and emits `status=true` impact events.
- An enemy is one line, and it fights on its own:
  `CombatAgent: target=Player eye=1.5 reaction=0.35 turn=220 cone=3 range=60
  burst=0.55 rest=0.85`. It needs a `Weapon:` on the same entity, and the
  target needs a `Health:` and a collider (the visibility ray hits colliders).
  `target=` is free text and must stay last on the line. Add
  `auto_target=true` (with a `Faction:` on the agent) to pick the nearest
  visible enemy team instead of naming one; the current target is held until it
  dies, leaves range or becomes hidden. A projectile weapon on an agent leads a
  moving target automatically — the solver is in `Projectile.hpp` and falls back
  to the current position when no intercept exists. Add `retaliate=true` to let
  the agent prefer whoever last damaged it (the damage ledger) when that target
  is still valid. Near-miss shots suppress an agent: aim slows, the fire cone
  narrows, and above 0.75 suppression the trigger is held until it decays.
- Shooting is invisible until a scene opts in: one `Feedback:` line turns shot and
  impact events into a muzzle flash, a tracer per pellet, a scorch decal and a
  procedural one-shot (Phases 56/59):
  `Feedback: flash_time=0.055 flash_size=0.22 tracer_time=0.09 tracer_length=9
  tracer_width=0.025 impact_time=6 impact_size=0.14 shot_volume=0.7
  impact_volume=0.45`. Every key is optional and a scene that omits the line draws
  and mixes exactly what it did before. Phase 59 added `blast_time=`/`blast_size=`
  (the larger second flash an explosive impact spawns once per round), the
  per-channel gains `impact_volume_explosive=`/`impact_volume_fire=` (the impact
  sound follows `damage_type`), the muzzle light `light_intensity=`/`light_radius=`
  /`light_r=`/`light_g=`/`light_b=` (`light=false` restores the 0.6.0 look, and the
  light takes whatever point-light slots the scene leaves free), and the caps
  `max_flashes=`/`max_tracers=`/`max_impacts=`/`max_sounds=`/`max_lights=`
  (`[1, 4096]`). The editor has a full Combat Feedback section for all of it.
- An NPC can have senses instead of omniscience (Phase 60):
  `Perception: sight=30 fov=60 gain=2 loss=0.6 linger=3 hearing=18 touch=1.5
  pain=true noise=0 visible=1 radius=0.5 eye=1.5 combat=true`. Sight is the
  authored sense (range, cone half-angle in degrees, how fast attention builds,
  how fast it fades, and how long a target stays KNOWN after everything goes
  quiet — that is the search window); `hearing`/`touch` are ranges on the AI
  module's defaults, and `pain` ignores distance: being shot tells you someone is
  out there. A range of 0 switches that sense off. With `combat=true` (the
  default) a `CombatAgent` on the same entity may only engage what its senses
  have ACCUMULATED, aims at the last known position once it loses sight, and
  holds fire while it cannot sense — `combat=false` keeps the old
  ray-finds-you rule exactly. The frame feeds all of this: the scene's actors are
  the cast, shot and impact events are the noises, and pain is addressed to the
  victim alone. Scripts ask their own memory with `nf.aware`/`nf.sensing`/
  `nf.last_known`/`nf.forget`, and the editor has a full Perception section.
- An NPC can decide instead of following a script (Phase 61): `Brain: rate=0.25`
  plus one `BrainAction: name=<token> input=<name> [curve=linear|quadratic|
  inverse|threshold] [slope=] [exponent=] [input_offset=] [output_offset=]
  [weight=] [hold_fire=true]` per consideration. The inputs are the world's, not
  the author's: `aware`, `sensing` (from the senses), `distance` to the current
  target, `health` and `ammo` as fractions, `threat` (suppression) and `enemies`.
  Actions are scored in line order and ties go to the earlier one, so the order is
  part of the behaviour. The engine publishes the winner rather than guessing what
  "advance" means — read it with `nf.decision()`; the one effect it applies is the
  authored `hold_fire`, which vetoes the trigger while that action wins. A brain
  re-decides when `rate` elapses (`rate=0` every frame).
- `nf build` cooks assets and writes `dist/`; `nf run` builds then launches
  `dist/NFPlayer.exe`.
- Rebinding is a file, and it is optional. `Content/Input/bindings.nfinput`
  overlays the engine defaults — a file that rebinds one action leaves the rest
  alone. Format: `action = key:W, pad:A, axis:LeftX`, with `+`/`-` signs and
  `#` comments. A line that does not parse is reported and the whole file is
  ignored rather than half-applied.

## Lua API (what a game can actually do)

```lua
function update(dt) end          -- required, called every frame
self_id, self_gen                 -- the running entity
nf.self()                         -- id, gen
nf.entity_pos(id, gen)            -- x, y, z, or nil
nf.set_entity_pos(id, gen, x,y,z)
nf.set_entity_scale(id, gen, sx,sy,sz)  -- zero hides a static crate
nf.entity_alive(id, gen)
nf.find_entity("CrateA")          -- id, gen, or nil
nf.entity_name(id, gen)
nf.entity_count()
nf.action("move_forward")         -- W/Up; also move_back/left/right/jump/interact
nf.axis("move_x")                 -- -1..1; also move_z, look_x, look_y
nf.camera_move()                  -- x, z: move_x/move_z ROTATED BY THE CAMERA
nf.camera_yaw()                   -- the active camera's yaw in degrees (0 = none)
nf.clamp(v, lo, hi)
nf.lerp(a, b, t)
nf.weapon_ammo()                  -- rounds in the magazine (0 with no weapon)
nf.weapon_reserve()               -- rounds carried
nf.weapon_reloading()             -- true while a reload runs
nf.weapon_reload()                -- start one; false if nothing to load
nf.weapon_fire()                  -- fire if the cadence allows; pellets traced
nf.last_hit()                     -- hit, x, y, z, distance
nf.impact_count()                -- impacts in the previous completed frame
nf.impact(i)                     -- table: target, name, x/y/z, nx/ny/nz,
                                 -- damage, applied, zone, armor_absorbed,
                                 -- damage_type, blocked, killed, splash
nf.shot_count()                  -- shots in the previous completed frame
nf.shot(i)                       -- table: shooter, x/y/z, dx/dy/dz, damage,
                                 -- pellet, pellets, projectile, seed
nf.health(id, gen)                -- current, max (0, 0 when it has none)
nf.damage(id, gen, amount)        -- raw health damage; bypasses armor/zones
nf.apply_status(id, gen, kind, dps, seconds, tick) -- burn/bleed/corrode
nf.status_remaining(id, gen, kind) -- seconds left, 0 when absent
nf.clear_status(id, gen, kind)    -- remove one kind
nf.last_attacker(id, gen)         -- id, gen, or nil
nf.killer(id, gen)                -- id, gen, or nil
nf.damage_taken(id, gen)          -- health, armor, hits, seconds_since_damage
nf.suppression(id, gen)           -- 0..1 from near misses
nf.aware(id, gen)                 -- does MY perception know that entity?
nf.sensing(id, gen)               -- is a sense reading it THIS frame?
nf.last_known(id, gen)            -- x, y, z where I last knew it, or nil
nf.forget(id, gen)                -- drop one memory; false if there was none
nf.decision()                     -- name, score of what this entity wants to do
print(...)                        -- engine log
```

`nf.camera_move()` is what a player script should move with: the mouse turns the
camera, so `W` means "away from the camera", not "-Z". In a scene with no
camera it returns the raw axes, so it is safe to call unconditionally. Use
`nf.axis("move_x")` only when you deliberately want screen-independent axes.

A complete collect-the-crates example is in
[`Docs/AI_Game_Guide.md`](Docs/AI_Game_Guide.md). Prefer editing the template's
`player.lua` over inventing new systems.

## Hard constraints

1. **Never create an HTML game.** This is the single most common failure.
2. Do not edit `Engine/` for a game task. Engine changes are a separate,
   explicitly requested feature and need tests.
3. Do not delete or rewrite the user's project files; make the smallest edit
   that produces the behavior.
4. Build before reporting success. If a build or run fails, read the log and
   fix it; never claim a result you did not observe.
5. Windows 10/11 x64 + Vulkan only. There is no browser build, no Linux player.
6. Release binaries are signed with the self-signed `SANAD Dev Build`
   certificate; a machine with Smart App Control/WDAC may still block newly
   built executables. That is an environment policy, not a reason to create a
   web game or to disable security. Two things DO help, and both are legitimate:
   - **Sign the build**: `powershell -ExecutionPolicy Bypass -File
     Scripts\sign_build.ps1` signs every executable in `build\<cfg>\bin` with
     a code-signing certificate from `Cert:\CurrentUser\My`. A certificate in
     the machine's Trusted Publishers + Trusted Root stores satisfies the
     policy. A public code-signing certificate is what a public release needs.
   - **Retry**: the machine's cloud reputation service answers a few minutes
     after a file is first seen, and a blocked binary becomes runnable on its
     own. Re-run it later instead of concluding that the test failed. A child
     process blocked this way makes the suite that spawns it look like a test
     failure: `nf new` / `nf run` refusing to start with Windows error **4551**
     is `STATUS_DEVICE_GUARD_BLOCKED`, not a bug in `nf`.
   - **Try the other certificate**: this machine refused a freshly signed
     `RuntimeTests.exe` under `SANAD Dev Build` and ran the same file signed by
     `NF Local Build`, while `UITests`, `AITests` and the tool-spawning tests
     needed `SANAD Dev Build`. Both certificates live in
     `Cert:\CurrentUser\My`; `Set-AuthenticodeSignature -FilePath <exe>
     -Certificate $cert -HashAlgorithm SHA256` re-signs one file without
     touching the others, and the binary that runs is the one to keep.
7. Do not commit `Cache/`, `dist/`, or generated binaries.
8. Launcher text goes through `draw_text_ext` (`ExtTextOutW`). `DrawTextW` can
   return a line height while drawing nothing on some Windows sessions; do not
   reintroduce it. `shell_w_str` pre-shapes Arabic to visual order for the same
   reason. Native `EDIT` controls have the same problem, so `edit_subclass`
   repaints their client with `draw_text_ext`; keep keyboard/selection/caret
   handling in the native control.

## Useful docs

- [README](README.md) — build and feature overview
- [AI game guide](Docs/AI_Game_Guide.md) — copy-ready game loop and scene/Lua
  examples
- [Getting started (AR)](Docs/Getting_Started_Ar.md)
- [Lua scripting (AR)](Docs/Lua_Scripting_Ar.md)
- [Asset import](Docs/Asset_Import.md)
