// brdf.glsl — the shared lighting body.
//
// Included by BOTH the deferred lighting pass (lighting.frag) and the forward
// transparency pass (forward.frag). A transparent surface is lit by exactly
// the same function the deferred pass applied to the opaque surface behind it,
// so anything in here has one copy or the two paths drift apart — and they
// drift silently, because a slightly different BRDF still produces a
// plausible image.
//
// The bindings are the same in both includers, which is what makes this file
// shareable verbatim: set 0 is the frame block at 4 and the two shadow atlases
// at 5/6 in either pipeline. The deferred pass additionally binds the gbuffer
// at 0-3 and the forward pass binds the material at 7-8, and neither sees the
// other's bindings.
//
// No #version here: the including file owns it, and a second occurrence is a
// hard glslc error.

struct PointLight {
    vec4 pos_radius;   // xyz = world position, w = radius
    vec4 color_int;    // rgb = color, a = intensity
    vec4 pad;
};

struct SpotLight {
    vec4 pos;          // xyz = world position
    vec4 dir_inner;    // xyz = direction light travels, w = inner cone cos
    vec4 color_int;    // rgb = color, a = intensity
    vec4 outer_range;  // x = outer cone cos, y = range (the cone's reach)
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
    ivec4 counts;                 // x = points, y = spots, z = local shadow grid, w = local shadow tile px
    PointLight points[8];
    SpotLight spots[8];
    // Point/spot shadow projectors (Phase 21): one mat4 per atlas TILE. A point
    // light i owns tiles [i*6, i*6+6) and a spot light j owns tile 24+j — the
    // same layout the CPU's point_shadow_tile()/spot_shadow_tile() describe, so
    // a tile index means the same thing on both sides of the upload.
    mat4 localShadowViewProj[28]; // must match kLocalShadowTileCount
    vec4 localShadowParams[28];   // x = enabled, y = strength, z = derived bias, w = light bias
    // Distance fog (appended, so it moves no field above it). x = enabled is a
    // float comparison, not a bool: std140 has no bools, and a bool uniform
    // read back as float is the kind of driver-dependent surprise that turns
    // "fog is off" into "fog is everywhere".
    vec4 fog_color;               // rgb haze tint, w unused
    vec4 fog_params;              // x enabled, y start, z end, w unused
} frame;

layout(set = 0, binding = 5) uniform sampler2D shadow_map;
// Phase 21: the point/spot shadow atlas. Six tiles per shadowed point light
// (one per cube face) plus one per shadowed spot light, in one flat 7x7 grid.
layout(set = 0, binding = 6) uniform sampler2D local_shadow_map;

const float PI = 3.14159265358979;
const int kCascadeCount = 4;      // must match kMaxShadowCascades
const int kLocalShadowTileCount = 28; // kMaxShadowPointLights*6 + kMaxShadowSpotLights
const int kLocalShadowPointTiles = 24; // kMaxShadowPointLights * 6
const int kMaxShadowPointLights = 4;   // must match LocalShadows.hpp
const int kMaxShadowSpotLights = 4;    // must match LocalShadows.hpp

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

// --- Filtering --------------------------------------------------------------
//
// Fixed 9-tap Poisson disk, unit radius (scaled to kShadowFilterRadius texels
// at the call site). Same tap count as the 3x3 grid it replaced — identical
// ALU and texture cost — but the offsets are not a periodic lattice, so a
// shadow edge stepping across the filter lands on different offset
// combinations instead of repeating one grid phase. That repetition is what
// showed up as stepped bands along every shadow edge.
const vec2 kShadowTaps[9] = vec2[9](
    vec2( 0.0000000,  0.0000000),
    vec2( 0.5054701,  0.3288777),
    vec2( 0.4147301, -0.5262884),
    vec2(-0.4282048, -0.3955864),
    vec2(-0.5349605,  0.4020852),
    vec2( 0.9475536,  0.2156003),
    vec2( 0.3655724,  0.9498603),
    vec2(-0.4122908,  0.9846612),
    vec2(-0.9664947, -0.2188046)
);

// How far the disk reaches, in texels. Wide enough that the bilinear depth
// fetch blends four texels per tap (an effective footprint well past 5x5 for
// 9 taps), tight enough that contact points stay crisp instead of washing
// into a grey halo around every caster.
const float kShadowFilterRadius = 2.0;

