#include <NF/Project/ProjectScaffold.hpp>

#include <NF/Assets/ProjectDescriptor.hpp>

#include "PathUtils.hpp"

#include <filesystem>
#include <string>

namespace nf::project {

namespace {

// Written into every new project. Coding agents open a project folder far more
// often than they read the engine README, and the failure this prevents is the
// expensive one: an agent that sees "make a game" and builds an HTML/JS game
// beside a native Vulkan project. Keep this factual and executable.
constexpr const char* kProjectAgentGuide = R"GUIDE(# AGENTS.md — {PROJECT} (SANAD Engine project)

You are inside a **native SANAD Engine game project**, not a web project.

## Hard rule

**Do not create an HTML/CSS/JavaScript game, a package.json, a web server, an
Electron app, or a Unity/Godot project.** A SANAD game is a `.nfproj` project
with `.nfscene` scenes, Lua scripts, and a native Windows player built by `nf`.
If a task says "make a game", make it **with this engine**, in this folder.

## Run it

Project file: `{PROJECT_FILE}`

- If `nf` is on `PATH`:
  ```bat
  nf build --project "{PROJECT_FILE}"
  nf run   --project "{PROJECT_FILE}"
  ```
- Otherwise find the engine build's `nf.exe` (usually
  `<engine>\build\Release\bin\nf.exe`) and run the same commands with its full
  path.
- Editor: `SANADEditor.exe --project "{PROJECT_FILE}"`.
- Headless smoke test (no window): add `--frames 120 --headless --validation`
  to `nf run`; it must exit 0 and the log must say `Validation errors: 0`.

## What a game is made of

```
{PROJECT_FILE}
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
)GUIDE";

bool is_valid_name(const std::string& name) {
    if (name.empty()) return false;
    // A name containing a separator would put the .nfproj somewhere other than
    // the project root, which the mount defaults would then disagree with.
    return name.find_first_of("/\\:") == std::string::npos && name != "." && name != "..";
}

} // namespace

std::string project_template_name_list() {
    std::string result;
    for (std::size_t i = 0; i < kProjectTemplateNames.size(); ++i) {
        if (i != 0) result += ", ";
        result += kProjectTemplateNames[i];
    }
    return result;
}

std::filesystem::path resolve_project_template(std::string_view name,
                                               const std::filesystem::path& templates_root,
                                               const std::filesystem::path& default_template_dir,
                                               std::string& out_error) {
    out_error.clear();
    if (name.empty()) return default_template_dir;

    for (const std::string_view allowed : kProjectTemplateNames) {
        if (name != allowed) continue;

        if (templates_root.empty()) {
            out_error = "template '" + std::string(allowed) +
                        "' is unavailable because the templates root is not configured; valid templates: " +
                        project_template_name_list();
            return {};
        }

        const auto selected = (templates_root / allowed).lexically_normal();
        std::error_code ec;
        if (!std::filesystem::is_directory(selected, ec) || ec) {
            out_error = "template '" + std::string(allowed) + "' is not available at '" +
                        selected.string() + "'; valid templates: " + project_template_name_list();
            return {};
        }
        return selected;
    }

    out_error = "unknown template '" + std::string(name) + "'; valid templates: " +
                project_template_name_list();
    return {};
}

bool scaffold_project(const ScaffoldOptions& opts, std::string& out_error) {
    if (!is_valid_name(opts.name)) {
        out_error = "invalid project name '" + opts.name +
                    "' (no path separators, no leading dot)";
        return false;
    }
    if (opts.root.empty()) {
        out_error = "project root is empty";
        return false;
    }

    const auto root = opts.root.lexically_normal();
    const auto project_file = root / (opts.name + assets::ProjectDescriptor::kExtension);

    std::error_code ec;
    if (std::filesystem::exists(project_file, ec)) {
        // Refuse rather than clobber: replacing a project file is not something
        // the user can undo by re-running the command.
        out_error = "a project already exists at '" + project_file.string() + "'";
        return false;
    }

    std::filesystem::create_directories(root, ec);
    if (ec) {
        out_error = "failed to create '" + root.string() + "': " + ec.message();
        return false;
    }

    if (!opts.template_dir.empty()) {
        if (!detail::copy_tree(opts.template_dir, root, out_error)) {
            return false;
        }
    }

    // The directories the descriptor's mounts point at. Created up front so a
    // freshly scaffolded project can be cooked and run immediately.
    for (const char* sub : {"Cache", "Shaders", "dist", "Content"}) {
        std::filesystem::create_directories(root / sub, ec);
        if (ec) {
            out_error = "failed to create '" + (root / sub).string() + "': " + ec.message();
            return false;
        }
    }

    auto desc = assets::ProjectDescriptor::make_default(root, opts.name);
    desc.set_title(opts.name);
    desc.set_startup_scene("content://Scenes/Main.nfscene");
    if (!desc.save_to_file(project_file, out_error)) {
        return false;
    }

    // Generated output must not be committed. Without this the first `nf build`
    // drops a Cache/ and a dist/ into the user's repository.
    if (!detail::write_text(root / ".gitignore", "Cache/\ndist/\n*.nfproj.tmp\n", out_error)) {
        return false;
    }

    // Agent-facing instructions travel with the project, not just the engine
    // repo: an AI opened in this folder may never see the engine README.
    std::string guide = kProjectAgentGuide;
    const std::string project_token = "{PROJECT}";
    const std::string path_token = "{PROJECT_FILE}";
    for (std::size_t at = guide.find(project_token); at != std::string::npos;
         at = guide.find(project_token, at + opts.name.size())) {
        guide.replace(at, project_token.size(), opts.name);
    }
    const std::string project_path = project_file.string();
    for (std::size_t at = guide.find(path_token); at != std::string::npos;
         at = guide.find(path_token, at + project_path.size())) {
        guide.replace(at, path_token.size(), project_path);
    }
    if (!detail::write_text(root / "AGENTS.md", guide, out_error)) {
        return false;
    }

    return true;
}

} // namespace nf::project
