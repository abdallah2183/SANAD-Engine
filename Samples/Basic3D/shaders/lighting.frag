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
    mat4 lightViewProj;           // world -> shadow-map clip
    vec4 shadow_params;           // x = enabled, y = strength, z = bias, w = texel (1/size)
    ivec4 counts;                 // x = point count, y = spot count
    PointLight points[8];
    SpotLight spots[8];
} frame;

const float PI = 3.14159265358979;

// Directional shadow factor via the fixed ortho shadow map (3x3 PCF).
// Same NDC convention as the main passes (uv = ndc*0.5+0.5, depth [0,1]),
// so the lookup is self-consistent with how the map was rendered.
float shadow_factor(vec3 pos) {
    vec4 lp = frame.lightViewProj * vec4(pos, 1.0);
    vec3 ndc = lp.xyz / max(lp.w, 1e-6);
    vec2 suv = ndc.xy * 0.5 + 0.5;
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0 || ndc.z > 1.0) {
        return 0.0; // outside the ortho box: fully lit
    }
    float bias = frame.shadow_params.z;
    float texel = frame.shadow_params.w;
    float occ = 0.0;
    for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
            float map_depth = texture(shadow_map, suv + vec2(float(ox), float(oy)) * texel).r;
            occ += (ndc.z - bias > map_depth) ? 1.0 : 0.0;
        }
    }
    return occ / 9.0;
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
    // sky tracks the camera exactly. Output is linear HDR like everything
    // else; tonemapping happens downstream.
    if (depth >= 0.999999) {
        vec4 far = frame.invViewProj * vec4(in_uv * 2.0 - 1.0, 0.99999, 1.0);
        vec3 ray = normalize((far.xyz / far.w) - frame.camPos_ambient.xyz);
        float h = clamp(ray.y, -1.0, 1.0);
        vec3 zenith = vec3(0.20, 0.42, 0.85);
        vec3 horizon = vec3(0.62, 0.72, 0.82);
        vec3 ground_haze = vec3(0.09, 0.09, 0.11);
        vec3 sky = (h >= 0.0) ? mix(horizon, zenith, pow(h, 0.6))
                              : mix(horizon, ground_haze, clamp(-h * 3.0, 0.0, 1.0));
        if (frame.dirLight_dir_enable.w > 0.5) {
            vec3 sun_dir = normalize(-frame.dirLight_dir_enable.xyz);
            float cos_a = max(dot(ray, sun_dir), 0.0);
            float disk = smoothstep(0.9996, 0.99985, cos_a);
            float glow = pow(cos_a, 600.0) * 0.6 + pow(cos_a, 24.0) * 0.12;
            vec3 sun_col = frame.dirLight_color_int.rgb * frame.dirLight_color_int.a;
            sky += sun_col * (disk * 4.0 + glow);
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
