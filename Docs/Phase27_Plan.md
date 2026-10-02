# Phase 27 — Post-Processing Stack (design §206)

Design §206 specifies the post stack as *Exposure · Color Grading · Bloom · DOF ·
Motion Blur · Lens Effects · Tonemapping · Sharpening*, and requires that every
stage be individually switchable. Before this phase the engine had Exposure,
four tonemap operators, and a two-field "post FX" tail (saturation, vignette) —
and none of it was reachable from a scene. §263 lists "post process" as a
renderer success criterion, so the gap was not a nice-to-have.

## Scope

### W1 — The parameter block and its CPU mirrors
- `PostFxParams` grows from two floats to three nested stages — `BloomParams`,
  `ColorGradeParams`, `SharpenParams` — with `saturation`/`vignette` left first
  so the existing aggregate initialisation `PostFxParams{1.15f, 0.28f}` in
  `Samples/MedievalVillage` keeps meaning what it meant.
- Every stage is neutral by default and every neutral value is **exactly
  identity**, so a renderer that never touches the block renders the frame it
  rendered before the stack existed. That is a hard contract with the golden
  pixels, and `post_stack_off_equals_the_legacy_path` pins it.
- CPU mirrors for every shader function the stack adds: `bloom_prefilter`,
  `bloom_downsample`, `unsharp_hdr`, `apply_color_grade`, and `apply_post_chain`
  — the last one mirroring the *whole* fragment body, so the stage ORDER is
  testable and not only the individual stages.

### W2 — A real multi-pass bloom chain
Not a single-shader glow. `kBloomLevels` (4) levels, level 0 at half resolution:

```
HDR ──prefilter(soft knee)──▶ level0 ──13-tap downsample──▶ level1 ──▶ level2 ──▶ level3
                                                                    │
tonemap: hdr + intensity * (level0 + level1 + level2 + level3) / 4 ◀─┘
```

- The chain **stops at the smallest level**. The tonemap shader sums the levels
  with bilinear taps, and a bilinear fetch of a half-size level *is* the 2×2 tent
  filter an explicit upsample pass would apply. That trade costs one sampler read
  per level and saves four passes plus additive blending — and this RHI expresses
  blend state on the render-pass attachment, not the pipeline.
- One shader pair (`bloom.vert` / `bloom.frag`) serves both the prefilter and the
  downsample; the kernel rides a push-constant mode flag. Two shader files would
  be two places to keep the texel convention, the uv convention and the output
  colour space in agreement.
- `enabled` is the only field that costs anything: with it false the renderer
  records **no bloom pass at all**, and the tonemap shader's
  `bloom_intensity > 0` guard means the bloom bindings are never even read. With
  bloom off the HDR view is bound in their place, so every binding is still a
  valid image — a set with an unwritten binding is not a set.

### W3 — Colour grading and sharpening
- Grading runs in **HDR, before the tonemap operator**, which is where §206 puts
  it and is the reason it matters: a contrast curve applied after the tonemap
  clips the highlights the operator had just rolled off.
- Sharpening is an unsharp mask on the linear HDR image, applied **before** the
  bloom add so the glow is not itself sharpened into a hard ring. It samples the
  HDR target directly, so it costs four texture fetches inside the existing pass
  and no passes of its own.
- The implemented order is
  `sharpen → bloom add → exposure → grade → tonemap → gamma → saturation → vignette`.
  §206 lists exposure before grading and bloom after it; the bloom add sits
  before exposure here because the prefilter reads the raw HDR target, so its
  threshold is in pre-exposure units — adding the glow after exposure would make
  its strength track the exposure control while its threshold did not.

### W4 — Scene, runtime and editor wiring
- `PostProcessComponent` in `RuntimeSceneTypes.hpp` (flat floats, following
  `SkyComponent`: this header is included by the editor and the tools, and
  pulling `Renderer3D.hpp` — and through it the RHI — into all of them to carry
  fifteen numbers is the wrong trade).
- A `PostProcess:` line in `.nfscene`: parser with per-key range validation,
  writer, and prefab/play-mode clone. Written **only** when the component exists,
  so a scene that has never heard of it round-trips byte-identical.
