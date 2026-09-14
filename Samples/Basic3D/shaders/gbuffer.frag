#version 450

// GBuffer fragment shader — writes the deferred material surface:
//   attachment 0: BaseColor (rgba)
//   attachment 1: Normal (encoded *0.5+0.5)
//   attachment 2: Metallic (r), Roughness (g), AO (b), EmissionStrength (a)
//
// Material parameters arrive through a per-instance uniform block (binding 0)
// so changing them never requires a new pipeline. An optional albedo texture
// (binding 1) multiplies the scalar base color when enabled.

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec4 out_base_color;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_surface;

layout(set = 0, binding = 0) uniform MaterialParams {
    vec4 baseColor;   // rgb + a (alpha kept for future transparency)
    vec4 surface;     // metallic, roughness, ao, emissionStrength
    vec4 misc;        // emission.rgb, useBaseColorTex
} matParams;

layout(set = 0, binding = 1) uniform sampler2D albedoTex;

void main() {
    vec3 n = normalize(in_normal);
    vec3 base = matParams.baseColor.rgb;
    if (matParams.misc.w > 0.5) {
        base *= texture(albedoTex, in_uv).rgb;
    }
    out_base_color = vec4(base, matParams.baseColor.a);
    out_normal = vec4(n * 0.5 + 0.5, 1.0);
    out_surface = vec4(matParams.surface.x, matParams.surface.y,
                       matParams.surface.z, matParams.surface.w);
}
