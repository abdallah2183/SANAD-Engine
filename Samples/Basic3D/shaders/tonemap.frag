#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D hdr_input;

layout(push_constant) uniform PushConstants {
    float exposure;
    float vignette;    // 0 = off
    float saturation;  // 1 = neutral
    float unused;
} pc;

void main() {
    vec3 hdr = texture(hdr_input, in_uv).rgb;
    // Simple exposure + Reinhard tonemapping
    vec3 mapped = vec3(1.0) - exp(-hdr * pc.exposure);
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
