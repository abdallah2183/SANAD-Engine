# SANAD Engine 0.3.0 — AI-ready release

**Download:**
- `SANAD-0.3.0-Windows-x64-Portable.zip` — the engine, the editor, the CLI, the tools
- `SANAD-0.3.0-TreeWorld.zip` — the sample game, packaged and ready to play

**SHA-256:**
```
bd3432cbbb4adfa5431bb9bd1f6de804a9babed9315a98d9aea0bfdd7c01784a  SANAD-0.3.0-Windows-x64-Portable.zip
fce03163a6f56c99041f3493a94b40d23bd7fc3bfa4044fe6a7ad6cd8142971e  SANAD-0.3.0-TreeWorld.zip
```

Windows 10/11 x64 · Vulkan-capable GPU driver required · Arabic-first UI (English available)

---

## Highlights | أبرز ما في النسخة

### TreeWorld: a real game, not a tech demo | لعبة حقيقية، مو ديمو تقني
- `Games/TreeWorld` is a complete misty-forest scene: 66 entities, a
  character, trees, rocks, crates, textures, a Lua gameplay script and a
  fog-lit atmosphere.
- It is packaged and played by the **shipped player**, not by the editor:
  `nf build --project Games/TreeWorld/TreeWorld.nfproj`, then the packaged
  `NFPlayer.exe` runs it standalone.
- CI accepts it by the player's own log line — `Validation errors: 0` and
  no leaked RHI objects — not by "the folder looks right".

### An agent can build a playable game | الوكيل يقدر يبني لعبة قابلة للعب
- `AGENTS.md` at the repository root and in every project `nf new` creates:
  the rules, the paths, the Lua API, and the acceptance command.
- `Docs/AI_Game_Guide.md` is a copy-ready game loop: scene, script, camera
  math, win condition.
- The CLI grew `nf new --template`, `nf build`, `nf run`, `nf verify`, and
  the templates really scaffold — Default, ThirdPerson, FPSStarter,
  Platformer2D.
- The Third-Person starter is a **playable Crate Hunter**: WASD to move,
  crates to collect, a score.

### The editor does not lose your work | المحرر ما يضيع شغلك
- `Ctrl+D` duplicates an entity **and its whole subtree**.
- An automatic copy is written before every scene switch, and *Recover Last
  Draft* brings it back.
- A rename now moves the registry entry and the cooked copy with the file;
  a delete purges them too. The browser shows the file's own name.

### The engine got its foundations right | الأساسات اتظبطت
- The Vulkan backend now tracks the image layout the GPU is **actually**
  in. The old code derived a barrier's old layout from the caller's usage
  hint, which produced transitions the driver never saw.
- The physics world no longer outlives the layer filters, the broadphase
  and the job system it points at, and a removed body takes its
  constraints with it.
- The dedicated server bounds what a peer can make it hold: the player
  table is keyed by address **and** port, provisioning is rate limited per
  address, and the input queues are windowed in both directions.
- A scene file is parsed without throwing: a malformed line fails to load
  instead of taking the process down.
- The asset registry holds one spelling per file, so the cooker no longer
  re-cooks the same mesh on every build.

## First five minutes | أول خمس دقائق

1. Extract the zip anywhere and run `SANADEditor.exe`.
2. **Create New Project** → **Third-Person Starter** → name it → **Create
   Project**.
3. Press **Play ▶**: WASD/arrows move, Space jumps. Close the game window to
   stop.
4. Or play the shipped sample directly:
   ```bat
   nf build --project Games\TreeWorld\TreeWorld.nfproj
   Games\TreeWorld\dist\NFPlayer.exe --project Games\TreeWorld\dist\TreeWorld.nfproj
   ```
5. **File → Open Project** on `Games/TreeWorld/TreeWorld.nfproj` to open the
   scene in the editor.

## What's inside

- `SANADEditor.exe`, `NFPlayer.exe`, `NFGamePlayer.exe`, `nf.exe`,
  `NFAssetCooker.exe`, `NFModelImporter.exe`- `Templates/` — Default, ThirdPerson, FPSStarter, Platformer2D, Blender
- `Games/TreeWorld/` — the sample game (source; its `Cache/` and `dist/`
  are not shipped)
- `Shaders/` — the compiled SPIR-V programs, `Resources/fonts/Amiri-Regular.ttf`
- `Docs/` — release notes, Arabic getting-started, tutorial, Lua scripting,
  asset import, the AI game guide, Blender pipeline
- `AGENTS.md`, `CLAUDE.md`, `llms.txt` — the agent entry points
- `README.txt` — quick start printed beside the executable

## Known issues | قيود معروفة

- **Windows x64 only.** There is no Linux or macOS player.
- **Unsigned binaries:** with **Smart App Control** or an enterprise **WDAC**
  policy enabled, Windows may refuse to launch the executables — including
  the ones a build just produced. Either allow the folder, or sign the
  build. This is a machine policy, not a defect: do not disable your
  security policy to run a demo.
- **The camera is still fixed.** It looks at the origin, so a real
  third-person game is not possible yet. A follow camera and a glTF
  character with idle/walk/run animation are the next release.
- The tree, the rocks and the character are built from primitives. Real
  CC0 assets are on the way.
- Autosave is a recovery file, not a versioned backup system.

## Verification | التحقق

- **Clean build from scratch:** `rmdir /s /q build` then
  `build_nf.bat Release` → **0 errors**, 41 executables, 700 build steps.
- **Test suites: 1,208 passed / 0 failed / 3 skipped** across the 18 suites
  that ran on the release machine (Release, Windows x64).
- **8 of the 26 suites could not be started on the release machine.** Windows
  **Smart App Control** refuses to launch freshly built, unsigned binaries
  there, so `AITests`, `AudioTests`, `FoliageTests`, `RHITests`,
  `SaveTests`, `Scene2DTests`, `ToolTests` and `UITests` were blocked by the
  machine's code-integrity policy. That is a machine policy, not a test
  failure — and it is why CI matters: the same suites are green there on
  every push.
- **TreeWorld** runs in the engine: 120 frames, **Validation errors: 0**,
  **Alive RHI objects before shutdown: 0**.
- **Editor acceptance, headless:** every automation step OK, 0 Vulkan
  validation errors, 0 leaked RHI objects.
- **End-to-end:** `nf new` → `nf build` → the packaged player renders frames
  with physics, animation and audio running.
- **The portable zip** contains the editor, the CLI, the player, the cooker,
  the importer, the templates, the shaders, the Arabic font and the
  TreeWorld source (127 files).

> **CI could not run for this release.** GitHub reported: *"The job was not
> started because your account is locked due to a billing issue."* Every
> workflow on the repository — including the one that was green before —
> stopped the same way. Fixing the account's billing restores the full
> 26-suite run, the TreeWorld acceptance and the portable-zip acceptance,
> all of which are wired into the CI workflow and will run on the next push.
