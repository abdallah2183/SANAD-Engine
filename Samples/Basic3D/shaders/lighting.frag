#version 450

// Deferred PBR lighting pass.
//
// Reads the gbuffer (BaseColor / Normal / Surface / Depth), reconstructs the
// world position from depth via invViewProj, and evaluates:
//   - 1 directional light
//   - up to 8 point lights   (windowed 1/d² falloff)
//   - up to 8 spot lights    (point falloff × smooth cone)
//
// BRDF: Cook-Torrance GGX (Lambert diffuse, Smith visibility, Schlick
// Fresnel). Output is linear HDR; exposure/tonemapping happen downstream.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D gbuffer_base;
layout(set = 0, binding = 1) uniform sampler2D gbuffer_normal;
layout(set = 0, binding = 2) uniform sampler2D gbuffer_surface;
layout(set = 0, binding = 3) uniform sampler2D gbuffer_depth;
layout(set = 0, binding = 5) uniform sampler2D shadow_map;

struct PointLight {
    vec4 pos_radius;   // xyz = world position, w = radius
    vec4 color_int;    // rgb = color, a = intensity
    vec4 pad;
};

struct SpotLight {
    vec4 pos;          // xyz = world position
    vec4 dir_inner;    // xyz = direction light travels, w = inner cone cos
    vec4 color_int;    // rgb = color, a = intensity
    vec4 outer_pad;    // x = outer cone cos
};

layout(set = 0, binding = 4) uniform FrameUniforms {
    mat4 invViewProj;             // world reconstruction from depth
    vec4 camPos_ambient;          // xyz = camera world position, w = ambient
    vec4 dirLight_dir_enable;     // xyz = light direction (travels this way), w = enabled
    vec4 dirLight_color_int;      // rgb = color, a = intensity
    mat4 lightViewProj[4];        // world -> shadow clip, one per cascade
    vec4 shadow_params;           // x = enabled, y = strength, z = bias, w = texel (1/atlas)
    vec4 cascade_splits;          // view-space FAR distance covered by cascade i
    vec4 cascade_info;            // x = count, y = tile uv scale, z = fade range
    vec4 cascade_bias;            // per-cascade MINIMUM bias, in NDC (added to z above)
    vec4 cam_forward;             // xyz = camera forward axis (cascade selection)
    vec4 sky_zenith;              // rgb zenith color
    vec4 sky_horizon;             // rgb horizon color
    vec4 sky_ground;              // rgb below-horizon color
    vec4 sky_params;              // x = enabled, y = sun disk mul, z = sun glow mul
    vec4 sky_clear;               // rgb fallback when the sky is disabled
    ivec4 counts;                 // x = point count, y = spot count
    PointLight points[8];
    SpotLight spots[8];
} frame;

const float PI = 3.14159265358979;
const int kCascadeCount = 4;      // must match kMaxShadowCascades

// Cascade selection: the shader only ever has a world position, so the camera's
// forward axis is what turns one into the view-space distance the splits are
// expressed in. Returns the last cascade whose far split the point is inside;
// anything past the final split falls into the last cascade rather than off the
// end, so a distant fragment is lit by the widest map instead of unshadowed.
int select_cascade(float view_depth) {
    int count = int(frame.cascade_info.x);
    int idx = 0;
    for (int i = 0; i < kCascadeCount; ++i) {
        if (i >= count - 1) break;
        if (view_depth > frame.cascade_splits[i]) idx = i + 1;
    }
    return idx;
}

