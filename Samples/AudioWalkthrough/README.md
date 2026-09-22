# Audio Walkthrough — G5 acceptance demo

Audio that ships: **cave echoes, wall muffle, menu music + ambience, and volume
sliders**, all driven from a settings file, with no engine source edits.

This folder is sample content, not engine code. Everything it does is public
`Engine/Audio` API (`NF/Audio/AudioScene.hpp` and friends).

## Run it

```bash
bash .workbuddy-ai/nfb.sh --target NFSampleAudioWalkthrough
build/DebugNinja/bin/NFSampleAudioWalkthrough.exe
```

Flags:

| Flag | Meaning |
|---|---|
| `--settings FILE` | Load `audio.volume.<bus>=<value>` lines. A missing file is written back with the shipped defaults (a template to edit). |
| `--out FILE.wav` | Where to write the render. Default `audio_walkthrough.wav` in the working directory. |
| `--play` | Also push the render to the real output device (WASAPI). Needs a sound card; silently reports if there is none. |

Exit code is `0` only when every check holds, so it doubles as a headless smoke
test on a machine with no audio hardware.

## What it renders (three beats of the walkthrough)

1. **Menu** — menu music fades in over an ambience bed, each on its own bus
   (`music`, `ambience`), while the listener stands in open air.
2. **Wall** — a 6 kHz "radio" and a 200 Hz "engine" behind a wall at `z = 8`.
   The hiss arrives dimmed; the rumble does not.
3. **Cave** — the listener steps into a reverb zone and one click returns as
   discrete echoes at `pre_delay + n * spacing`.

Play the WAV. The cave beat is the one to listen for: one click, then repeats
at 0.03 s, 0.14 s, 0.25 s, …

## Settings contract (for the Settings window — G3 / G9)

The object to bind is `nf::audio::AudioVolumeSettings` (documented in
`Engine/Audio/include/NF/Audio/Buses.hpp`). One slider per bus, five buses:
`master`, `music`, `sfx`, `ambience`, `voice`.

```cpp
// read
f32 v = scene.settings().volume(BusId::Music);
// write — clamps to [0, 1]; true only when the value actually changed
bool changed = scene.settings().set_volume(BusId::Music, slider_value);
// persist — appends one line per bus
scene.settings().append_settings_text(text);   // audio.volume.music=0.8
// load — feed every key=value pair from the settings file
scene.settings().apply_setting(key, value);    // false = not ours, pass it on
```

Shipped defaults: master `1.0`, music `0.8`, sfx `1.0`, ambience `0.7`,
voice `1.0`. Values are clamped to `[0, 1]` — a slider never amplifies, because
the buses feed a sum. A slider move takes effect on the **next block** with no
reconfiguration.

The demo prints the persisted form on every run, so the file format is visible
without opening a source file.

## Manual checklist (parts that need a real device)

The automated checks cover the numbers; these need ears. Run with `--play` on a
machine with working audio output.

- [ ] **Menu music** fades in smoothly over ~0.5 s — no click at the start, no
      jump at the end of the fade.
- [ ] **Ambience bed** is audible under the music and is a *separate* layer:
      it does not duck when the music fades.
- [ ] **Wall muffle** — in the WAV, the 6 kHz hiss is clearly duller behind the
      wall than the 200 Hz rumble; the rumble is barely changed.
- [ ] **Cave echoes** — the click repeats, each repeat quieter than the last,
      roughly 0.11 s apart. Leaving the cave (end of the render) does not leave
      a tail ringing.
- [ ] **Slider, live** — edit `audio.settings` (`audio.volume.music=0.2`), run
      with `--settings audio.settings`, and the music is quieter on the first
      block; the other buses are unchanged.
- [ ] **Master mute** — set `audio.volume.master=0` and the whole render is
      silent, with no truncation artifacts.
- [ ] **No dropouts** — `--play` on the real device is continuous from the menu
      fade through the cave echoes, with no gaps or crackle at the phase
      boundaries.

## What this demo does *not* cover

The walkthrough is reachable from C++ today. Driving it from a `.nfscene` (so a
game gets it with zero code) needs `Engine/Runtime` to mix through `AudioScene`
and to parse zone/music/bus keys — that is not G5's column, so it is filed as a
Request in `.workbuddy-ai/COORDINATION.md`. Until it lands, a developer using
`.nfscene` audio still writes the `AudioScene` wiring themselves.