// Multiplier on the base bias for surfaces the light grazes. The CPU derives
// each projector's minimum for tan(theta) up to its bias slope constant
// (2.0 — a ~63-degree worst case), so a face no steeper than that needs
// nothing extra: the scale stays at 1.0 and the tuned look is untouched. Past
// that angle tan(theta) keeps growing while a fixed minimum stops covering
// the depth a texel-sized step gains along the surface, and acne returns on
// exactly the steep, curved mesh faces the minimum was meant to protect —
// which is why the scale must be a function of the RECEIVING surface's N·L,
// not another global constant. The 4x cap keeps a near-silhouette fragment
// from pushing the bias into peter-panning territory.
float slope_bias_scale(float NoL) {
    float n = max(NoL, 1e-3);
    float tan_theta = sqrt(max(0.0, 1.0 - n * n)) / n;
    return clamp(tan_theta * 0.5, 1.0, 4.0);
}

// One cascade's occlusion, 9-tap disk PCF against its tile of the atlas.
//
// Every tap is clamped into the tile's own rect. Without that, a tap near a
// tile edge samples the NEIGHBOURING cascade — a different projection of a
// different slice — and shadows leak across the seam as bright or dark flecks.
float sample_cascade(vec3 pos, int cascade, float NoL) {
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
    // the lit faces self-shadow. `shadow_params.z` is the artist's bias on
    // top. Both scale up on grazing surfaces (slope_bias_scale above).
    float bias = (frame.shadow_params.z + frame.cascade_bias[cascade])
               * slope_bias_scale(NoL);
    // Half-texel inset so a clamped tap lands on the tile's edge texel centre
    // instead of halfway into the neighbour.
    vec2 lo = tile + vec2(texel * 0.5);
    vec2 hi = tile + vec2(scale - texel * 0.5);

    float occ = 0.0;
    for (int k = 0; k < 9; ++k) {
        vec2 tap = clamp(uv + kShadowTaps[k] * (kShadowFilterRadius * texel),
                         lo, hi);
        float map_depth = texture(shadow_map, tap).r;
        occ += (ndc.z - bias > map_depth) ? 1.0 : 0.0;
    }
    return occ / 9.0;
}

// Directional shadow factor across the cascade atlas.
//
// The bias travels through cascade_bias because a constant NDC bias is a
// smaller WORLD offset in a wider cascade, which is how acne shows up only in
// the far ones; scaling by each cascade's depth span keeps the physical offset
// the artist tuned constant across all four.
float shadow_factor(vec3 pos, float NoL) {
    float view_depth = dot(pos - frame.camPos_ambient.xyz, frame.cam_forward.xyz);
    int cascade = select_cascade(view_depth);
    float occ = sample_cascade(pos, cascade, NoL);

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
            occ = mix(occ, sample_cascade(pos, cascade + 1, NoL), clamp(t, 0.0, 1.0));
        }
    }

    // Dissolve over the tail of the LAST cascade. A fragment past the final
    // split keeps sampling that cascade while its world position still lands
    // inside the ortho box, then snaps to 0 the moment it leaves — a hard
    // line across the world at the shadow reach, on the ground and on every
    // caster crossing it. Fading over the same fraction the cross-fade uses
    // turns the reach into a gradual thinning instead, for three adds.
    if (fade > 0.0) {
        float far_last = frame.cascade_splits[count - 1];
        float near_last = (count > 1) ? frame.cascade_splits[count - 2] : 0.0;
        float tail = max(fade * (far_last - near_last), 1e-4);
        occ *= 1.0 - clamp((view_depth - (far_last - tail)) / tail, 0.0, 1.0);
    }
    return occ;
}

// --- Local shadows (Phase 21) ---------------------------------------------

// Which of a point light's six tiles a fragment's shadow lives in. The six
// projectors partition every direction (see select_face in LocalShadows.cpp),
// so the axis the light->fragment direction is MOST aligned with names the face
// and that component's sign picks the positive or negative one. Ties resolve to
// the lower index, matching select_face's candidate order exactly — a fragment
// on a seam must land in the same tile the CPU built for it.
int point_shadow_face(vec3 dir) {
    vec3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z) return dir.x > 0.0 ? 0 : 1;
    if (a.y >= a.z)               return dir.y > 0.0 ? 2 : 3;
    return dir.z > 0.0 ? 4 : 5;
}

