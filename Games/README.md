# Games made with SANAD

These are complete `.nfproj` projects built with the engine — not engine tests
and not web demos. Each folder is self-contained source (scene text, Lua,
materials, textures); `Cache/` and `dist/` are generated and ignored.

| Game | What it shows | Run |
|:---|:---|:---|
| [TreeWorld](TreeWorld/) | Third-person character, misty forest, 63 entities, Lua input, fog/post-processing, procedural bark/leaf textures, five collectible fireflies | `nf run --project Games\TreeWorld\TreeWorld.nfproj` |

```bat
:: from the engine repository root
build\Release\bin\nf.exe build --project Games\TreeWorld\TreeWorld.nfproj
build\Release\bin\nf.exe run   --project Games\TreeWorld\TreeWorld.nfproj

:: headless acceptance
build\Release\bin\nf.exe run --project Games\TreeWorld\TreeWorld.nfproj --frames 120 --headless --validation
```

A game here is **native**: Windows x64 + Vulkan, built by `nf`. Do not add an
`index.html`, a web server, or a Unity/Godot project.
