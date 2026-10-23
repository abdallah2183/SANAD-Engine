#version 450

// Deferred screen-space decal fragment (Phase 36).
//
// Attachment 0 is GBuffer0 (base colour), attachment 1 is GBuffer2
// (metallic/roughness/ao/emissionStrength). GBuffer1 (normal) and GBuffer3
// (emissive radiance) are deliberately not written: the decal changes what the
// surface looks like, not its shape or what it emits. The normal is READ for
// backface rejection.
//
// The pass runs between GBuffer and Lighting, so the lighting pass sees the
// decal as just another surface. Blending is straight alpha-over:
//   base   = decal.rgb * coverage + base.rgb * (1 - coverage)
//   surface= decal.mra * coverage + surface.mra * (1 - coverage)
// and the surface attachment's alpha factors are Zero/One in the render pass so
// the destination emission strength survives the pass. That detail is why
// `ColorAttachment` grew per-attachment alpha factors; without it a decal over
// an emissive wall would scale the wall's glow by its coverage.
//
// Local space: +Z faces out of the surface, local XY are the albedo plane, and
// the authored size is baked into the world matrix by the CPU. Therefore the
// footprint test is exactly `abs(local.xyz) <= 0.5`. Local +Y maps to increasing
// texture V, so a decal authored upright in the scene reads upright when the
// entity's rotation leaves +Y up.

layout(location = 0) in vec2 in_uv;

layout(location = 0) out vec4 out_base_color;
layout(location = 1) out vec4 out_surface;

layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D normalTex;
// Frame-block prefix (see tonemap.frag / ssao.frag): a std140 block may declare
// a prefix, so this pass restates only the inverse view-projection it needs.
layout(set = 0, binding = 2) uniform FramePrefix {
    mat4 invViewProj;
    vec4 camPos_ambient;
    mat4 prevViewProj;
} frame;
// Same 16-float block the gbuffer shader reads (binding 0 there, binding 3
// here). The decal path lives in its own descriptor layout, so the numbers are
// kept in the same order rather than repacked.
layout(set = 0, binding = 3) uniform MaterialParams {
    vec4 baseColor;   // rgb + a
    vec4 surface;     // metallic, roughness, ao, emissionStrength
    vec4 misc;        // emission.rgb, useBaseColorTex
    vec4 mapFlags;    // normal/mrough/occlusion/emissive presence
} matParams;
layout(set = 0, binding = 4) uniform sampler2D albedoTex;

layout(push_constant) uniform DecalPush {
    mat4 invWorld;  // world -> decal local
    vec4 basis;     // xyz = normalized world +Z (facing), w = opacity
    vec4 params;    // x = hasAlbedo (mirrors matParams.misc.w), yzw unused
} pc;

void main() {
    float depth = texture(depthTex, in_uv).r;
    // Sky pixels have no surface to project onto. Reconstructing one produces
    // a position at the far plane whose local test may pass by accident.
    if (depth >= 0.999999) {
        discard;
    }

    vec4 world_h = frame.invViewProj * vec4(in_uv * 2.0 - 1.0, depth, 1.0);
    vec3 world_pos = world_h.xyz / max(world_h.w, 1e-6);

    vec3 local = (pc.invWorld * vec4(world_pos, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5)))) {
        discard;
    }

    // Backface rejection. The decal projects only onto surfaces whose normal
    // agrees with the decal's outward axis: without this, a shot on a thin wall
    // bleeds through onto the wall behind it. Grazing surfaces (dot == 0) are
    // kept — a decal draped over an edge should not cut off exactly at the
    // silhouette.
    vec3 N = normalize(texture(normalTex, in_uv).rgb * 2.0 - 1.0);
    if (dot(N, pc.basis.xyz) < 0.0) {
        discard;
    }

    float coverage = clamp(pc.basis.w * matParams.baseColor.a, 0.0, 1.0);
    if (coverage <= 0.0001) {
        discard;
    }

    vec3 base = matParams.baseColor.rgb;
    if (pc.params.x > 0.5 && matParams.misc.w > 0.5) {
        base *= texture(albedoTex, local.xy + 0.5).rgb;
    }

    out_base_color = vec4(base, coverage);
    // Alpha is ignored for this attachment (Zero/One factors), but writing the
    // coverage keeps the fragment self-describing and matches the base output.
    out_surface = vec4(matParams.surface.xyz, coverage);
}
