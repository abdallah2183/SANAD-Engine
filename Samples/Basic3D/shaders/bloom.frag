#version 450

// bloom.frag — both stages of the bloom chain, selected by pc.mode.
//
//   0 PRE-FILTER  HDR target      -> bloom level 0 (half resolution)
//   1 DOWNSAMPLE  bloom level i-1 -> bloom level i   (half of the previous)
//
// One shader for both because the two stages have the same shape — sample one
// texture, write one colour — and differ only in the kernel. Two shader files
// would be two places to keep the texel-size convention, the uv convention and
// the output colour space in agreement; the mode flag makes that impossible to
// get wrong.
//
// The chain deliberately STOPS at the smallest level. There is no
// upsample-and-add pass: the tonemap shader sums all levels with bilinear
// taps, and a bilinear fetch of a half-size level IS the 2x2 tent filter an
// explicit upsample would apply. That trade costs one sampler read per level
// and saves four passes plus additive blending, which this RHI expresses on
// the render-pass attachment rather than the pipeline.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D src_input;

// Must match Renderer3D's BloomPush (8 floats, 32 bytes).
layout(push_constant) uniform PushConstants {
    // 1 / source size, in texels. The downsample's tap spacing is derived from
    // it rather than hardcoded so a level of any size filters identically.
    float src_texel_x;
    float src_texel_y;
    float threshold;   // PRE-FILTER only: luminance above which a pixel blooms
    float knee;        // PRE-FILTER only: width of the soft ramp, same units
    float radius;      // DOWNSAMPLE only: tap-spacing multiplier
    float mode;        // 0 = pre-filter, 1 = downsample
    float pad0;
    float pad1;
} pc;

// Soft-knee threshold. Below (threshold - knee) nothing passes; above
// (threshold + knee) everything above the threshold passes; in between the
// contribution is a quadratic ramp, so a surface crossing the threshold fades
// in instead of switching on. Matches rendering::bloom_prefilter exactly.
vec3 prefilter(vec3 c) {
    float lum = max(c.r, max(c.g, c.b));
    float soft = 0.0;
    if (pc.knee > 0.0) {
        soft = clamp(lum - pc.threshold + pc.knee, 0.0, 2.0 * pc.knee);
        soft = soft * soft / (4.0 * pc.knee + 1e-4);
    }
    float contrib = max(soft, lum - pc.threshold) / max(lum, 1e-4);
    return c * contrib;
}

// 13-tap box downsample. Weights sum to exactly 1, so a flat image survives
// every level unchanged and the sum the tonemap reads is the image, not the
// image scaled by the level count. Matches rendering::bloom_downsample.
vec3 downsample13(vec2 uv) {
    vec2 t = vec2(pc.src_texel_x, pc.src_texel_y) * pc.radius;
    vec3 a = texture(src_input, uv + t * vec2(-2.0,  2.0)).rgb;
    vec3 b = texture(src_input, uv + t * vec2( 0.0,  2.0)).rgb;
    vec3 c = texture(src_input, uv + t * vec2( 2.0,  2.0)).rgb;
    vec3 d = texture(src_input, uv + t * vec2(-2.0,  0.0)).rgb;
    vec3 e = texture(src_input, uv).rgb;
    vec3 f = texture(src_input, uv + t * vec2( 2.0,  0.0)).rgb;
    vec3 g = texture(src_input, uv + t * vec2(-2.0, -2.0)).rgb;
    vec3 h = texture(src_input, uv + t * vec2( 0.0, -2.0)).rgb;
    vec3 i = texture(src_input, uv + t * vec2( 2.0, -2.0)).rgb;
    vec3 j = texture(src_input, uv + t * vec2(-1.0,  1.0)).rgb;
    vec3 k = texture(src_input, uv + t * vec2( 1.0,  1.0)).rgb;
    vec3 l = texture(src_input, uv + t * vec2(-1.0, -1.0)).rgb;
    vec3 m = texture(src_input, uv + t * vec2( 1.0, -1.0)).rgb;
    vec3 o = e * 0.125;
    o += (a + c + g + i) * 0.03125;
    o += (b + d + f + h) * 0.0625;
    o += (j + k + l + m) * 0.125;
    return o;
}

void main() {
    vec3 result = (pc.mode < 0.5) ? prefilter(texture(src_input, in_uv).rgb)
                                  : downsample13(in_uv);
    // Alpha is 1 because the levels are sampled as colour, never blended; an
    // undefined alpha here would be a value nothing reads, which is exactly the
    // kind of "it works until it does not" a later change would trip over.
    out_color = vec4(result, 1.0);
}
