#version 450

// Deferred PBR lighting pass.
//
// Reads the gbuffer (BaseColor / Normal / Surface / Emissive / Depth), reconstructs the
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
// Binding 7, not 4-6: 4-6 are the frame uniforms and the two shadow atlases,
// declared by brdf.glsl and shared with forward.frag, which reuses 7 and 8 for
// its material block. The emissive attachment is deferred-only, so it must sit
// above the shared block — and it is deliberately NOT declared in brdf.glsl for
// the same reason: the forward path has the material block and does not need it.
layout(set = 0, binding = 7) uniform sampler2D gbuffer_emissive;
// Binding 8: the sky environment bake for image-based lighting (SkyEnv —
// equirect HDR, mip chain for roughness). Above the shared block for the same
// reason as the emissive attachment: the forward pass reuses 7-8 for its
// material block, so the bake rides a sampler PARAMETER into ibl_contrib and
// each includer declares its own binding (9 in forward.frag).
layout(set = 0, binding = 8) uniform sampler2D env_map;

#include "brdf.glsl"

// --- Procedural clouds (P4 weather) ------------------------------------------
//
// An INTEGER hash, not the usual `fract(sin(dot(...)))` trick, and that is the
// whole point: `sin` is not required to agree between a driver and the C++
// runtime, so a sin-based hash could not be mirrored — and an unmirrorable sky
// is one no test can pin. NF/Rendering/Sky.hpp carries the same constants and
// the same arithmetic; the tests compare them.

// Must match rendering::kCloudOctaves and kCloudFbmNorm.
const int kCloudOctaves = 4;
const float kCloudFbmNorm = 0.9375;

uint cloud_hash_u32(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float cloud_hash_cell(ivec2 p) {
    const uint h = cloud_hash_u32(uint(p.x) * 0x9e3779b9u ^
                                  cloud_hash_u32(uint(p.y) * 0x85ebca6bu));
    return float(h & 0x00ffffffu) * (1.0 / 16777216.0);
}

float cloud_value_noise(vec2 uv) {
    const vec2 i = floor(uv);
    const vec2 f = uv - i;
    const vec2 u = f * f * (3.0 - 2.0 * f);
    const ivec2 c = ivec2(i);
    const float a = cloud_hash_cell(c);
    const float b = cloud_hash_cell(c + ivec2(1, 0));
    const float cc = cloud_hash_cell(c + ivec2(0, 1));
    const float d = cloud_hash_cell(c + ivec2(1, 1));
    const float p = a + (b - a) * u.x;
    const float q = cc + (d - cc) * u.x;
    return p + (q - p) * u.y;
}

float cloud_density(vec2 uv) {
    float sum = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    for (int i = 0; i < kCloudOctaves; ++i) {
        sum += cloud_value_noise(uv * freq) * amp;
        freq *= 2.0;
        amp *= 0.5;
    }
    return sum / kCloudFbmNorm;
}

// The layer's alpha over the sky.
//
// SAMPLING SPACE: the view ray is intersected with a horizontal plane
// `skyCloud.y` units ABOVE THE CAMERA, and the world XZ of that intersection
// over `skyCloud.z` is the noise coordinate. Drift is `skyCloud.w` along +X. So
// the layer is infinite, camera-relative in altitude and world-anchored in XZ —
// which is what keeps it put as the camera moves. Zero for a ray that never
// reaches the plane, so the layer stays above the horizon.
float cloud_alpha(vec3 ray) {
    if (frame.skyCloud.x <= 0.0 || ray.y <= 1.0e-4) {
        return 0.0;
    }
    const float t = frame.skyCloud.y / ray.y;
    const float inv = 1.0 / max(frame.skyCloud.z, 1.0e-3);
    const vec2 uv = vec2((frame.camPos_ambient.x + ray.x * t) * inv + frame.skyCloud.w * 0.05,
                         (frame.camPos_ambient.z + ray.z * t) * inv);
    const float d = clamp(cloud_density(uv), 0.0, 1.0);
    const float e = max(1.0 - frame.skyCloud.x, 1.0e-3);
    const float x = clamp((d - frame.skyCloud.x) / e, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

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
        // Procedural cloud layer, applied LAST so it sits over the gradient,
        // the haze band and the sun — a cloud the sun showed through would read
        // as a hole in the layer rather than as cloud.
        const float cloud_a = cloud_alpha(ray);
        if (cloud_a > 0.0) {
            // Lit from the sun side: brighter toward the sun, darker away from
            // it, which is what makes the layer read as volume rather than as a
            // flat decal. Falls back to a neutral 0.5 with no light, matching
            // the sky's own "no sun" path.
            float sun_side = 0.5;
            if (frame.dirLight_dir_enable.w > 0.5) {
                sun_side = 0.5 + 0.5 * dot(ray, normalize(-frame.dirLight_dir_enable.xyz));
            }
            const vec3 cloud_col =
                mix(horizon * 0.62, horizon * 1.30, clamp(sun_side, 0.0, 1.0));
            sky = mix(sky, cloud_col, cloud_a);
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

    // Image-based lighting replaces the scalar ambient when the bake is live:
    // the sky (gradient + haze + sun) lights the surface directionally instead
    // of one flat grey. The scalar stays as the fallback for a renderer that
    // never baked (or a device that refused the texture) — an unset IBL reads
    // as the old look, never as black.
    vec3 ambient = frame.camPos_ambient.w * albedo * ao;
    if (frame.ibl_params.x > 0.5) {
        ambient = ibl_contrib(env_map, frame.ibl_params.w, N, V,
                              albedo, metallic, roughness, ao,
                              frame.ibl_params.y, frame.ibl_params.z);
    }
    // The material's own emission colour, NOT the albedo. It was albedo for as
    // long as the colour had nowhere to live in the gbuffer: a material with a
    // dark base colour and a bright `emission:` rendered black. `emission_strength`
    // is still read above because the gbuffer stores the two already multiplied
    // and nothing else needs the scalar.
    vec3 emissive = texture(gbuffer_emissive, in_uv).rgb;

    // Fog last, on the composited colour: haze attenuates the ambient and the
    // emission on the way to the eye as well, and applying it before the sum
    // would leave the terms disagreeing about how much air is between them.
    // Sky pixels already returned above, so this only touches real surfaces.
    vec3 lit = ambient + Lo + emissive;
    out_color = vec4(apply_fog(pos, lit), 1.0);
}
