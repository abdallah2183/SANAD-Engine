#version 450

// FXAA 3.11 (console variant) — the anti-aliasing pass this renderer was
// missing. Runs on the TONEMAPPED LDR image, not on HDR: FXAA reasons about
// perceptual luma, and the HDR buffer is linear, so an edge that reads as a
// hard step on screen can be a gentle ratio in linear space (and vice versa).
// Sampling after tonemap is what makes the luma test match what the eye sees.
//
// The algorithm is the published console variant: four diagonal taps for the
// luma range, an edge-orientation vector from the same four, a clamp of that
// vector to 8 texels, and two candidate blends (a 3-tap and a 5-tap) where the
// wider one is rejected when it overshoots the local luma range. No texture
// lookups beyond those, no branches on gradients — the early-out covers the
// flat majority of the frame.
//
// Push block must match FxaaPush in Renderer3D.cpp (4 floats, 16 bytes).

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D src_input;

layout(push_constant) uniform Push {
    vec2 texel;             // 1/width, 1/height of the SOURCE image
    float edge_threshold;   // relative luma range that counts as an edge
    float edge_threshold_min; // absolute floor, so dark areas do not shimmer
} pc;

float luma_of(vec3 c) {
    // Rec.601 weights on the tonemapped image: the same weights FXAA's own
    // reference implementation uses, kept here so a future change to the
    // colour space has one place to look.
    return dot(c, vec3(0.299, 0.587, 0.114));
}

vec3 fetch(vec2 uv) {
    return texture(src_input, uv).rgb;
}

void main() {
    vec2 uv = in_uv;

    vec3 rgbM  = fetch(uv);
    vec3 rgbNW = fetch(uv + vec2(-1.0, -1.0) * pc.texel);
    vec3 rgbNE = fetch(uv + vec2( 1.0, -1.0) * pc.texel);
    vec3 rgbSW = fetch(uv + vec2(-1.0,  1.0) * pc.texel);
    vec3 rgbSE = fetch(uv + vec2( 1.0,  1.0) * pc.texel);

    float lumaM  = luma_of(rgbM);
    float lumaNW = luma_of(rgbNW);
    float lumaNE = luma_of(rgbNE);
    float lumaSW = luma_of(rgbSW);
    float lumaSE = luma_of(rgbSE);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    float lumaRange = lumaMax - lumaMin;

    // Flat neighbourhood: pass the centre through untouched. This is both the
    // quality rule (blending a flat area only softens it) and the reason the
    // pass is cheap on the majority of pixels.
    if (lumaRange < max(pc.edge_threshold_min, lumaMax * pc.edge_threshold)) {
        out_color = vec4(rgbM, 1.0);
        return;
    }

    // Edge direction: the gradient of the diagonal luma pairs. Horizontal
    // edges give a mostly vertical vector, which is the direction FXAA then
    // walks along the edge rather than across it.
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    // Bias the direction away from zero so a nearly-flat gradient does not
    // produce an enormous reciprocal, and clamp the walk to 8 texels — beyond
    // that the taps are no longer describing the same edge.
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * 0.125), 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * pc.texel;

    // Two candidate blends along the edge: a tight 3-tap and a wider 5-tap.
    vec3 rgbA = 0.5 * (fetch(uv + dir * (1.0 / 3.0 - 0.5)) +
                       fetch(uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 rgbB = rgbA * 0.5 + 0.25 * (fetch(uv + dir * -0.5) +
                                     fetch(uv + dir * 0.5));

    // The wider blend is only safe while it stays inside the local luma range;
    // outside it, it has smeared a neighbouring feature into this pixel.
    float lumaB = luma_of(rgbB);
    out_color = vec4((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB, 1.0);
}
