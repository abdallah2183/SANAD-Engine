# Phase 13 — Directional Shadows + Sky

Visual payoff milestone: a lit cube on a floor with a real shadow under it,
under a procedural sky — instead of gray geometry on near-black.

## Scope

### W1 — Directional shadow map
- Fixed 2048² `D32_SFloat` shadow target (`DepthAtt | Sampled`), standalone
  texture (not resolution-dependent; survives resize like the white fallback).
- Shadow pass reuses the depth-only render pass + depth pipeline with a light
  view-projection (ortho box ±12, light at `-dir * 20` looking at origin).
  Recorded as a rendergraph pass over an imported texture so barriers stay
  automatic.
- `FrameUniforms` gains `light_view_proj[16]` + `shadow_params[4]`
  (x enabled, y strength 0..1, z bias, w map size). std140: +80 bytes.
  Header struct, static_assert, and `lighting.frag` block move together.
- Lighting set layout 5 → 6 bindings (binding 5 = shadow map). Writes updated.
- `lighting.frag`: world pos → light NDC, 3×3 PCF, bias from uniforms.
  Only the directional diffuse+specular term is scaled; ambient/emission stay.
- Defaults: enabled, strength 1.0, bias tuned so the cube neither acne nor
  peter-pans at test scale (document the value).

### W2 — Procedural sky + sun
- Background branch (`depth >= 1`) becomes a zenith/horizon gradient driven
  by the view ray + a sun disk along the directional light. No textures.
- Keeps the "background distinguishable from geometry" test contract — tests
  assert sky-blue-ish, not near-black.

### W3 — Runtime + editor wiring
- `runtime::DirectionalLight` gains `shadow_enabled` (default true);
  Runtime extraction forwards it to `Renderer3D` (new setter).
- Editor light section: shadow checkbox through the existing validated
  command path (same pattern as the camera Active flag).

### W4 — Verify
- Pixel tests: shadowed floor pixels darker than lit ones beside them;
  shadow off == old brightness; sky pixels blue-ish and non-black.
- Full suite green, validation 0, headless acceptance, samples still render.

## Explicitly out of scope
Point/spot shadows, cascades, contact hardening, volumetrics, clouds,
time-of-day animation, LOD cross-fading (still out from Phase 12).

## Verification notes
(filled as the work lands)
