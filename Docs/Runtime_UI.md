# Scripted runtime UI

This feature ships in SANAD 0.9.0. It renders a game's Lua UI in the standalone `NFPlayer` and — while **playing in the editor** — in the **Game View (Viewport panel)**, with the same widgets, shaping, fonts and interaction. The editor path does not need the editor to *author* UI; any project with `nf.ui` scripts previews here exactly as shipped.

[Documentation index](README.md) · [Runnable UI Lab](../Samples/RuntimeUiLab/README.md)

## Run the example

From the engine repository root, after a Release build:

```powershell
.\build\Release\bin\nf.exe build --project Samples/RuntimeUiLab/RuntimeUiLab.nfproj
.\Samples\RuntimeUiLab\dist\NFPlayer.exe --project Samples/RuntimeUiLab/dist/RuntimeUiLab.nfproj
```

The lab displays English and Arabic text, charge/drain/reset buttons, a pause button, a charge indicator and a load slider. Its Lua script also changes a cube's scale. It is a general engine example, independent of the Space Apps competition project.

## Try it in the editor Game View

1. Open `Samples/RuntimeUiLab/RuntimeUiLab.nfproj` in `SANADEditor.exe`.
2. Press **Play** (toolbar triangle, **Game** menu, or `Ctrl+P`). The Viewport panel becomes the game: the cube, physics and `nf.ui` panels render there.
3. Click inside the game image to use the widgets. Type Arabic/English in **Profile** (`text_input`), edit the bounded integer (`number_input`), drag the **Load** slider, Tab between fields, and use the wheel on the **Demo** list.
4. **Pause** (`P` or the banner button) freezes the simulation for inspection; **Stop** (banner, `Esc` when no field is editing, or `Ctrl+P`) returns to editing.

While playing, editor gizmo/navigation and scene shortcuts are disabled and clicks/keys inside the game image belong to the game. To regain editor control: click an editor panel (Outliner/Inspector) or press **Stop**. If a text field is editing, `Esc` cancels the edit (it does not stop); click the game image and press `Esc` again, or use the **Stop** button.

`Pause` means frozen for inspection: `Runtime::update(0)` runs, so no simulation or script steps, the last `ScriptCanvas` snapshot keeps rendering, and input is cancelled on entry (`cancel_interaction`), so nothing can queue a hidden mutation that would surface on resume. `Stop` (and Play, and scene changes) clears UI focus/editing and Lua environments, restoring the pre-play edit scene. Preview never writes settings on its own — `nf.settings.save` only fires when Lua asks for it.

Game View layout uses the game content size (the offscreen target), not the window size. The aspect-fit letterbox inside the panel is the only interactive/drawn area; clicks in the black bars are ignored (`game_screen_to_normal` returns false). Drawing and hit-testing use the same rect, so at small and large dock sizes the UI never drifts from the scene. `NFRendering::draw_display_list` is the shared draw path (shape-once, measured wrap, per-line align) used by both `GameUiOverlay` (shipped player) and the Game View (editor context) — no second ImGui context, no extra `Render` that would break the editor frame.

## Play / Pause / Stop and input ownership

The keyboard/mouse have one explicit owner per frame (`Editor/include/NF/Editor/InputOwnership.hpp`): **Editor**, **Game** (game UI Tab-focused), or **GameField** (text/number editing). `input_owner` is a pure function of playing/paused/open/focused/foreground/popup state. `main.cpp` uses it to gate editor shortcuts, suspend ImGui Nav (Tab/arrows) for the frame, and decide pointer/key eligibility. The Game View banner always shows the Stop button and the owner hint: `P` pause, `Esc` cancels the edit first, then `Esc` stops.