// One cascade's occlusion, 3x3 PCF against its tile of the atlas.
//
// Every tap is clamped into the tile's own rect. Without that, a tap near a
// tile edge samples the NEIGHBOURING cascade — a different projection of a
// different slice — and shadows leak across the seam as bright or dark flecks.
float sample_cascade(vec3 pos, int cascade) {
    vec4 lp = frame.lightViewProj[cascade] * vec4(pos, 1.0);
    vec3 ndc = lp.xyz / max(lp.w, 1e-6);
    if (ndc.x < -1.0 || ndc.x > 1.0 || ndc.y < -1.0 || ndc.y > 1.0 ||
        ndc.z < 0.0 || ndc.z > 1.0) {
        return 0.0; // outside this cascade's box: nothing occludes it
    }

    float scale = frame.cascade_info.y;                  // tile size in atlas uv
    vec2 tile = vec2(float(cascade % 2), float(cascade / 2)) * scale;
    vec2 uv = tile + (ndc.xy * 0.5 + 0.5) * scale;

    float texel = frame.shadow_params.w;                 // one atlas texel, in uv
    // Two bias terms. `cascade_bias` is this cascade's own minimum, derived on
    // the CPU from its texel size (cascade_auto_bias) — the near cascade has
    // both smaller texels and a shorter depth span than the fixed box it
    // replaced, so a constant tuned against that box is now far too small and
    // the lit faces self-shadow. `shadow_params.z` is the artist's bias on top.
    float bias = frame.shadow_params.z + frame.cascade_bias[cascade];
    // Half-texel inset so a clamped tap lands on the tile's edge texel centre
    // instead of halfway into the neighbour.
    vec2 lo = tile + vec2(texel * 0.5);
    vec2 hi = tile + vec2(scale - texel * 0.5);

    float occ = 0.0;
    for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
            vec2 tap = clamp(uv + vec2(float(ox), float(oy)) * texel, lo, hi);
            float map_depth = texture(shadow_map, tap).r;
            occ += (ndc.z - bias > map_depth) ? 1.0 : 0.0;
        }
    }
    return occ / 9.0;
}

// Directional shadow factor across the cascade atlas.
//
// The bias travels through cascade_bias because a constant NDC bias is a
// smaller WORLD offset in a wider cascade, which is how acne shows up only in
// the far ones; scaling by each cascade's depth span keeps the physical offset
// the artist tuned constant across all four.
float shadow_factor(vec3 pos) {
    float view_depth = dot(pos - frame.camPos_ambient.xyz, frame.cam_forward.xyz);
    int cascade = select_cascade(view_depth);
    float occ = sample_cascade(pos, cascade);

    // Cross-fade into the next cascade over the tail of this one. The two maps
    // have different texel densities, so a hard hand-off is visible as a line
    // where the shadow edge changes resolution; blending over a short band
    // hides it. Skipped on the last cascade, which has no successor.
    int count = int(frame.cascade_info.x);
    float fade = frame.cascade_info.z;
    if (fade > 0.0 && cascade < count - 1) {
        float far_i = frame.cascade_splits[cascade];
        float near_i = (cascade == 0) ? 0.0 : frame.cascade_splits[cascade - 1];
        float band = max(fade * (far_i - near_i), 1e-4);
        float t = (view_depth - (far_i - band)) / band;
        if (t > 0.0) {
            occ = mix(occ, sample_cascade(pos, cascade + 1), clamp(t, 0.0, 1.0));
        }
    }
    return occ;
}

float D_GGX(float NoH, float alpha) {
    float a2 = alpha * alpha;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d + 1e-7);
}

float visibility_smith(float NoV, float NoL, float alpha) {
    float k = alpha * alpha * 0.5;
    float g_v = NoV / (NoV * (1.0 - k) + k);
    float g_l = NoL / (NoL * (1.0 - k) + k);
    return g_v * g_l;
}

