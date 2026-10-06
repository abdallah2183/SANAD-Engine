# CliffStory — حكاية الجرف / The Cliff's Tale

A 2D story-climb built on the engine's `NFScene2D` layer, and the first game in
the tree to render through it.

```
build\DebugNinja\bin\NFSampleCliffStory.exe
```

A lantern-bearer climbs 130 units of stone to relight the shrine at the top. The
story is told by the background as much as by the text: the sky runs from a dusk
at the valley floor to night at the summit, and stars fade in as you rise.

| Control | |
|---|---|
| `←` `→` or `A` `D` | move |
| `Space` / `↑` / `W` | jump (hold for height) |
| `R` | restart |
| `Esc` | quit |

`--frames N`, `--screenshot P`, `--skip-title`, `--warp P` and `--validation`
are also accepted; `--help` lists them.

---

## What this sample is actually for

`NFScene2D` already owns a complete 2D half-frame — `Camera2D` transforms world
to screen pixels, `SpriteBatcher` sorts and bakes, `Lighting2D` folds lights into
sprite tints, `PhysicsWorld2D` steps the player — and all of it is verified by
`Scene2DTests`. What it deliberately does *not* have is a GPU path:
`SpriteBatcher::vertices()` is a CPU array, so the same bake stays checkable in a
headless test, and `Scripts/check_layering.sh` keeps Scene2D free of any RHI
dependency.

`Render2D.cpp` is the missing last hundred lines, and it is the only file in the
project that knows about the RHI:

- **One pipeline, one pass.** No depth attachment: the batcher already emits
  sprites back-to-front, so the painter's algorithm *is* the layering. That buys
  correct alpha for overlapping ivy, lantern glow and haze for free.
- **One descriptor set per texture page.** Seven pages, one pipeline — the
  `Material`/`MaterialInstance` split doing what it was designed for. The frame
  below is ~260 sprites in **4 draw calls**.
- **A runtime glyph atlas.** Arabic story text rides the same batcher as the rock.
  A stock TrueType rasteriser draws logical Arabic letter by letter, so
  `NF/UI`'s `ArabicShaper` converts to presentation forms and visual order first.

The level is deterministic — the cliff face is a pure function of height, and
everything scattered on it comes from an integer hash rather than an RNG — so
`--warp` and `--frames` land in exactly the same place twice.

## Art

Nothing here is a placeholder. Every texture is the medieval village kit already
in the tree:

| | |
|---|---|
| `T_RockTrim_BaseColor.png` | seamless stone masonry — the whole cliff |
| `T_Brick_BaseColor.png` | the shrine's columns and lintel |
| `T_Plaster_BaseColor.png` | the shrine's back wall |
| `T_RoundTiles_BaseColor.png` | the shrine's steps and the lantern cages |
| `T_VineLeaf.png` | ivy, auto-cropped into individual leaves at load |

The font is the engine's own `Resources/fonts/Amiri-Regular.ttf`.

Ivy deserves a note: the vine sheet is 512px of scattered leaves on transparent
ground, *not* a clean grid, so slicing it into fixed cells would hand back a third
of a leaf in half the cells. `build_leaf_sheet` takes each grid cell's opaque
bounding box instead, on the CPU at load, so every entry is a whole leaf whatever
shape the artist drew — and the aspect ratio is clamped, because one wide crop
otherwise becomes a leaf wider than the ledge it is rooted on.

## Two engine behaviours worth knowing

Both cost real time to find, and both are the kind of thing that looks like
something else entirely:

**Vulkan's clip space has Y down, not up.** `Screen pixels -> NDC` is
`y * 2/height - 1`, not the OpenGL `1 - y * 2/height`. The wrong sign mirrors the
whole frame. A cliff is nearly vertically symmetric, so it *still reads as a
cliff* — while every HUD element lands upside down and glyphs render inverted,
which sends you hunting a font bug that does not exist.

**`Camera2D::screen_to_world` does not round-trip in Y.** A marker drawn at pixel
row 30 comes out at row ~690. `Game::px_to_world` writes the inverse out by hand
from the camera's documented forward transform instead, so HUD quads convert with
the same function and stay pinned to the viewport whatever the camera does.

## Layout

| | |
|---|---|
| `Render2D.{hpp,cpp}` | the GPU half: pipeline, texture pages, glyph atlas |
| `CliffWorld.{hpp,cpp}` | the level: face profile, ledges, contents, physics |
| `main.cpp` | the game: camera, lighting, story, HUD, frame loop |
| `shaders/sprite2d.{vert,frag}` | one shader pair for all of it |

## Not expressible here

The 2D layer has no `.nfscene` line types yet — no `Sprite:`, `Camera2D:` or
`Body2D:` — so this sample talks to `NFScene2D` and the RHI directly and treats
the scene file as the 3D path it is. Wiring 2D into the scene format and the
editor is the obvious follow-up; until then a 2D game is C++.