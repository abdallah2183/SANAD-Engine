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
6. The 0.2.0 demo binaries are unsigned; a machine with Smart App Control/WDAC
   may block newly built executables. That is an environment policy, not a
   reason to create a web game or to disable security. Two things DO help, and
   both are legitimate:
   - **Sign the build**: `powershell -ExecutionPolicy Bypass -File
     Scripts\sign_build.ps1` signs every executable in `build\<cfg>\bin` with
     a code-signing certificate from `Cert:\CurrentUser\My`. A certificate in
     the machine's Trusted Publishers + Trusted Root stores satisfies the
     policy. A public code-signing certificate is what a public release needs.
   - **Retry**: the machine's cloud reputation service answers a few minutes
     after a file is first seen, and a blocked binary becomes runnable on its
     own. Re-run it later instead of concluding that the test failed.
7. Do not commit `Cache/`, `dist/`, or generated binaries.

## Useful docs

- [README](README.md) — build and feature overview
- [AI game guide](Docs/AI_Game_Guide.md) — copy-ready game loop and scene/Lua
  examples
- [Getting started (AR)](Docs/Getting_Started_Ar.md)
- [Lua scripting (AR)](Docs/Lua_Scripting_Ar.md)
- [Asset import](Docs/Asset_Import.md)