// One tile's occlusion: 9-tap disk PCF with the taps clamped inside the tile,
// for the same reason as the cascade path — a neighbouring tile is a DIFFERENT
// projector, and reading its depth leaks a fleck of unrelated shadow across
// the seam.
//
// The matrix is the authority for NDC here; the face index only CHOSE the tile.
// Reconstructing NDC analytically in the shader would duplicate the handedness
// of all six bases, and a sign error there shows up as a mirrored tile that
// still returns a plausible depth — invisible in a debugger.
float sample_local_shadow(vec3 pos, int tile, float NoL) {
    vec4 lp = frame.localShadowViewProj[tile] * vec4(pos, 1.0);
    vec3 ndc = lp.xyz / max(lp.w, 1e-6);
    if (ndc.x < -1.0 || ndc.x > 1.0 || ndc.y < -1.0 || ndc.y > 1.0 ||
        ndc.z < 0.0 || ndc.z > 1.0) {
        return 0.0; // outside this projector: nothing occludes it
    }

    int grid = max(frame.counts.z, 1);              // tiles per atlas row
    float scale = 1.0 / float(grid);                // one tile, in atlas uv
    vec2 tile_uv = vec2(float(tile % grid), float(tile / grid)) * scale;
    vec2 uv = tile_uv + (ndc.xy * 0.5 + 0.5) * scale;

    float texel = scale / max(float(frame.counts.w), 1.0); // one texel, in uv
    // Derived minimum (per-tile, from this projector's texel size) plus the
    // light's own bias — the same pair the cascade path adds as
    // shadow_params.z + cascade_bias[i] — both scaled on grazing surfaces by
    // the same receiver-dependent factor.
    float bias = (frame.localShadowParams[tile].z + frame.localShadowParams[tile].w)
               * slope_bias_scale(NoL);
    vec2 lo = tile_uv + vec2(texel * 0.5);
    vec2 hi = tile_uv + vec2(scale - texel * 0.5);

    float occ = 0.0;
    for (int k = 0; k < 9; ++k) {
        vec2 tap = clamp(uv + kShadowTaps[k] * (kShadowFilterRadius * texel),
                         lo, hi);
        float map_depth = texture(local_shadow_map, tap).r;
        occ += (ndc.z - bias > map_depth) ? 1.0 : 0.0;
    }
    return occ / 9.0;
}

// A point light's shadow factor. The enabled check is per-tile and tiles are
// indexed by light, so a light that did not opt in costs one comparison and no
// texture work at all.
//
// The guard against kMaxShadowPointLights is not redundant with the enabled
// flag: counts.x is the LIGHT cap (8) and the atlas only holds tiles for the
// first kMaxShadowPointLights (4) of them. A light past the shadow cap would
// compute tile = i*6 + face >= 28 and read localShadowViewProj out of bounds —
// undefined behaviour in a uniform array, not a clean "no shadow".
float point_shadow_factor(vec3 pos, int light, float NoL) {
    if (light >= kMaxShadowPointLights) return 0.0;
    int tile = light * 6 + point_shadow_face(pos - frame.points[light].pos_radius.xyz);
    if (frame.localShadowParams[tile].x <= 0.5) return 0.0;
    return sample_local_shadow(pos, tile, NoL);
}

