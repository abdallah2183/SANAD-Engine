#version 450

// Deferred PBR lighting pass.
//
// Reads the gbuffer (BaseColor / Normal / Surface / Depth), reconstructs the
// world position from depth via invViewProj, and evaluates the shared lighting
// body (1 directional light, up to 8 point lights, up to 8 spot lights,
// Cook-Torrance GGX, shadow-tested). Output is linear HDR; exposure /
// tonemapping happen downstream.
//
// The BRDF, the frame uniform block and the two shadow atlases live in
// brdf.glsl, shared with forward.frag: a transparent surface must be lit by the
// same function that lit the opaque surface behind it, and two copies of a BRDF
// in one engine always end up disagree.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D gbuffer_base;
layout(set = 0, binding = 1) uniform sampler2D gbuffer_normal;
layout(set = 0, binding = 2) uniform sampler2D gbuffer_surface;
layout(set = 0, binding = 3) uniform sampler2D gbuffer_depth;

#include "brdf.glsl"

void main() {
    float depth = texture(gbuffer_depth, in_uv).r;

    // Procedural sky for empty pixels (Phase 13): gradient by view-ray
    // height plus a sun disk along the directional light. The ray comes
    // from the same inverse transform that reconstructs positions, so the
    // sky tracks the camera exactly. Colors/multipliers come from SkyParams
    // (see NF/Rendering/Sky.hpp, whose CPU mirror must match this branch).
    // Output is linear HDR like everything else; tonemapping happens downstream.
    // Procedural sky for empty pixels (Phase 13, natural palette Phase 21): a
    // blue gradient overhead, a pale haze band a few degrees above the horizon,
    // sun-forward warm scattering inside that band, a sun disk with a halo and a
    // fade into the ground colour below the horizon. The ray comes from the same
    // inverse transform that reconstructs positions, so the sky tracks the
    // camera exactly. Constants and curve live in NF/Rendering/Sky.hpp
    // (compute_sky_color is the CPU twin of this block — keep them in sync).
    // Output is linear HDR like everything else; tonemapping happens downstream.
    if (depth >= 0.999999) {
        if (frame.sky_params.x < 0.5) {
            out_color = vec4(frame.sky_clear.rgb, 1.0);
            return;
        }
        vec4 far = frame.invViewProj * vec4(in_uv * 2.0 - 1.0, 0.99999, 1.0);
        vec3 ray = normalize((far.xyz / far.w) - frame.camPos_ambient.xyz);
        float h = clamp(ray.y, -1.0, 1.0);
        vec3 zenith = frame.sky_zenith.rgb;
        vec3 horizon = frame.sky_horizon.rgb;
        vec3 ground_haze = frame.sky_ground.rgb;
        if (h < 0.0) {
            // Below the horizon: fade into the ground over 35 degrees.
            out_color = vec4(mix(horizon, ground_haze, clamp(-h * 2.8571428, 0.0, 1.0)), 1.0);
            return;
        }
        vec3 sky = mix(horizon, zenith, pow(h, 0.62));
        // Haze band: zero at the horizon and at the zenith, peaking ~3.5 degrees
        // above eye level. d*d, not pow(d, 2.0): pow of a negative base is
        // undefined in GLSL and up - 0.06 goes negative near the horizon.
        float d = (h - 0.06) * 10.0;
        float band = exp(-(d * d)) * smoothstep(0.0, 0.015, h);
        sky += horizon * (0.50 * band);
        if (frame.dirLight_dir_enable.w > 0.5) {
            vec3 sun_dir = normalize(-frame.dirLight_dir_enable.xyz);
            float cos_a = max(dot(ray, sun_dir), 0.0);
            vec3 sun_col = frame.dirLight_color_int.rgb * frame.dirLight_color_int.a;
            float sun_peak = max(max(sun_col.r, sun_col.g), sun_col.b);
            vec3 sun_tint = (sun_peak > 1e-6) ? (sun_col / sun_peak) : vec3(1.0);
            // Warm patch the low sun paints on the haze (dies with sun_glow).
            sky += sun_tint * (0.30 * frame.sky_params.z * band * pow(cos_a, 4.0));
            // Disk (~2.5 degrees across, so it reads at viewport resolution)
            // plus halo.
            float disk = smoothstep(0.9990, 0.9997, cos_a);
            float glow = pow(cos_a, 600.0) * 0.6 + pow(cos_a, 24.0) * 0.12;
            sky += sun_col * (disk * 4.0 * frame.sky_params.y + glow * frame.sky_params.z);
        }
        out_color = vec4(sky, 1.0);
        return;
    }

    vec3 albedo = texture(gbuffer_base, in_uv).rgb;
    vec3 N = normalize(texture(gbuffer_normal, in_uv).rgb * 2.0 - 1.0);
    vec4 surface = texture(gbuffer_surface, in_uv);
    float metallic = surface.r;
    float roughness = clamp(surface.g, 0.045, 1.0);
    float ao = surface.b;
    float emission_strength = surface.a;

    // World position from depth: ndc = uv*2-1 with Vulkan's [0,1] depth.
    vec4 world = frame.invViewProj * vec4(in_uv * 2.0 - 1.0, depth, 1.0);
    vec3 pos = world.xyz / world.w;

    vec3 V = normalize(frame.camPos_ambient.xyz - pos);
    vec3 Lo = direct_lighting(pos, N, V, albedo, metallic, roughness);

    vec3 ambient = frame.camPos_ambient.w * albedo * ao;
    vec3 emissive = albedo * emission_strength;

    // Fog last, on the composited colour: haze attenuates the ambient and the
    // emission on the way to the eye as well, and applying it before the sum
    // would leave the terms disagreeing about how much air is between them.
    // Sky pixels already returned above, so this only touches real surfaces.
    vec3 lit = ambient + Lo + emissive;
    out_color = vec4(apply_fog(pos, lit), 1.0);
}
