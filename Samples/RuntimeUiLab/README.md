# Runtime UI Lab

A native SANAD example that exercises custom Lua UI in the exported Windows player. It is a general engine test, separate from the Space Apps mission-design project.

From the repository root, using the current development build:

```powershell
.\build\Release\bin\nf.exe build --project Samples/RuntimeUiLab/RuntimeUiLab.nfproj
.\Samples\RuntimeUiLab\dist\NFPlayer.exe --project Samples/RuntimeUiLab/dist/RuntimeUiLab.nfproj
```

## In the editor\n\nOpen the project in `SANADEditor.exe`, press **Play** (toolbar or `Ctrl+P`), and use the Viewport panel as the game — the same buttons, slider, list and Profile page render there. **Pause** (`P`) freezes inspection; **Stop** (`Esc` when no field is editing, or the banner button) returns to editing. Click an Outliner/Inspector panel to regain editor control while playing.\n\nClick **Charge**, **Drain**, **Pause** and **Reset**. Drag the **Load** slider. The charge bar and cube respond to the Lua state. The same screen displays shaped Arabic text. Use the tabs for **Main**, **Settings** (checkbox + spin slider) and **Demo** (selectable list). Tab/Enter/arrows/wheel work too. The anchored panel stays inside the window at 800×600, 1280×720 and 1920×1080; long labels wrap inside their rows.

For a bounded run, append `--frames 300 --validation`. For a smoke test without a window, append `--frames 120 --headless --validation`. Maximizing, resizing, minimizing and restoring were verified against this build under `--validation` (`Validation errors: 0`); minimizing parks the loop without a `dt` jump on restore.

- [Lua script](Content/Scripts/example.lua)
- [Scene](Content/Scenes/Main.nfscene)
- [Runtime UI API](../../Docs/Runtime_UI.md)

Generated `Cache/`, `Shaders/` and `dist/` are build artifacts. The portable output includes its UI shaders, Arabic font and font license. Verified by running the copied `dist/` outside the repository (headless and windowed, both `Validation errors: 0`).
