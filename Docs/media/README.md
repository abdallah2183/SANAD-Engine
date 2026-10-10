# Media shot list — NOVAForge / محرك سَنَد

Every media file the README references lives here. **Record each one, save it
into this folder with the exact filename below, and the README picks it up
automatically.** A missing file renders as a broken image on GitHub, so only
save a file once you have actually recorded it.

## Recording spec (all files)

| | |
|:--|:--|
| **Width** | 960 px (source may be larger; downscale to 960) |
| **Frame rate** | 15–20 fps (motion reads fine, file stays small) |
| **Max size** | **under 8 MB** per file |
| **Format** | `.gif` for the showcase grid; `.png` for any still |
| **Content** | no desktop chrome, no mouse trail, no personal paths |

Recommended capture: a 1080p windowed editor, OBS or ScreenToGif, cropped to
the editor viewport, then downscaled to 960 px and optimized (`gifsicle -O3`).

## Files needed

| Filename | What to record | Notes |
|:---|:---|:---|
| `hero.gif` | The editor opening a project, then pressing Play | Shown at the very top of both READMEs. 8–12 s loop. |
| `editor_rtl.gif` | Arabic RTL UI: toggle to العربية, type a name with lam-alef (لا، لآ), watch shaping + bidi | Proves the Arabic-first claim. 6–10 s. |
| `rendering.gif` | Deferred PBR scene with cascaded shadows + sky-baked IBL; slowly orbit the camera | 6–10 s. |
| `vehicle.gif` | `NFSampleVehicleDemo`: drive, hit a destructible crate | 8–12 s. |
| `cliffstory.gif` | `NFSampleCliffStory`: the climb, dusk→night transition | 8–12 s. |

Static captures already present in `Docs/images/` (used as fallbacks in the
README): `shot_editor_en.png`, `shot_editor_ar.png`, `shot_vehicle.png`,
`shot_game.png`.

## Social preview image (repo card)

GitHub shows `Docs/images/novaforge_logo.jpg` as the default social image.
For a custom 1280×640 card, render the logo + one-line tagline on a dark
background and save it as a PNG/JPG, then set it in **Settings → General →
Social preview** (or commit an `og:image` the website already references).
`og:image` spec: **1280×640**, under 1 MB.