- **Pause is true.** `Runtime::set_paused(true)` makes `Runtime::update(dt)` return before scripts, physics, animation, gameplay, AI/nav, particles, weapons, destruction and audio — regardless of the passed `dt`. `dt = 0` alone is not the guard: a script that increments a counter or saves without reading `dt` would still advance. The last `ScriptCanvas` snapshot is kept for frozen inspection; live interaction is cancelled on entry so nothing can queue a hidden mutation. Resume discards pause time (`clock.reset()` on the transition, first live `dt` is one frame) so there is no time jump.
- **Audio on Pause:** the mixer is not stepped, so no new sources are mixed; queued device output drains to silence. There is no pause-resume crossfade.
- **Stop** restores the exact pre-play scene, clears `set_paused`, empties the game UI and Lua cache, and re-enables editor Nav the next frame. Repeated Play/Stop cycles do not accumulate callbacks.
- **Tab/Enter/arrows** belong to `ScriptCanvas` once while Game/GameField owns the keys; ImGui Nav is cleared (`ConfigFlags` Nav bits off + `ClearActiveID` for one frame) so there is no double move. Editor Nav returns automatically when ownership returns to Editor.
- **P/W/E/R** inside a text field type letters — the editor shortcut block (`ui_owns_shortcuts`) swallows the toggle edges — and the gizmo hotkeys in `Panels.cpp` require edit mode. **Escape** cancels the draft first (which also clears game focus); a second `Esc` (or the banner Stop button) stops the game.
- **Focus loss/Pause/Stop** calls `ScriptCanvas::cancel_interaction()`: capture, focus, drafts and held keys are dropped, and `WM_CHAR` bursts are drained on resume so nothing fires a phantom click or stuck key. A drag that leaves the Game View content rect is cancelled, not completed, and the release over an editor panel does not click it.
- **Gamepad:** while a text/number field is editing, `KeyboardInputSource` and `EditorPlayInput` block keys, pad buttons, pad axes and mouse buttons from gameplay in both the standalone player and the editor. There is no new pad navigation layer in this phase; pad input only drives gameplay actions (wiring documented in `KeyboardInput.cpp`).

## Start a game with UI (UiStarter template)

```bat
build\Release\bin\nf.exe new Games\MyGame --name MyGame --template UiStarter
```

`Templates/UiStarter` mirrors `Default` (camera, light, cube, physics, audio) with `Content/Scripts/example.lua` replaced by a main menu, a Settings page (`nf.ui.text_input`, `nf.ui.number_input`, load-once/save-on-demand via `nf.settings`) and a Play button that queues a separate trial scene. The trial activates three beacons before a configurable time limit, then opens Results with retry and menu actions. It uses the existing `nf.ui`/`nf.settings` APIs only — no new UI system — and ships no `Cache/`, `dist/` or personal settings. The default template is unchanged (`nf new` without `--template` still copies `Default`).

## Script API

Call these functions from `update(dt)`. The UI is rebuilt each simulation frame: omit a widget to hide it. Give each widget a stable, nonempty ID. IDs are scoped to the script entity, so two entities may use the same local ID.

All rectangles use normalized client coordinates `(x, y, width, height)`, with `(0, 0)` at the top left and `(1, 1)` at the bottom right. Text is logical UTF-8; the draw backend shapes Arabic once and loads the bundled Amiri font.

| Function | Result |
|:---|:---|
| `nf.ui.panel(id, x, y, w, h)` | Dark background panel |
| `nf.ui.label(id, text, x, y, w, h, size, align)` | Text; optional pixel font size defaults to 22, `align` is `"left"` (default), `"center"` or `"right"` |
| `nf.ui.button(id, text, x, y, w, h, enabled, align)` | `true` once after a press and release on the same enabled button (or Enter/Space when focused) |
| `nf.ui.bar(id, text, fraction, x, y, w, h)` | Progress indicator; fraction is clamped to 0–1 |
| `nf.ui.slider(id, text, value, min, max, x, y, w, h)` | Updated value; assign the result to game state (Left/Right when focused, 5% steps) |
| `nf.ui.checkbox(id, text, checked, x, y, w, h, enabled)` | New checked state; assign the result to game state |
| `nf.ui.list(id, items, selected, x, y, w, h, visible_rows, enabled)` | Selected index (1-based) and count; `items` is a Lua array of strings, `visible_rows` defaults to 4 |
| `nf.ui.tooltip(id, text, x, y, w, h)` | Tooltip when the rect is hovered; measured with the draw font, max 360px wide, clamped inside the window |
| `nf.ui.text_input(id, value, x, y, w, h, enabled, max_len)` | Returns `new_value, committed, editing`; the caller owns `value`. Mouse/Tab focus, cursor, Home/End, arrows, Backspace/Delete. Enter commits, Escape cancels. `max_len` caps codepoints (0 = 4096, absolute cap 16384 bytes). Characters arrive via `WM_CHAR` (surrogate pairs combined). While editing, all keyboard keys (including WASD) are blocked from gameplay. |
| `nf.ui.number_input(id, label, value, min, max, x, y, w, h, enabled, step, integer_mode)` | Returns `new_value, committed, valid, error` (`error` is `nil` when valid). Interim drafts (`""`, `"-"`) never overwrite `value`. Up/Down steps by `step` and commits. Parsing is locale-independent (`from_chars`); NaN/Inf/overflow and out-of-range are invalid. `integer_mode=true` accepts integers only. The invalid status is shown in red under the field. |
| `nf.ui.anchor_rect(anchor, w, h, margin, min_w_px, min_h_px)` | Returns `x, y, w, h`; `anchor` is `"topleft"`, `"topright"`, `"bottomleft"`, `"bottomright"`, `"topcenter"`, `"bottomcenter"`, `"leftcenter"`, `"rightcenter"` or `"center"` |
| `nf.ui.cursor(visible)` | Requests a free cursor for this frame |
| `nf.settings.save(table)` | Persists string/number/boolean keys to the per-project user folder; returns `ok, error`. Lua cannot pass paths — only keys. |
| `nf.settings.load()` | Returns `table, ok, error`; missing file returns an empty table + `ok=true`. Corrupt/oversized/future-version returns `ok=false` and keeps the caller's defaults. |
| `nf.settings.clear()` | Removes the project settings file; returns `ok, error`. |
| `nf.settings.path()` | Returns the settings file path (for display/debugging). |

