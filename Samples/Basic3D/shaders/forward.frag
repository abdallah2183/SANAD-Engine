#version 450

// Forward (transparency) fragment shader.
//
// Draws a transparent surface directly over the lit HDR image the deferred pass
// produced, blended src-alpha / one-minus-src-alpha. The gbuffer path cannot do
// this: blending into the gbuffer would average the material parameters of two
// surfaces, and the lighting pass would then light a surface that never existed.
//
// The lighting itself is brdf.glsl verbatim — the same block, the same shadow
// atlases, the same BRDF the deferred pass used for the surface behind this
// one. A glass pane that darkens what is behind it must darken it by the light
// this pane actually intercepts, which is only true if the two paths agree
// about what "lit" means.
//
// Bindings 0-3 are the deferred path's gbuffer; this path has no gbuffer, so it
// reuses the set for the material block (7-8) alongside the frame/shadow
// bindings (4-6) the include declares.

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec3 in_world_pos;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 7) uniform MaterialParams {
    vec4 baseColor;   // rgb + a (a is the transparency this path exists for)
    vec4 surface;     // metallic, roughness, ao, emissionStrength
    vec4 misc;        // emission.rgb, useBaseColorTex
} matParams;

layout(set = 0, binding = 8) uniform sampler2D albedoTex;

#include "brdf.glsl"

void main() {
    vec3 N = normalize(in_normal);
    vec3 albedo = matParams.baseColor.rgb;
    if (matParams.misc.w > 0.5) {
        albedo *= texture(albedoTex, in_uv).rgb;
    }
    // The same clamp the deferred path applies (see lighting.frag): a roughness
    // below 0.045 makes the GGX lobe arbitrarily sharp and the surface becomes
    // a mirror that reflects nothing at a glancing angle.
    float metallic = matParams.surface.x;
    float roughness = clamp(matParams.surface.y, 0.045, 1.0);
    float ao = matParams.surface.z;
    float emission_strength = matParams.surface.w;

    vec3 V = normalize(frame.camPos_ambient.xyz - in_world_pos);
    vec3 Lo = direct_lighting(in_world_pos, N, V, albedo, metallic, roughness);

    vec3 ambient = frame.camPos_ambient.w * albedo * ao;
    vec3 emissive = albedo * emission_strength;

    // Post-multiplied alpha: the blend (src*a + dst*(1-a)) weights the colour
    // this surface contributes by the weight it claims over the pixel, so an
    // alpha of 1 replaces the background exactly and an alpha of 0 contributes
    // nothing — both are what a caller setting baseColor.a expects.
    //
    // Fog on the same composited colour the deferred pass fogged, for the same
    // reason: a pane at the far edge of the scene is behind as much air as the
    // opaque surface it partly hides, and fogging only the opaque one would
    // leave a transparent silhouette that gets sharper with distance.
    vec3 lit = ambient + Lo + emissive;
    out_color = vec4(apply_fog(in_world_pos, lit), matParams.baseColor.a);
}
