#version 450

// Fullscreen triangle for the decal pass (Phase 36).
//
// One triangle is drawn per decal, with the CPU-projected screen rect as the
// scissor. Same orientation contract as lighting.vert/ssao.vert: no Y flip,
// uv = ndc * 0.5 + 0.5 reads the texel this pixel covers. The actual decal
// footprint comes from the fragment shader's box test, not from geometry: a
// projected decal is a volume, and rasterizing its screen hull as a quad would
// miss the parts of the hull that do not lie on a single plane.

vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

layout(location = 0) out vec2 out_uv;

void main() {
    vec2 pos = positions[gl_VertexIndex];
    gl_Position = vec4(pos, 0.0, 1.0);
    out_uv = pos * 0.5 + 0.5;
}
