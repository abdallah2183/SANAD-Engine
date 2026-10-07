#version 450

// SSAO bilateral blur (half resolution): 5x5 gather with depth+normal aware
// weights.
//
// The raw SSAO is 16 taps of hash-rotated spiral — smooth in the aggregate
// but grainy per pixel. A plain box blur would smear dark crevices across
// bright edges (halos around every contact), so each tap is weighted by how
// much it belongs to THIS pixel: similar depth (relative, so the perspective
// compression does not need a linearization constant) and a similar normal.
// No push constants: texel size comes from textureSize(), the sigmas are
// tuned constants below — this stage has nothing per-frame to say.
//
// Sky pixels carry depth ~1 and an encoded (0,0,1)... they blur like anything
// else, which is correct: the blur must not invent an edge at the skyline,
// and the raw pass already wrote 1.0 there.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D ssaoTex;
layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(set = 0, binding = 2) uniform sampler2D normalTex;

// Relative depth sigma: a tap 10% nearer/farther than the center counts ~e^-1.
// Relative, so near and far geometry blur with the same world-space sense.
const float kDepthSigma = 0.10;
// Normal weight exponent: dot 0.99 -> ~0.9, dot 0.9 -> ~0.35, dot 0 -> 0.
// Keeps the blur inside one face without segmenting smooth curves.
const float kNormalPower = 8.0;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(ssaoTex, 0));
    float center_ao = texture(ssaoTex, in_uv).r;
    float center_d = texture(depthTex, in_uv).r;
    vec3 center_n = normalize(texture(normalTex, in_uv).rgb * 2.0 - 1.0);

    float sum = center_ao;
    float wsum = 1.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            if (x == 0 && y == 0) {
                continue;
            }
            vec2 ouv = in_uv + vec2(float(x), float(y)) * texel;
            float ao = texture(ssaoTex, ouv).r;
            float d = texture(depthTex, ouv).r;
            vec3 n = normalize(texture(normalTex, ouv).rgb * 2.0 - 1.0);
            // Depth weight collapses when EITHER end is sky: a skyline tap
            // must not drag the sky's 1.0 into a crevice (or vice versa), and
            // raw depth ~1 makes the relative comparison meaningless there.
            float dw = 1.0;
            if (center_d < 0.999999 && d < 0.999999) {
                float rel = (d - center_d) / max(center_d * kDepthSigma, 1e-5);
                dw = exp(-rel * rel);
            } else if ((center_d < 0.999999) != (d < 0.999999)) {
                dw = 0.0;
            }
            float nw = pow(clamp(dot(n, center_n), 0.0, 1.0), kNormalPower);
            float w = dw * nw;
            sum += ao * w;
            wsum += w;
        }
    }
    float ao = sum / max(wsum, 1e-4);
    out_color = vec4(ao, ao, ao, 1.0);
}
