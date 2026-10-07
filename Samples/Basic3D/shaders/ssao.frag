#version 450

// SSAO raw pass (half resolution): world-space hemisphere occlusion.
//
// Reads depth + gbuffer normal, writes one R8 occlusion value per pixel.
// World-space (not view-space) on purpose: the sample radius is then in world
// units, so a doorway occludes the same at 2 m and at 20 m, and no view
// matrix uniform is needed — invViewProj (frame block prefix, the same 144
// bytes tonemap.frag declares) reconstructs positions, and the push block
// carries viewProj back for reprojecting samples.
//
// Taps: 16 on a golden-angle spiral over the hemisphere around N, rotated per
// pixel by an integer hash (the same hash family as the cloud noise — sin is
// not required to agree anywhere, and a screen-door pattern that depends on
// driver sin would shimmer). Radii grow with the tap index so near geometry
// weighs more. A tap occludes when the visible surface at its screen position
// lies BETWEEN the pixel and the sample point along the ray (geometry hanging
// over the ray blocks the skylight behind it), faded by how far along; past
// the sample the ray escaped. Samples behind the camera or off-screen
// contribute nothing.
//
// Sky pixels (depth ~1) output 1.0: there is no surface to occlude, and
// reconstructing one would be garbage world coordinates.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 1) uniform sampler2D normalTex;
// Frame-block prefix (see tonemap.frag): a std140 block may declare a prefix,
// so only the camera data this pass needs is restated — not the forty floats
// after it.
layout(set = 0, binding = 2) uniform FramePrefix {
    mat4 invViewProj;
    vec4 camPos_ambient;
    mat4 prevViewProj;
} frame;

layout(push_constant) uniform SsaoPush {
    mat4 viewProj;
    vec4 params; // x = radius (world units), y = bias, z = intensity, w = unused
} pc;

const int kSsaoTaps = 16;
const float kGoldenAngle = 2.3999632; // 137.5 degrees: irrational turn, even fill

// Integer hash on pixel coords (see lighting.frag cloud_hash_u32): rotation
// without sin, identical on every driver.
uint ssao_hash_u32(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void main() {
    float depth = texture(depthTex, in_uv).r;
    if (depth >= 0.999999) {
        out_color = vec4(1.0);
        return;
    }
    vec4 world = frame.invViewProj * vec4(in_uv * 2.0 - 1.0, depth, 1.0);
    vec3 pos = world.xyz / max(world.w, 1e-6);
    vec3 N = normalize(texture(normalTex, in_uv).rgb * 2.0 - 1.0);

    float radius = max(pc.params.x, 1e-4);
    float bias = pc.params.y;
    // Tangent frame for rotating the spiral. Fixed world-up reference would
    // degenerate on floors (N == up); the reference follows N instead, and a
    // near-parallel N still leaves a valid (if skewed) frame — skewed taps
    // only redistribute samples, they never NaN.
    vec3 ref = (abs(N.y) > 0.99) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 T = normalize(cross(ref, N));
    vec3 B = cross(N, T);

    ivec2 px = ivec2(gl_FragCoord.xy);
    float rot = float(ssao_hash_u32(uint(px.x) * 0x9e3779b9u ^
                                    ssao_hash_u32(uint(px.y) * 0x85ebca6bu)) &
                      0x00ffffffu) *
                (1.0 / 16777216.0) * 6.2831853;

    float occ = 0.0;
    for (int i = 0; i < kSsaoTaps; ++i) {
        float fi = float(i);
        // Spiral radius grows with the tap: sqrt keeps the annulus density
        // even, and the +0.5 offset keeps the first tap off the center (a tap
        // at the pixel itself always self-occludes).
        float r = sqrt((fi + 0.5) / float(kSsaoTaps));
        float a = rot + fi * kGoldenAngle;
        vec2 tap = vec2(cos(a), sin(a)) * r;
        float hz = sqrt(max(0.0, 1.0 - dot(tap, tap)));
        vec3 dir = normalize(T * tap.x + B * tap.y + N * hz);
        // Sample distance grows with the tap index: near taps probe contact,
        // far taps probe the pocket. The bias lifts the ray off the surface
        // along N so a flat wall does not self-occlude on float noise.
        vec3 sp = pos + N * bias + dir * (radius * (0.25 + 0.75 * r));
        vec4 clip = pc.viewProj * vec4(sp, 1.0);
        if (clip.w <= 1e-6) {
            continue; // behind the camera: nothing to compare against
        }
        vec3 ndc = clip.xyz / clip.w;
        vec2 suv = ndc.xy * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0 ||
            ndc.z < 0.0 || ndc.z > 1.0) {
            continue; // off-screen: no information, not occlusion
        }
        float sdepth = texture(depthTex, suv).r;
        if (sdepth >= 0.999999) {
            continue; // sample ray left the geometry for the sky
        }
        vec4 sworld = frame.invViewProj * vec4(suv * 2.0 - 1.0, sdepth, 1.0);
        vec3 spos = sworld.xyz / max(sworld.w, 1e-6);
        // Occlusion test along the RAY (not against the tangent plane): the
        // sample point hangs in space at ray_len along dir. If the visible
        // surface lies between the pixel and the sample (t < ray_len), that
        // direction is blocked — closer blockers weigh more. At or past the
        // sample the ray escaped; behind the pixel it is irrelevant.
        vec3 ray = sp - pos;
        float ray_len = length(ray);
        float t = dot(spos - pos, ray / max(ray_len, 1e-6));
        if (t > 1e-4 && t < ray_len) {
            occ += 1.0 - t / ray_len;
        }
    }
    float ao = clamp(1.0 - pc.params.z * occ / float(kSsaoTaps), 0.0, 1.0);
    out_color = vec4(ao, ao, ao, 1.0);
}
