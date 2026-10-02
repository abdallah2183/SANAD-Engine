#version 450

// tonemap.frag — the final pass, and the whole §206 post stack after the bloom
// chain's own passes.
//
// Stage order, which is what rendering::apply_post_chain mirrors:
//
//   sharpen (unsharp mask on linear HDR)
//     -> bloom add (sum of the levels, scaled by intensity)
//     -> exposure
//     -> colour grade (contrast / temperature / tint / gamma, in HDR)
//     -> tonemap operator
//     -> gamma
//     -> saturation, vignette
//
// Every stage is guarded by its own enable value, and every neutral value is
// EXACTLY identity: with the whole stack off this shader computes
// `texture(hdr_input, in_uv).rgb * pc.exposure` and nothing else, which is the
// expression it computed before the stack existed. The golden pixels depend on
// that, so do not "tidy" a guard away — a stage that always runs costs an image
// that differs for every scene that never asked for it.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D hdr_input;
// Bloom levels 0..3, largest (half resolution) first. When bloom is off the
// renderer binds the HDR texture here instead and sets bloom_intensity to 0 —
// the guard below means these are then never read, so an "unused" binding can
// never leak a stale image into the frame.
layout(set = 0, binding = 1) uniform sampler2D bloom0;
layout(set = 0, binding = 2) uniform sampler2D bloom1;
layout(set = 0, binding = 3) uniform sampler2D bloom2;
layout(set = 0, binding = 4) uniform sampler2D bloom3;

// The mode is carried as a float rather than an int because the renderer
// uploads the block as one float array. Values are compared by range, not
// equality, so a numeric fuzz from the float path cannot land between two
// modes.
//
//   0 Exponential — 1 - exp(-x). The shipped default and the *only* mode the
//     golden pixels were authored against; keep it at 0 so an unset block
//     reproduces the old frame bit for bit.
//   1 ACES       — Narkowicz's fitted filmic curve.
//   2 Reinhard   — x / (1 + x), the classic photographic operator.
//   3 Linear     — exposure only, for diagnostics and HDR capture.
//
// Must match Renderer3D's tonemap push block (16 floats, 64 bytes).
layout(push_constant) uniform PushConstants {
    float exposure;
    float vignette;         // 0 = off
    float saturation;       // 1 = neutral
    float tonemap_mode;
    float bloom_intensity;  // 0 = the bloom levels are not read at all
    float sharpen_amount;   // 0 = the unsharp mask is not evaluated
    float grade_enabled;    // > 0.5 = grade
    float grade_contrast;
    float grade_pivot;
    float grade_temperature;
    float grade_tint;
    float grade_gamma;
    float texel_x;          // 1 / output width, for the sharpen taps
    float texel_y;          // 1 / output height
    float sharpen_radius;   // tap-spacing multiplier
    float pad0;
} pc;

// Unsharp mask on the linear HDR image. Matches rendering::unsharp_hdr.
vec3 sharpen_hdr(vec3 centre, vec2 uv) {
    vec2 t = vec2(pc.texel_x, pc.texel_y) * pc.sharpen_radius;
    vec3 blurred = (texture(hdr_input, uv + vec2(t.x, 0.0)).rgb +
                    texture(hdr_input, uv - vec2(t.x, 0.0)).rgb +
                    texture(hdr_input, uv + vec2(0.0, t.y)).rgb +
                    texture(hdr_input, uv - vec2(0.0, t.y)).rgb) * 0.25;
    return centre + (centre - blurred) * pc.sharpen_amount;
}

// Sum of the bloom levels. A bilinear fetch of a level half the size is the
// 2x2 tent filter an explicit upsample pass would apply, so summing the levels
// with their own samplers IS the upsample — which is why the chain has no
// upsample pass. The levels are averaged (1/N) so adding a level to the chain
// does not brighten the glow; `intensity` is the single control that does.
vec3 bloom_sum(vec2 uv) {
    vec3 s = texture(bloom0, uv).rgb;
    s += texture(bloom1, uv).rgb;
    s += texture(bloom2, uv).rgb;
    s += texture(bloom3, uv).rgb;
    return s * 0.25;
}

// Colour grade, applied in HDR before the tonemap operator (§206 puts grading
// before tonemapping, and the reason it matters: a contrast curve applied to an
// already-tonemapped image clips the highlights the operator had just rolled
// off). Matches rendering::apply_color_grade exactly, including the 0.25 gain.
vec3 color_grade(vec3 c) {
    const float kGain = 0.25;
    if (pc.grade_temperature != 0.0) {
        c.r *= 1.0 + kGain * pc.grade_temperature;
        c.b *= 1.0 - kGain * pc.grade_temperature;
    }
    if (pc.grade_tint != 0.0) {
        c.g *= 1.0 - kGain * pc.grade_tint;
    }
    if (pc.grade_contrast != 1.0) {
        c = (c - vec3(pc.grade_pivot)) * pc.grade_contrast + vec3(pc.grade_pivot);
    }
    if (pc.grade_gamma != 1.0 && pc.grade_gamma > 0.0) {
        // max(0, ...) because pow() of a negative base is undefined and a
        // contrast pivot below the black point can push a channel negative.
        c = pow(max(c, vec3(0.0)), vec3(1.0 / pc.grade_gamma));
    }
    return c;
}

void main() {
    vec3 hdr = texture(hdr_input, in_uv).rgb;

    if (pc.sharpen_amount > 0.0) {
        hdr = sharpen_hdr(hdr, in_uv);
    }
    if (pc.bloom_intensity > 0.0) {
        hdr += bloom_sum(in_uv) * pc.bloom_intensity;
    }

    // Exposure. With both guards above false this is the original first line,
    // `texture(...).rgb * pc.exposure`, exactly.
    hdr *= pc.exposure;

    if (pc.grade_enabled > 0.5) {
        hdr = color_grade(hdr);
    }

    vec3 mapped;
    if (pc.tonemap_mode < 0.5) {
        // Exponential (default).
        mapped = vec3(1.0) - exp(-hdr);
    } else if (pc.tonemap_mode < 1.5) {
        // ACES (Narkowicz fitted). The constants are the published fit, not a
        // hand-tuned approximation — do not simplify them, the CPU mirror in
        // Renderer3D.cpp:tonemap() spells the same polynomial.
        const float a = 2.51;
        const float b = 0.03;
        const float c = 2.43;
        const float d = 0.59;
        const float e = 0.14;
        mapped = clamp((hdr * (a * hdr + b)) / (hdr * (c * hdr + d) + e), 0.0, 1.0);
    } else if (pc.tonemap_mode < 2.5) {
        // Reinhard. Never clamps: the curve approaches 1 asymptotically, which
        // is the whole point of the operator.
        mapped = hdr / (vec3(1.0) + hdr);
    } else {
        // Linear. Deliberately unclamped here — the UNorm target clamps on
        // write, and clamping in the shader would hide an over-bright scene
        // from a diagnostic capture.
        mapped = hdr;
    }

    // Gamma correction
    mapped = pow(mapped, vec3(1.0/2.2));
    // Post FX (must match rendering::apply_postfx exactly):
    // saturation around Rec.709 luma, then vignette darkening.
    float luma = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
    mapped = mix(vec3(luma), mapped, pc.saturation);
    vec2 d = in_uv - 0.5;
    mapped *= 1.0 - pc.vignette * smoothstep(0.3, 0.9, length(d));
    out_color = vec4(mapped, 1.0);
}
