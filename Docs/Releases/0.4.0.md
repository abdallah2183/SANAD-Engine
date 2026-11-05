# SANAD Engine 0.4.0 — the player can aim

**Not yet packaged.** This release notes file describes the working tree. The
portable zip and the sample-game zip are produced by
`Scripts\build.ps1` / `nf ship` on a machine that can run the freshly built
binaries; the checksums below are filled in by that step, not by hand.

Windows 10/11 x64 · Vulkan-capable GPU driver required · Arabic-first UI (English available)

---

## Highlights | أبرز ما في النسخة

### The camera is the player's | الكاميرا بيد اللاعب

The 0.3.0 notes ended with a known gap, in their own words: *"The camera cannot
be aimed yet. It follows a fixed orbit; a mouse look and a player-driven yaw
come next."* This release closes it.

- **Mouse look.** Moving the pointer turns the orbit. The pointer is hidden and
  clipped to the window while a camera is being aimed, held at the centre and
  re-centred every frame, so a small desk is enough to turn through a full
  circle. `Esc` gives the pointer back and a click inside the window takes it
  again — a captured cursor with no way out reads as a frozen machine, not as
  a game.
- **Gamepad.** The platform layer has read XInput since 0.1.0, and nothing in
  gameplay ever saw it. Now the left stick moves, the right stick aims, `A`
  jumps, `LB`/`LT` sprint and `X` interacts. Sticks get a **radial** deadzone
  with a rescale, so a resting stick neither creeps nor jumps.
- **Camera-relative movement.** On a keyboard, `W` is a direction on the
  screen, not a direction in the world. Once the player can turn, `W` has to
  mean "away from the camera" — so the movement vector is rotated by the
  camera's yaw before the character sees it. `nf.camera_move()` returns that
  vector; a scene with no camera gets the raw axes, so old scripts keep
  working unchanged.
- **Wheel zoom** with a per-scene range, and **auto-align**: while the player
  runs without touching the mouse, the camera drifts back behind the character
  at a scene-authored rate. `align=0` (the default) leaves it exactly where the
  player left it.
- **A vertical clamp** that is part of the orbit rather than a limitation:
  past the pole the camera rolls over and shows the world upside down.

### The control scheme is data | نظام التحكّم صار بيانات

The engine answered "which key means jump" with a ladder of `if (action ==
"jump") return key_down(VK_SPACE)`. That is fine for a demo and wrong for an
engine: a game that wanted jump on another key had to replace the whole input
source.

- A project can now ship `Content/Input/bindings.nfinput`:
  ```
  jump         = key:Space, pad:A
  move_x       = -key:A, +key:D, axis:LeftX
  ```
  `key:`, `pad:` and `axis:` with an optional `+`/`-` sign, `#` comments
  anywhere, names case-insensitive.
- The file **overlays** the defaults rather than replacing them: rebinding one
  key cannot silently delete the rest of the control scheme. A malformed file
  is reported with its line number and ignored, because half a control scheme
  is worse than the one that worked.
- The old ladder IS the default table, including the negated stick Y, so every
  existing game behaves exactly as it did.
- The evaluator is a pure function of `(table, action, device)`, and the device
  is a table of function pointers — which is why seven of the new cases can
  press keys on a machine with no keyboard and read a pad with no pad.

### Where the numbers live | الأرقام في المشهد

Every one of these is scene data, not a constant in the runtime, and every key
is optional — a scene written before this existed loads with the defaults,
which are the aiming every third-person game has:

```
Camera: … follow=Player pivot=0,1.45,0 distance=5.2 yaw=180 pitch=-13
        smoothing=9 radius=0.35 avoid=1
        mouse=1 looksens=0.12 sticksens=180 pitchmin=-80 pitchmax=60
        invert_y=0 zoommin=1.5 zoommax=12 zoomstep=0.6 align=90
```

The editor's Camera panel grew an **Aiming** section for all of them, with the
same validation the Inspector applies to every other camera field: a value the
runtime would have had to silently re-clamp is rejected instead, because a
number that differs from the one on screen is worse than an edit that fails.

### The maths is its own module | الرياضيات وحدة مستقلة