vec3 fresnel_schlick(float cos_theta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

vec3 eval_pbr(vec3 N, vec3 V, vec3 L, vec3 radiance,
              vec3 albedo, float metallic, float roughness) {
    float NoL = dot(N, L);
    if (NoL <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    float NoV = max(dot(N, V), 1e-4);
    float NoH = max(dot(N, H), 0.0);
    float alpha = max(roughness * roughness, 1e-3);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float D = D_GGX(NoH, alpha);
    float Vis = visibility_smith(NoV, NoL, alpha);
    vec3 F = fresnel_schlick(max(dot(H, V), 0.0), F0);

    vec3 specular = (D * Vis) * F;
    vec3 diffuse = (vec3(1.0) - F) * (1.0 - metallic) * albedo / PI;
    return (diffuse + specular) * radiance * NoL;
}

void main() {
    float depth = texture(gbuffer_depth, in_uv).r;

    // Procedural sky for empty pixels (Phase 13): gradient by view-ray
    // height plus a sun disk along the directional light. The ray comes
    // from the same inverse transform that reconstructs positions, so the
    // sky tracks the camera exactly. Colors/multipliers come from SkyParams
    // (see NF/Rendering/Sky.hpp, whose CPU mirror must match this branch).
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
        vec3 sky = (h >= 0.0) ? mix(horizon, zenith, pow(h, 0.6))
                              : mix(horizon, ground_haze, clamp(-h * 3.0, 0.0, 1.0));
        if (frame.dirLight_dir_enable.w > 0.5) {
            vec3 sun_dir = normalize(-frame.dirLight_dir_enable.xyz);
            float cos_a = max(dot(ray, sun_dir), 0.0);
            float disk = smoothstep(0.9996, 0.99985, cos_a);
            float glow = pow(cos_a, 600.0) * 0.6 + pow(cos_a, 24.0) * 0.12;
            vec3 sun_col = frame.dirLight_color_int.rgb * frame.dirLight_color_int.a;
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
    vec3 Lo = vec3(0.0);

    // Directional light (shadow-tested: only the direct term dims)
    if (frame.dirLight_dir_enable.w > 0.5) {
        vec3 L = normalize(-frame.dirLight_dir_enable.xyz);
        vec3 radiance = frame.dirLight_color_int.rgb * frame.dirLight_color_int.a;
        vec3 dir_lo = eval_pbr(N, V, L, radiance, albedo, metallic, roughness);
        if (frame.shadow_params.x > 0.5) {
            dir_lo *= 1.0 - frame.shadow_params.y * shadow_factor(pos);
        }
        Lo += dir_lo;
    }

    // Point lights — windowed inverse-square falloff inside the radius
    for (int i = 0; i < frame.counts.x; ++i) {
        vec3 delta = frame.points[i].pos_radius.xyz - pos;
        float dist = length(delta);
        vec3 L = delta / max(dist, 1e-4);
        float range = max(frame.points[i].pos_radius.w, 1e-4);
        float window = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
        if (window <= 0.0) continue;
        float attenuation = (window * window) / max(dist * dist, 1e-4);
        vec3 radiance = frame.points[i].color_int.rgb * frame.points[i].color_int.a * attenuation;
        Lo += eval_pbr(N, V, L, radiance, albedo, metallic, roughness);
    }

    // Spot lights — point falloff × smooth cone between inner/outer cosines
    for (int i = 0; i < frame.counts.y; ++i) {
        vec3 delta = frame.spots[i].pos.xyz - pos;
        float dist = length(delta);
        vec3 L = delta / max(dist, 1e-4);
        float range = 25.0;
        float window = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
        if (window <= 0.0) continue;

        float cos_angle = dot(-L, normalize(frame.spots[i].dir_inner.xyz));
        float inner = frame.spots[i].dir_inner.w;
        float outer = frame.spots[i].outer_pad.x;
        float cone = smoothstep(outer, inner, cos_angle);
        if (cone <= 0.0) continue;

        float attenuation = (window * window) / max(dist * dist, 1e-4);
        vec3 radiance = frame.spots[i].color_int.rgb * frame.spots[i].color_int.a * cone * attenuation;
        Lo += eval_pbr(N, V, L, radiance, albedo, metallic, roughness);
    }

    vec3 ambient = frame.camPos_ambient.w * albedo * ao;
    vec3 emissive = albedo * emission_strength;

    out_color = vec4(ambient + Lo + emissive, 1.0);
}