- `Runtime::extract_post_process()` pushes the block into the renderer, called
  from both render paths beside `extract_sky()`.
- Editor: `SetPostProcessCommand`, `make_post_process_command` (validated
  factory, same ranges as the loader), `EditorApp::set_post_process`, an
  Inspector panel, and an entry in the Add-component menu. Undo of the edit that
  first created the block **removes** the component, so the scene round-trips
  with no `PostProcess:` line.

### W5 — Verify
- `Tests/RHITests/test_post_stack.cpp` — 30 tests: the neutral contract, the
  prefilter's knee (continuity, monotonicity, colour-ratio preservation,
  max-channel vs luma), the 13-tap weights derived from the tap offsets, the
  unsharp mask, the grade's four controls, the two ORDER tests, and three GPU
  tests (no bloom passes when off; `kBloomLevels` passes when on; the glow
  changes the pixels and *brightens* rather than darkens them).
- `Tests/RuntimeTests/test_runtime_post_process.cpp` — the line round-trips, a
  one-key line keeps the other defaults, a zero radius is refused with a warning
  naming the key, an absent component writes no line, and the Runtime really
  pushes the block into the renderer (the integration half — a component the
  runtime reads and drops is the "registered but never driven" failure).
- `Tests/EditorTests/test_inspector.cpp` — apply / undo / redo / read-back, and
  refusal of out-of-range and non-finite values with the same bounds the loader
  uses.

## Explicitly out of scope

- **Depth of field, motion blur and lens effects.** §206 lists them; each needs
  data the renderer does not currently keep (a depth-of-field circle of
  confusion wants a physical lens model and the depth target read back as a
  texture; motion blur wants a velocity buffer the deferred path does not write).
  Shipping them as sliders wired to nothing would be worse than not shipping
  them.
- **A per-stage LUT for colour grading.** The four controls cover the common
  case; a LUT wants an asset type, a loader and a UI, which is its own phase.
- **`exposure` and the tonemap mode as scene data.** They are renderer setters
  with golden pixels pinned to their defaults, and moving them into the
  component is a separate compatibility decision.

## Verification notes

### Acceptance (run this literally)

```bash
bash .workbuddy-ai/nfb.sh                       # full build, exit 0
./build/DebugNinja/bin/RHITests.exe post_stack  # 30/30
./build/DebugNinja/bin/RuntimeTests.exe post_process   # 5/5
./build/DebugNinja/bin/EditorTests.exe post_process    # 1/1
python Scripts/audit_imgui_ids.py               # no NEW collisions
./build/DebugNinja/bin/NOVAForgeEditor.exe --scene content://Scenes/Example.nfscene \
    --frames 125 --validation                   # exit 0, 0 validation errors, automation=OK
```