`NF/Runtime/PlayerLook.hpp` holds the four decisions aiming makes — what a
mouse pixel is worth, what a held stick is worth, how far the orbit may pitch,
and which way forward is — free of the ECS, the window and the frame loop, the
same way `CameraFollow.hpp` holds the follow camera. Eleven cases pin them:
the short way round, frame-rate independence, the deadzone rescale, the clamp
that repairs an inverted range, and the rotation that keeps a diagonal as fast
as a straight line.

## What changed, by area | ما تغيّر

| Area | Change |
|:---|:---|
| `Engine/Gameplay` | `IInputSource` grew `begin_frame`, `look_delta`, `wheel_delta` and `set_pointer_capture`, all defaulting to "no pointer" — every existing source (replay logs, headless, the editor) compiles and behaves as before |
| `Engine/Platform` | unchanged; the gamepad was already there, the runtime now reads it |
| `Engine/Runtime` | `PlayerLook` (new), `InputBindings` (new), `KeyboardInput` (pointer + pad + capture + a table instead of a ladder), `Runtime::update_follow_camera` applies the aim, `Runtime::camera_yaw`, capture handed back when the input source is replaced |
| `Engine/Scripting` | `nf.camera_yaw()`, `nf.camera_move()` |
| `Engine/UI` | 13 new Arabic/English keys for the Aiming section |
| `Editor` | Aiming panel section, `CameraEdit` fields, validation, undo through the existing `SetCameraCommand`; play mode aims with a right-drag, using the gesture the viewport already teaches, because the editor must not steal the pointer |
| `Games/TreeWorld` | camera `align=90`; the explorer walks where the camera looks |
| `Templates/ThirdPerson` | the Crate Hunter starter is finally third-person: its camera follows the player and can be aimed |
| `Tests` | `RuntimeTests/test_player_look.cpp` — 11 maths cases, 2 scene-format cases, 5 runtime cases; `RuntimeTests/test_input_bindings.cpp` — 7 binding cases |

## Verification | التحقق

**What was run on this machine:**

- **Build:** `ninja -C build/Release` after the change — **0 errors, 0 warnings**
  under the repository's `/W4 /WX`. All 45 executables link, including
  `SANADEditor.exe` and `NFPlayer.exe`.
- **The maths:** all 11 `PlayerLook` cases pass and all 7 `InputBindings` cases
  pass (`RuntimeTests.exe`, `[       OK ]` for each) — 18 new cases, none of
  which needs a device or a scratch directory.

**What could NOT be run here, and why:** this machine's Device Guard policy
refuses to start the freshly linked unsigned binaries
(`was blocked by your organization's Device Guard policy`), and the Vulkan
device is unavailable to a process launched this way, so every case that needs
a device skips with `no Vulkan device available`. That is an environment
policy, not a defect — `AGENTS.md` §6 documents the same class of block. The
device-dependent cases in `test_player_look.cpp` are written and compile; they
were not observed passing.

**To verify on a machine without that policy:**

```bat
build_nf.bat Release
build\Release\bin\RuntimeTests.exe
build\Release\bin\nf.exe run --project Games\TreeWorld\TreeWorld.nfproj --frames 300
```

Then, in the running game: the mouse turns the camera, the wheel changes the
distance, `Esc` gives the pointer back, and `W` walks wherever the camera is
looking.

## Known issues | قيود معروفة

- **Windows x64 only.** There is no Linux or macOS player.
- **The binaries are unsigned**, so a machine with Smart App Control or WDAC
  may refuse to launch them. Sign the build
  (`Scripts\sign_build.ps1`) rather than disabling your security.
- **No anti-aliasing yet.** The renderer still has no MSAA, FXAA or TAA; that
  is the next rendering item, not a regression.
- **No per-object motion vectors**, which is the prerequisite for a temporal
  pass; the motion blur that exists is camera-only.
- **The editor's play mode aims with a right-drag**, not a captured pointer:
  the editor needs the pointer for its own panels, so it uses the gesture the
  viewport already teaches. The shipped player captures it properly.
- **Input is rebindable from a file, not from a screen.** There is no rebinding
  UI yet and no per-player profile; a game ships one `bindings.nfinput`.
- Autosave is a recovery file, not a versioned backup system.
