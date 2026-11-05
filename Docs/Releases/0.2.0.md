# SANAD Engine 0.2.0 — Demo Release

**Download:** `SANAD-0.2.0-Windows-x64-Portable.zip` (7,109,607 bytes)
**SHA-256:** `AEDED0E0739955C4AA8FE254596699470A9E23CE79667740BF383DBAE7C0996C`

Windows 10/11 x64 · Vulkan-capable GPU driver required · Arabic-first UI (English available)

---

## Highlights | أبرز ما في النسخة

### Editor that tells you what to do | محرر يقول لك تعمل إيه
- The toolbar now draws at all (it was silently invisible), with New / Open /
  Save / Add Ground / Add Cube / Play.
- A first-run **Start Here** card: New scene → Add ground → Play, each one click.
- An empty project library opens **Create New Project** directly, with the
  playable **Third-Person Starter** selected.
- The debug census card and `viewport lit=` diagnostic are hidden by default
  (View → Viewport stats brings them back).

### Your work is not lost | شغلك ما يضيع
- Closing a modified scene asks **Save / Discard / Cancel**.
- An automatic copy is always written to `Cache/EditorRecovery.nfscene` before
  the prompt; an Untitled scene saves to a timestamped
  `Content/Scenes/Recovered_*.nfscene` instead of overwriting.
- `Play` brings the game window to the front (it used to open behind the editor).

### Projects work from any folder | المشاريع تعمل من أي مجلد
- Templates resolve beside the executable (`<exe>/Templates`), not from the
  build machine's absolute path. The old portable layout could not create a
  project at all on another PC.
- A freshly scaffolded project renders without running `nf cook` first: meshes
  fall back to the authored `content://` copy when `cache://` is empty.
- File > New cooks its ground mesh **and saves the asset registry**, so the
  floor no longer disappears after a restart.

### UI | الواجهة
- Neutral chrome with one blue accent; the brown/gold fills across every button
  and header are gone.
- Selected rows and section headers read clearly; the launcher uses the same
  blue identity.

## First five minutes | أول خمس دقائق

1. Extract the zip anywhere and run `SANADEditor.exe`.
2. **Create New Project** → **Third-Person Starter** → name → **Create Project**.
3. Shape the scene (toolbar cube/ground, W/E/R gizmos), press **Play ▶**:
   WASD/arrows move, Space jumps. Close the game window to stop.
4. `Ctrl+S` saves.
5. Standalone build:
   ```bat
   nf new MyGame --template ThirdPerson
   nf build --project MyGame\MyGame.nfproj
   MyGame\dist\NFPlayer.exe
   ```

## What's inside

- `SANADEditor.exe`, `NFGamePlayer.exe`, `nf.exe`, `NFAssetCooker.exe`,
  `NFModelImporter.exe`
- `Templates/` — Default, ThirdPerson, FPSStarter, Platformer2D, Blender
- `Shaders/` — 22 SPIR-V programs, `Resources/fonts/Amiri-Regular.ttf`
- `Docs/` — Demo release notes, Arabic getting-started, tutorial, Lua scripting,
  asset import, Blender pipeline, repository README, roadmap
- `README.txt` — quick start printed beside the executable

## Known issues | قيود معروفة

- **Windows x64 only.**
- **Unsigned binaries:** with **Smart App Control** or an enterprise **WDAC**
  policy enabled, Windows may refuse to launch `SANADEditor.exe`. Either allow
  the folder, turn Smart App Control off (Windows Security → App & browser
  control), or sign the build. Code signing is required before a broad public
  release.
- The demo scene's lighting/materials are intentionally simple; visual polish
  lands after this demo.
- Autosave is a recovery file, not a versioned backup system.
- The installer (Setup.exe) is not part of this demo; use the portable zip.

## Verification | التحقق

- Release build of the editor, player, CLI, tools, samples and tests: clean.
- **26 test suites: 1875 passed / 0 failed / 2 skipped.**
- Portable zip extracted to a clean folder: `nf.exe` created a complete
  Third-Person project (scene + player script) **with the developer Templates
  folder renamed away**, and the packaged `NFGamePlayer` rendered 10 frames and
  exited cleanly.
- Headless editor run: 0 Vulkan validation errors, 0 leaked RHI objects.