Measured 2026-10-02 (all from that session's own runs, not recalled):

| check | result |
|---|---|
| full build | exit 0 |
| full sweep, 26 suites | **1769 passed / 0 failed / 2 skipped** (was 1733/0/2 before the phase; +36 = 30 + 5 + 1) |
| `RHITests` | 211 → **241** (`test_post_stack.cpp` = 30) |
| `RuntimeTests` | 101 → **106** (`test_runtime_post_process.cpp` = 5) |
| `EditorTests` | 251 → **252** (`test_inspector.cpp` + 1) |
| `RHITests post_stack` | 30 / 0 |
| `RuntimeTests post_process` | 5 / 0, 0 `ERROR [RHI]` lines |
| editor headless 125f | exit 0, 0 validation errors, 0 automation failures, 0 RHI leaks |
| editor windowed 125f | exit 0, 0 validation errors, `cube lit pixels = 229149` (unchanged) |

### The visual proof, and two engine findings it turned up

`C:/Users/abdal/OneDrive/Desktop/PostFxDemo` is a real project (`nf new` → scene →
`nf cook` → `nf verify` → `nf build` → `nf run`), not a hand-written sample. Its
`Content/Scenes/Main.nfscene` carries one `PostProcess:` line:

```
PostProcess: bloom=true bloom_threshold=0.55 bloom_knee=0.35 bloom_intensity=1.6
  bloom_radius=1.5 grade=true grade_contrast=1.1 grade_pivot=1 grade_temperature=0.06
  grade_tint=0 grade_gamma=1 sharpen=true sharpen_amount=0.4 sharpen_radius=1
  saturation=1.15 vignette=0.3
```

`nf run --headless --frames 120 --validation` → exit 0, `scene loaded … with 6 entities`,
`Rendered 120 frames`, `Validation errors: 0`, `Alive RHI objects … 0`. The two screenshots
in `shots/phase27_postfx_{on,off}.png` are the same scene with and without that line:
**off** the emissive cube is a flat cream quad; **on** it is a warm blob with a halo
bleeding onto the ground behind it, and the vignette darkens the corners. Frame cost at
1280×720 in a debug build: 9.05 ms → 12.76 ms for the whole stack (the bloom chain is
4 fullscreen passes at ≤ half resolution).

Building that demo is what surfaced the two findings filed in `COORDINATION.md`, and both
are worth carrying forward:

- **`.nfmat` `emission:` is inert.** `lighting.frag:100` computes
  `emissive = albedo * emission_strength` — the colour is parsed, saved and round-tripped
  and then never read (`gbuffer.frag` writes it into `misc.rgb` and nothing samples it).
  A material with a dark `base_color` and a bright `emission:` renders BLACK. The demo
  works around it by putting the glow colour in `base_color`, which also tints the diffuse
  response — not an equivalent substitute, and the reason the workaround is commented in
  the `.nfmat`.
- **`scenes_equal_structure` compares the wrong pair.** `Runtime.cpp:385-410` synthesizes a
  name from the entity's KIND for every entity without a `Name:` line, so a scene with two
  meshes has two entities named `"Mesh"`; the editor's structure check indexes by name and
  keeps only the last, then reports a transform mismatch on a scene that saved and reloaded
  perfectly. It made the editor exit 1 on the first version of this demo. Adding a unique
  `Name:` per entity is the workaround; the check (or the synthesis) is what should change.

### Rule 0 — each claim reverted, watched to fail, restored

Run 2026-10-02. Three breakages at once, then one more on its own:

| breakage | expected | measured |
|---|---|---|
| `bool bloom_on = false;` in `Renderer3D::render` | the two GPU bloom tests fail | `post_stack_renderer_records_the_whole_chain_when_enabled` FAILED (`bloom_levels_recorded != kBloomLevels`), `post_stack_bloom_changes_the_rendered_image` FAILED (`different > size/4`). 30/0 → **28/2** |
| `extract_post_process()` removed from `render_offscreen` | the runtime push test fails | `runtime_pushes_post_process_into_the_renderer` FAILED (`seen.bloom.enabled`). 5/0 → **4/1** |
| grade moved after the operator in `apply_post_chain` | the ordering test fails | **it did not** — see below |

The third row is the useful one. The original ordering test compared the chain
against "grade the finished LDR image", and the break produces "grade right
after the operator, before gamma" — a different wrong answer that the test was
blind to. It now computes *both* wrong orders and requires the result to differ
from each, and the re-run of that break gives
`post_stack_grade_runs_before_the_tonemap FAILED: std::abs(got.x - wrong_a.x) > 1e-3f`
(29/1). **A test that names an ordering is not the same as a test that pins it:
enumerate the orderings you are ruling out, not just the one you had in mind.**

### Two things worth remembering

- **`##suffix` must be unique, not merely present.** The first draft of the
  Inspector panel gave all fourteen widgets `"##pp"`. ImGui's ID is the text
  after `##`, so every slider shared one ID. `Scripts/audit_imgui_ids.py` does
  not catch it (it flags *bare* labels), and ImGui's own duplicate detector is
  hover-based so no automated run would have seen it either. A `##suffix` makes
  an ID unique only if the suffix is.
- **The bloom levels are declared as reads on the tonemap pass.** Without that
  the graph leaves each level in the layout the bloom pass wrote it in, and the
  tonemap pass samples a colour attachment — a validation error on one driver and
  a garbage glow on another.