```lua
local charge = 50
local unlocked = false
local mode = 1
function update(dt)
    nf.ui.cursor(true)
    local x, y, w, h = nf.ui.anchor_rect("topleft", 0.42, 0.88, 0.025, 300, 220)
    nf.ui.panel("background", x, y, w, h)
    if nf.ui.button("charge", "Charge", x + 0.03, y + 0.06, 0.30, 0.08) then
        charge = math.min(100, charge + 10)
    end
    unlocked = nf.ui.checkbox("opt", "Unlocked", unlocked, x + 0.03, y + 0.16, 0.30, 0.06)
    mode, _ = nf.ui.list("modes", {"Orbit", "Pulse", "Wave"}, mode, x + 0.03, y + 0.24, 0.30, 0.20, 3)
    nf.ui.label("arabic", "مختبر واجهات الألعاب", x + 0.03, y + 0.48, 0.30, 0.06, 18, "right")
    nf.ui.bar("level", "Charge: " .. charge .. "%", charge / 100, x + 0.03, y + 0.56, 0.30, 0.06)
    nf.ui.tooltip("tip", "Click to charge", x + 0.03, y + 0.06, 0.30, 0.08)
end
```

Text and number editing plus per-project persistence (RuntimeUiLab's Profile page):

```lua
local player_name = "Player"
local volume = 80
function update(dt)
    nf.ui.cursor(true)
    local x, y, w, h = nf.ui.anchor_rect("topleft", 0.42, 0.88, 0.025, 300, 220)
    nf.ui.panel("background", x, y, w, h)
    local new_name, committed, editing = nf.ui.text_input("name", player_name, x + 0.03, y + 0.06, w - 0.06, 0.06, true, 48)
    if committed then player_name = new_name end
    local new_vol, ncommitted, nvalid, nerr = nf.ui.number_input("vol", "Volume", volume, 0, 100, x + 0.03, y + 0.14, w - 0.06, 0.06, true, 5, true)
    if ncommitted and nvalid then volume = new_vol end
    if nerr then nf.ui.label("err", tostring(nerr), x + 0.03, y + 0.21, w - 0.06, 0.04) end
    if nf.ui.button("save", "Save", x + 0.03, y + 0.26, 0.14, 0.06) then
        local ok, err = nf.settings.save({player_name = player_name, volume = tostring(volume)})
    end
    if nf.ui.button("load", "Load", x + 0.20, y + 0.26, 0.14, 0.06) then
        local saved, ok, err = nf.settings.load()
        if ok and saved then player_name = saved.player_name or player_name end
    end
end
```

## Input and lifecycle

A press captures the topmost interactive widget from the preceding frame. In the editor, pointer/wheel/text are eligible only while playing, unpaused, and the mouse is inside the Game View content rect (bars own nothing); elsewhere the game receives an unfocused frame and the editor keeps its shortcuts. While the game UI has keyboard focus in the editor, `ui_owns_shortcuts` also blocks editor `W/E/R/Delete` etc., so typed `WASD` cannot move the player and `Esc` cancels the edit instead of stopping. Releasing outside a button cancels its click. A slider keeps capture while dragged outside its track. Focus loss, disabled buttons and scene changes cancel pending clicks. The host prevents pointer capture, mouse actions and camera look from consuming a cursor requested by the UI; keyboard and gamepad input remain available to gameplay.

Tab and Shift+Tab move keyboard focus in submission order; Enter or Space activates the focused button/checkbox, Left/Right adjust a focused slider, Up/Down move a focused list, and the mouse wheel scrolls the list under the pointer. A yellow outline marks the focused widget. While a widget is focused, Tab/Enter/Space/arrows are consumed by the UI and are blocked from gameplay movement (`KeyboardInputSource` filters those six keys). While a text or number field is editing, every keyboard key (including WASD) is blocked, so typed letters never move the player; the gamepad is unaffected. Losing window focus, disabling or omitting the focused widget, and scene resets clear focus without keeping stale state. The mouse wheel is also routed to the list (one notch ~ one row); a click inside a list consumes the click so it does not leak to widgets behind it, and items scrolled out of the list's clipped viewport are never hit-tested.

Anchors resolve once per frame from the current viewport size; the same resolved normalized rect drives both hit-testing and `snapshot` drawing, so there is no draw/hit drift. At 800×600, 1280×720 and 1920×1080 the anchored panel stays inside the window and its rows do not overlap. Long label/button text wraps on word boundaries inside its rect (hard `\n` lines are preserved). Wrapping happens in `GameUiOverlay` with real `ImFont` metrics: each visual line is shaped exactly once, words are never split mid-ligature, numbers keep LTR order, and right/center/left alignment is per line. Bidi is the simplified algorithm documented in `ArabicShaper.hpp` (Arabic/Urdu/Farsi scripts; no HarfBuzz fine positioning; Hebrew is not shaped) — see its header for scope.

The standalone player initializes the overlay only when a script produces widgets. Existing games without UI do not initialize an ImGui context. Rendering uses the existing RHI overlay backend shared with the MedievalVillage sample, with vertex/index buffers per frame slot. Resize recreates overlay framebuffers after waiting for the device.

A scene with widgets but no meshes succeeds the driver's geometry check (the widget count satisfies the check when the mesh count is zero); a scene with neither still fails as before.

Headless runs build and test widget state without opening a window or rendering text. Runtime exit logs report the remaining widget count for these checks.

## Resize, minimize, restore

Maximizing, resizing, minimizing and restoring must survive with Vulkan validation enabled. The player detaches the overlay and the renderer's swapchain-target framebuffers before destroying the old swapchain (Win32 permits only one live swapchain per window), recreates framebuffers on the new size, and parks in a 16 ms message-pump wait while the window is minimized (zero client size or `IsIconic`) instead of attempting a zero-extent creation. `clock.reset()` discards the minimized interval, so restoring does not feed a multi-second `dt` to the simulation, audio or particles. A swapchain that was destroyed by a minimize race is recreated on the first restored frame (the `!swapchain` branch). Frame fences are reset only after a successful `acquire`, so a failed acquire (out-of-date or timeout) never leaves a fence unsignaled at the next wait — verified by forcing maximize/restore/resize/minimize/restore cycles under `--validation` with `Validation errors: 0`.

## Packaging and limits

`nf build` includes the UI shaders, Amiri font and its license under `Shaders/Basic3D/UI`. Keep this directory with the exported player. The packaged player was verified by copying `dist/` outside the repository and running both headless and windowed (`Validation errors: 0`) — the Arabic font resolves from the package (`Segoe UI + Amiri (AR)`), which proves the font and `OFL.txt` shipped.

Each frame accepts up to 512 widgets. IDs allow 128 bytes through Lua, text allows 4096 bytes, and rectangles must stay inside the normalized viewport. The API uses fixed styling and explicit rectangles; it does not provide automatic text scrolling beyond the list's clipped viewport, images or a visual layout editor. Tooltips size from the live `ImFont` measurement (headless falls back to a documented monospace estimate), so the box matches the drawn text; the shared `TextLayout` helper (`nf::ui::wrap_text`, `layout_tooltip`) owns wrapping and is unit-tested without a GPU. List rows wrap but are not vertically clipped beyond their row box.

### Text editing limits (honest)

- Single line only; no drag or Shift selection, no undo/redo, no clipboard paste API.
- Cursor moves by codepoint (Home/End/arrows); shaping is `ArabicShaper` (simplified bidi). Full Unicode BiDi and HarfBuzz fine positioning are not provided; Hebrew is not shaped.
- IME: only committed `WM_CHAR` characters are inserted. Composition-window positioning, candidate selection and marked-text rendering are not provided.
- Surrogate pairs arrive as two `WM_CHAR` messages and are combined before insertion; a lone surrogate is dropped. Deletion never removes half a UTF-8 sequence.
- Losing window focus, disabling or omitting the field, Tab-ing away, clicking another widget, or a scene change cancels the draft without committing it.

### Number editing

- The committed `value` is never overwritten by an interim or invalid draft. An invalid commit (NaN, Inf, overflow, out-of-range, non-numeric) keeps the last valid value, stays in editing mode, and shows a red status.
- Parsing uses `std::from_chars`, so Windows locale (`,` vs `.`) never affects it.

### Settings persistence

- Settings live under `%LOCALAPPDATA%\\SANAD\\<ProjectName>\\settings.txt` (Windows), keyed by the sanitized project name so two games never share a file. They are not written beside the executable, so a packaged game keeps its settings even when its `dist/` folder is read-only.
- The file is versioned (`version: 1`), atomically replaced (temp + rename), capped at 1 MiB / 512 keys / 4096-byte values. A missing file loads as defaults; a corrupt, oversized or future-version file returns an error and leaves the caller's state untouched instead of crashing.
- Lua keys are `[A-Za-z0-9_.-]` (max 64, not `version`); Lua values are strings, numbers (inspected as decimal) or booleans (`"1"`/`"0"`). Paths cannot be passed from Lua.



## Scene changes and session data

The runtime exposes the same game flow in NFPlayer and the editor Game View:

```lua
local ok, error = nf.scene.load("content://Scenes/DemoLevel.nfscene")
local ok, error = nf.scene.restart() -- reload the current on-disk scene
local path = nf.scene.current()
local state, target, error = nf.scene.status()
-- state: idle, queued, ready, failed, quitting (unavailable without a host)
local ok, error = nf.scene.quit() -- exit player; Stop Play inside the editor

nf.session.set("score", 3)       -- boolean, finite number, UTF-8 string, or nil to remove
local score = nf.session.get("score", 0)
nf.session.clear()              -- clears values; does not cancel a scene command
```

Scene requests accept `content://Scenes/*.nfscene` including subdirectories, without traversal or backslashes. First accepted request wins until processed; later load/restart/quit requests return `false, error`. Acceptance means queued, not successfully loaded. All callbacks finish in the current world. At the next live `update` boundary the runtime waits for GPU work and loads synchronously. Missing or malformed destinations report `failed` and preserve the old scene; scripts can show the error and retry. A successful change clears script environments, UI capture, per-scene simulation bindings and old streaming jobs. Session values survive.

A loading cover is rendered on the requesting frame before the following update starts loading. Loading can still block the window; this is not asynchronous streaming and no percentage is shown. Pause and edit-mode `dt=0` defer pending commands. Loading time is not applied to the new scene as a large simulation delta.

Session data is RAM-only, never written by a transition or restart. Limits: 256 keys (1–64 ASCII letters/digits/underscore/hyphen/dot), 4096 bytes per string, 64 KiB total key/value payload. Numbers remain numbers and booleans remain booleans. Use `nf.settings` only for explicit persistence. New Game in UiStarter clears session values; retry preserves profile and time limit. Editor Play/Stop clear session values and pending commands. Stop after scene changes re-adopts the original in-memory authoring snapshot, including unsaved transforms, without writing the source scene.

`UiStarter` contains `Main.nfscene`, `DemoLevel.nfscene` and `Results.nfscene`. Create it using `nf new <directory> --name <name> --template UiStarter`, then build normally; all scenes and scripts are included in the package. Use mouse or Tab/Enter to activate the three beacon buttons; activated beacons change shape in the 3D view. The time limit (5–60 seconds) and player name are configurable in Settings. The sample is an engine capability demo, not a space mission simulator.
