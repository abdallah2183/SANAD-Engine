#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D hdr_input;

// The mode is carried as a float rather than an int because the renderer
// uploads the whole block as one float[4] (see Renderer3D's tonemap push).
// Values are compared by range, not equality, so a numeric fuzz from the
// float path cannot land between two modes.
//
//   0 Exponential — 1 - exp(-x). The shipped default and the *only* mode the
//     golden pixels were authored against; keep it at 0 so an unset block
//     reproduces the old frame bit for bit.
//   1 ACES       — Narkowicz's fitted filmic curve.
//   2 Reinhard   — x / (1 + x), the classic photographic operator.
//   3 Linear     — exposure only, for diagnostics and HDR capture.
layout(push_constant) uniform PushConstants {
    float exposure;
    float vignette;    // 0 = off
    float saturation;  // 1 = neutral
    float tonemap_mode;
} pc;

void main() {
    vec3 hdr = texture(hdr_input, in_uv).rgb * pc.exposure;

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
