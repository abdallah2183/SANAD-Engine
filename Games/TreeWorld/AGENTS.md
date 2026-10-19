# AGENTS.md — TreeWorld (SANAD Engine project)

You are inside a **native SANAD Engine game project**, not a web project.

## Hard rule

**Do not create an HTML/CSS/JavaScript game, a package.json, a web server, an
Electron app, or a Unity/Godot project.** A SANAD game is a `.nfproj` project
with `.nfscene` scenes, Lua scripts, and a native Windows player built by `nf`.
If a task says "make a game", make it **with this engine**, in this folder.

## Run it

Project file: `TreeWorld.nfproj`

- If `nf` is on `PATH`:
  ```bat
  nf build --project "TreeWorld.nfproj"
  nf run   --project "TreeWorld.nfproj"
  ```
- Otherwise find the engine build's `nf.exe` (usually
  `<engine>\build\Release\bin\nf.exe`) and run the same commands with its full
  path.
- Editor: `SANADEditor.exe --project "TreeWorld.nfproj"`.
- Headless smoke test (no window): add `--frames 120 --headless --validation`
  to `nf run`; it must exit 0 and the log must say `Validation errors: 0`.

## What a game is made of

```
TreeWorld.nfproj
Content/
  Scenes/       .nfscene text files (entities + components)
  Scripts/      .lua files with update(dt)
  Meshes/       .nfmesh assets
  Materials/    .nfmat assets
  Textures/
Cache/          cooked assets (generated)
dist/           packaged player (generated)
```

The default startup scene is `Content/Scenes/Main.nfscene`. Add gameplay in
`Content/Scripts/*.lua` and attach a script to an entity with:

```
Script: lang=lua path=content://Scripts/my_game.lua
```

## Lua API (sandboxed Lua 5.4)

```lua
function update(dt) end          -- required; called every frame
self_id, self_gen                 -- the entity running this script
nf.self()                         -- same, returns id, gen
nf.entity_pos(id, gen)            -- x, y, z (nil when no Transform)
nf.set_entity_pos(id, gen, x,y,z)
nf.set_entity_scale(id, gen, sx,sy,sz)  -- zero hides a static crate
nf.entity_alive(id, gen)
nf.find_entity("CrateA")          -- id, gen, or nil
nf.entity_name(id, gen)
nf.entity_count()
nf.action("move_forward")         -- W / Up, also move_back/left/right/jump/interact
nf.axis("move_x")                 -- -1..1, also move_z
nf.clamp(v, lo, hi)
nf.lerp(a, b, t)
print(...)                        -- goes to the engine log
```

`nf.action` and `nf.axis` use the engine's named actions:
`move_forward`, `move_back`, `move_left`, `move_right`, `move_x`, `move_z`,
`jump`, `sprint`, `interact`.

## 60-second playable example

`Content/Scripts/player.lua`:

```lua
local speed = 4.0
local score = 0
local targets = { "CrateA", "CrateB" }
local collected = {}

function update(dt)
    local x, y, z = nf.entity_pos(self_id, self_gen)
    if not x then return end

    local mx = nf.axis("move_x")
    local mz = nf.axis("move_z")
    nf.set_entity_pos(self_id, self_gen, x + mx * speed * dt, y, z + mz * speed * dt)

    for _, name in ipairs(targets) do
        local id, gen = nf.find_entity(name)
        if id and not collected[name] then
            local tx, ty, tz = nf.entity_pos(id, gen)
            if tx then
                local dx, dz = tx - x, tz - z
                if dx * dx + dz * dz < 1.5 * 1.5 then
                    nf.set_entity_scale(id, gen, 0.001, 0.001, 0.001)
                    collected[name] = true
                    score = score + 1
                    print("score", score)
                    if score == #targets then
                        print("YOU WIN")
                    end
                end
            end
        end
    end
end
```

Attach it to the Player entity in `Main.nfscene`. A playable result is more
important than a design document; run it before claiming success.

## Scene format essentials

```
# SANAD Scene v1
version: 1
name: Main
entity_count: 2
---
entity: 1:0
  Name: Player
  Transform: local(0,1,0) world(0,1,0) rot(0,0,0) scale(1,1,1) parent(4294967295:0)
  Mesh: asset_id=<uuid> material=content://Materials/Default
  Script: lang=lua path=content://Scripts/player.lua
---
entity: 2:0
  Name: MainCamera
  Transform: local(0,4,8) world(0,4,8) rot(0,0,0) scale(1,1,1) parent(4294967295:0)
  Camera: fov=60 aspect=1.7777778 near=0.1 far=1000 active=true
```

`entity_count` must match the number of `entity:` blocks. Keep every entity
inside the camera's view unless you also author a camera.

## Rules for agents

1. Do not replace this project with HTML or a different engine.
2. Do not edit `Engine/` unless the request is explicitly an engine feature.
3. Change the smallest files that achieve the game: scene + one Lua script.
4. Build and run before reporting success. A window that opens and exits 0 is
   the acceptance test; a file that merely exists is not.
5. If a command fails, read the log and fix it. Never claim a pass you did not
   observe.
6. Windows x64 + Vulkan is the target. There is no browser build.