float spot_shadow_factor(vec3 pos, int light, float NoL) {
    if (light >= kMaxShadowSpotLights) return 0.0;
    int tile = kLocalShadowPointTiles + light;
    if (frame.localShadowParams[tile].x <= 0.5) return 0.0;
    return sample_local_shadow(pos, tile, NoL);
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

// Direct lighting for one surface: the directional light, the point lights and
// the spot lights, each shadowed. This is the body both callers share — the
// deferred pass after reconstructing the surface from the gbuffer, the forward
// pass after evaluating it from the interpolated vertex attributes.
//
// Emission is deliberately NOT here: the deferred pass reads its emission
// strength from a gbuffer channel, the forward pass from the material block,
// and folding either in here would drag the other's storage into the shared
// file. Ambient likewise leaves the albedo×ao product to the caller.
vec3 direct_lighting(vec3 pos, vec3 N, vec3 V,
                     vec3 albedo, float metallic, float roughness) {
    vec3 Lo = vec3(0.0);

    // Directional light (shadow-tested: only the direct term dims)
    if (frame.dirLight_dir_enable.w > 0.5) {
        vec3 L = normalize(-frame.dirLight_dir_enable.xyz);
        vec3 radiance = frame.dirLight_color_int.rgb * frame.dirLight_color_int.a;
        float dir_NoL = dot(N, L);
        vec3 dir_lo = eval_pbr(N, V, L, radiance, albedo, metallic, roughness);
        if (frame.shadow_params.x > 0.5) {
            // The receiving angle rides along: the shadow test scales its bias
            // by how steeply the light hits THIS surface. Irrelevant when the
            // BRDF already returned 0 (NoL <= 0), harmless otherwise.
            dir_lo *= 1.0 - frame.shadow_params.y * shadow_factor(pos, max(dir_NoL, 0.0));
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
        vec3 point_lo = eval_pbr(N, V, L, radiance, albedo, metallic, roughness);
        // The light's six tiles carry the same strength, so tile i*6 is a
        // per-light switch whether or not this light casts. point_shadow_factor
        // returns 0 for a light that never opted in. The N·L term goes with it
        // for the receiver-dependent bias (see slope_bias_scale).
        float occ = point_shadow_factor(pos, i, max(dot(N, L), 0.0));
        if (occ > 0.0) {
            point_lo *= 1.0 - frame.localShadowParams[i * 6].y * occ;
        }
        Lo += point_lo;
    }

    // Spot lights — point falloff × smooth cone between inner/outer cosines
    for (int i = 0; i < frame.counts.y; ++i) {
        vec3 delta = frame.spots[i].pos.xyz - pos;
        float dist = length(delta);
        vec3 L = delta / max(dist, 1e-4);
        // The cone's reach, authored per light on the CPU (SpotLight::range).
        // This used to be a literal 25.0 that had to match
        // kLocalShadowSpotDefaultFar by hand; the CPU now uploads the same value
        // the shadow projector uses as its far plane, so a light's pool of
        // light and the shadow it casts cannot disagree about where it ends.
        // Clamped the same way the point path clamps its radius.
        float range = max(frame.spots[i].outer_range.y, 1e-4);
        float window = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
        if (window <= 0.0) continue;

        float cos_angle = dot(-L, normalize(frame.spots[i].dir_inner.xyz));
        float inner = frame.spots[i].dir_inner.w;
        float outer = frame.spots[i].outer_range.x;
        float cone = smoothstep(outer, inner, cos_angle);
        if (cone <= 0.0) continue;

        float attenuation = (window * window) / max(dist * dist, 1e-4);
        vec3 radiance = frame.spots[i].color_int.rgb * frame.spots[i].color_int.a * cone * attenuation;
        vec3 spot_lo = eval_pbr(N, V, L, radiance, albedo, metallic, roughness);
        float occ = spot_shadow_factor(pos, i, max(dot(N, L), 0.0));
        if (occ > 0.0) {
            spot_lo *= 1.0 - frame.localShadowParams[kLocalShadowPointTiles + i].y * occ;
        }
        Lo += spot_lo;
    }

    return Lo;
}

// Distance fog: fades a surface toward a haze colour over world-space distance
// from the camera. Applies to the composited result (ambient + direct +
// emission), not to the direct term alone — haze attenuates skylight and
// emission on the way to the eye too, and fogging only the direct light would
// leave a dimmed scene with an unaccounted-for bright ambient.
//
// NOT inside direct_lighting, for the same reason emission isn't: the two
// callers assemble their final colour from different sources (the deferred
// pass reads emission from a gbuffer channel, the forward pass from the
// material block), and folding the fog in here would make this file
// responsible for each caller's storage. Fog is a property of the view, not
// of the BRDF.
//
// Distance is the Euclidean view-ray length, not view-space depth: atmospheric
// attenuation is a path integral along the ray, so an off-axis surface at the
// same depth is genuinely farther away and fogs more. The visible difference
// is the haze thickening toward the frame corners, which is what real air
// does.
//
// The sky is exempt by structure rather than by a flag: lighting.frag returns
// for sky pixels before it can reach this call, and it must — the sky is at
// infinity, so any finite `end` saturates the factor and would replace the
// whole sky with the fog colour. A scene that wants distant terrain to fade
// INTO its horizon sets the fog colour to the horizon colour; that coupling is
// the caller's, because two scenes disagree about whether haze should match
// the sky.
vec3 apply_fog(vec3 pos, vec3 color) {
    if (frame.fog_params.x < 0.5) return color;
    float dist = length(pos - frame.camPos_ambient.xyz);
    // A degenerate band (end <= start) would divide by zero. Clamping the
    // denominator makes that read as an instant fade instead of NaN, and a
    // negative span clamps the factor to 0 on its own.
    float span = max(frame.fog_params.z - frame.fog_params.y, 1e-4);
    float f = clamp((dist - frame.fog_params.y) / span, 0.0, 1.0);
    return mix(color, frame.fog_color.rgb, f);
}
