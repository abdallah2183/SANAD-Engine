# SANAD Engine 0.3.0 — AI-ready release

**Download:**
- `SANAD-0.3.0-Windows-x64-Portable.zip` — the engine: editor, CLI, player, cooker, importer, templates, shaders, Arabic font, docs
- `SANAD-0.3.0-TreeWorld.zip` — the sample game, packaged and ready to play

**SHA-256:**
```
77782ac8f27774bfbae2c82fc2b1c1b750077195a2ef97c1016b4a65a36cfcac  SANAD-0.3.0-Windows-x64-Portable.zip
da2cf2a645dcdd3e9a39373836e999c2807c85535d5c03037d9baccf8b2a7afc  SANAD-0.3.0-TreeWorld.zip
```

Windows 10/11 x64 · Vulkan-capable GPU driver required · Arabic-first UI (English available)

---

## Highlights | أبرز ما في النسخة

### A third-person game, not a fixed camera | لعبة شخص ثالث، مش كاميرا ثابتة
- The camera **follows a player**: a pivot on the character, an orbit
  (distance, yaw, pitch), smoothing that is frame-rate independent, and a
  collision sphere that pulls the camera in front of a tree instead of
  leaving it inside one.
- The camera is set in the scene — `Camera: … follow=Player pivot=0,1.45,0
  distance=5.2 yaw=180 pitch=-13 smoothing=9 radius=0.35` — and in the
  editor's Camera panel.
- A headless run has no swapchain to present to, so it reports **zero
  draws** whatever the scene contains. The windowed run is what proves the
  rendering: `extracted=60 visible=40 draws=80`.

### A character with a skeleton and a walk cycle | شخصية بهيكل عظمي ومشي
- The explorer is **one entity with a rig**: a glTF that carries a skin, 19
  bones and three clips (idle, walk, run). The engine skins it on the CPU
  and plays the clip that matches how fast it is moving.
- The character was seven ordinary entities (a body, a head, hair, two arms,
  two legs) and a Lua script that moved them by hand every frame.
- The rig and the props are **authored as code** (`Tools/Character`,
  `Tools/Props`), so the assets are reproducible, reviewable as code, and
  carry no third-party licence: they are ours, and they are CC0 like the
  rest of the repository.

### A forest that is not a cube and three spheres | غابة مش مكعبات وكرات
- The trees are a tapered eight-sided trunk that leans, with three
  branches, and a canopy of four overlapping crowns. The rocks are two
  jittered, flat-shaded blobs.

### An agent can build a playable game | الوكيل يقدر يبني لعبة قابلة للعب
- `AGENTS.md` at the repository root and in every project `nf new` creates:
  the rules, the paths, the Lua API, and the acceptance command.
- `Docs/AI_Game_Guide.md` is a copy-ready game loop: scene, script, camera
  math, win condition.
- The CLI grew `nf new --template`, `nf build`, `nf run`, `nf verify`, and
  the templates really scaffold — Default, ThirdPerson, FPSStarter,
  Platformer2D.
- The Third-Person starter is a **playable Crate Hunter** game: WASD to
  move, crates to collect, a score.

### The editor does not lose your work | المحرر ما يضيع شغلك
- `Ctrl+D` duplicates an entity **and its whole subtree**.
- An automatic copy is written before every scene switch, and *Recover Last
  Draft* brings it back.
- A rename now moves the registry entry and the cooked copy with the file;
  a delete purges them too. The browser shows the file's own name.

### The foundations, fixed | الأساسات اتظبطت
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
   forest in the editor and watch the camera follow.

## What's inside

- `SANADEditor.exe`, `NFPlayer.exe`, `NFGamePlayer.exe`, `nf.exe`,
  `NFAssetCooker.exe`, `NFModelImporter.exe`
- `Templates/` — Default, ThirdPerson, FPSStarter, Platformer2D, Blender
- `Games/TreeWorld/` — the sample game (source; its `Cache/` and `dist/` are
  not shipped)
- `Shaders/` — the compiled SPIR-V programs,
  `Resources/fonts/Amiri-Regular.ttf`
- `Docs/` — release notes, Arabic getting-started, tutorial, Lua scripting,
  asset import, the AI game guide, Blender pipeline
- `AGENTS.md`, `CLAUDE.md`, `llms.txt` — the agent entry points
- `README.txt` — quick start printed beside the executable

## Known issues | قيود معروفة

- **Windows x64 only.** There is no Linux or macOS player.
- **The binaries are unsigned.** Windows **Smart App Control** or an
  enterprise **WDAC** policy may refuse to launch them — including the
  ones a build just produced. Sign the build (`Scripts\sign_build.ps1`,
  or `build_installer.ps1 -Sign`) with a certificate the machine trusts,
  or allow the folder. This is a machine policy, not a defect: do not
  disable your security to run a demo.
- **The camera cannot be aimed yet.** It follows a fixed orbit; a mouse
  look and a player-driven yaw come next.
- The tree, the rocks and the character are stylised; real scanned assets
  are not in.
- Autosave is a recovery file, not a versioned backup system.

## Verification | التحقق

Everything below was run on the release machine, Release, Windows x64, and
every number is what the binaries actually printed.

- **Clean build from scratch:** `rmdir /s /q build` then
  `build_nf.bat Release` → **0 errors**, 700 build steps, 41 executables.
- **All 26 test suites: 1,923 passed / 0 failed / 3 skipped.**
- **TreeWorld, played by the shipped player:**
  - headless — 120 frames, **Validation errors: 0**, **Alive RHI objects
    before shutdown: 0**, `SANAD Runtime exited cleanly`;
  - windowed — 120 frames with **`extracted=60 visible=40 draws=80`**: the
    scene really is drawn, and the character is skinned and animated
    (`Animated characters: 1 (max mesh deviation 0.0018)`).
- **The portable zip**, extracted into a folder that has never seen the
  engine: the shipped CLI creates a Third-Person project, builds it, and
  the shipped player plays it — **Validation errors: 0**, **0 leaked RHI
  objects**.
- **Editor acceptance, headless:** every automation step OK, 0 Vulkan
  validation errors, 0 leaked RHI objects.

> **CI could not run for this release.** GitHub reported: *"The job was not
> started because your account is locked due to a billing issue."* Every
> workflow on the repository — including the one that was green before —
> stopped the same way. Fixing the account's billing restores the CI gates,
> which are already wired up: the full 26-suite run on a machine without a
> code-integrity policy, the TreeWorld acceptance in both its headless and
> its rendering form, and the portable-zip acceptance.
