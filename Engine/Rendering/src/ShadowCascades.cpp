#include <NF/Rendering/ShadowCascades.hpp>

#include <algorithm>
#include <cmath>

namespace nf::rendering {
namespace {

/// View-space distance -> NDC depth for the engine's projection convention
/// (Vulkan depth [0,1], near -> 0, far -> 1; see Mat4::perspective).
///
/// Deriving this per projection type rather than assuming "near-plane point
/// scaled by d/near" keeps orthographic cameras correct: an ortho frustum's
/// corner rays are parallel, not eye-divergent, so the scaling shortcut that is
/// exact for perspective draws the wrong slice for ortho.
float ndc_depth_for_distance(const Camera& cam, float distance) {
    const float n = cam.near_plane;
    const float f = cam.far_plane;
    if (!(f > n) || n <= 0.0f) return 0.0f;
    if (distance <= n) return 0.0f;
    if (distance >= f) return 1.0f;
    if (cam.type == Camera::ProjectionType::Orthographic) {
        return (distance - n) / (f - n);
    }
    // Mat4::perspective has m[2][2] = f/(n-f), m[3][2] = nf/(n-f), m[2][3] = -1,
    // which inverts to this closed form.
    return f * (1.0f - n / distance) / (f - n);
}

/// The eight world-space corners of the camera frustum slice between two
/// view-space distances. Corners come from the inverse view-projection at the
/// NDC depths those distances map to, so the engine's Y-flip and [0,1] depth
/// range are honoured by construction instead of being re-derived here.
void slice_corners(const Camera& cam, float d_near, float d_far, Vec3 out[8]) {
    const Mat4 inv_vp = cam.view_projection.inverse();
    const float z_near = ndc_depth_for_distance(cam, d_near);
    const float z_far = ndc_depth_for_distance(cam, d_far);

    const float xs[2] = {-1.0f, 1.0f};
    const float ys[2] = {-1.0f, 1.0f};
    u32 i = 0;
    for (const float z : {z_near, z_far}) {
        for (const float y : ys) {
            for (const float x : xs) {
                out[i++] = inv_vp.transform_point(Vec3{x, y, z});
            }
        }
    }
}

/// Light basis: same `up` choice the single-cascade path used, so a light that
/// points straight down does not degenerate into a zero cross product.
Vec3 light_up_for(const Vec3& dir) {
    return (std::fabs(dir.y) > 0.98f) ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
}

} // namespace

void compute_cascade_splits(float near_z, float far_z, u32 count, float lambda,
                            float* out_splits) {
    if (out_splits == nullptr) return;
    count = std::clamp(count, 1u, kMaxShadowCascades);
    lambda = std::clamp(lambda, 0.0f, 1.0f);

    // A degenerate range has no meaningful interior: emit the two boundaries
    // and stop, rather than taking log/ratios of a non-positive far plane.
    if (!(far_z > near_z) || near_z <= 0.0f) {
        for (u32 i = 0; i <= count; ++i) out_splits[i] = near_z;
        return;
    }

    const float ratio = far_z / near_z;
    for (u32 i = 0; i <= count; ++i) {
        const float p = static_cast<float>(i) / static_cast<float>(count);
        const float log_d = near_z * std::pow(ratio, p);
        const float uni_d = near_z + (far_z - near_z) * p;
        out_splits[i] = lambda * log_d + (1.0f - lambda) * uni_d;
    }
    // Pin the ends exactly: pow() rounding would otherwise leave out[count]
    // a hair off the far plane and the last cascade would not reach it.
    out_splits[0] = near_z;
    out_splits[count] = far_z;
}

CascadeFit fit_cascade(const Camera& camera, const Vec3& light_dir, float split_near,
                       float split_far, u32 tile_size, float caster_extrusion) {
    CascadeFit fit{};
    fit.split_near = split_near;
    fit.split_far = split_far;

    const float dir_len = light_dir.length();
    if (dir_len < 1e-6f) {
        // A zero-length light direction would produce NaNs that reach the
        // uniform buffer and blank every shadow in the frame; refuse instead.
        return fit;
    }
    if (tile_size == 0) tile_size = 1;
    if (!(split_far > split_near)) {
        split_far = split_near + 1e-3f;
        fit.split_far = split_far;
    }

    const Vec3 dir = light_dir / dir_len;

    Vec3 corners[8];
    slice_corners(camera, split_near, split_far, corners);

    Vec3 lo = corners[0];
    Vec3 hi = corners[0];
    Vec3 sum{0.0f, 0.0f, 0.0f};
    for (const Vec3& c : corners) {
        lo = lo.min(c);
        hi = hi.max(c);
        sum += c;
    }
    const Vec3 center = sum / 8.0f;
    // Slice radius: no corner is farther than this from the center, so backing
    // the light eye off by it (plus a unit of slack) guarantees every corner
    // lands strictly in front of the light's near plane.
    const float radius = (hi - lo).length() * 0.5f;

    const Vec3 light_eye = center - dir * (radius + 1.0f);
    const Mat4 light_view = Mat4::look_at(light_eye, center, light_up_for(dir));

    // Light-space AABB of the slice. Only x/y of the eye position affect the
    // basis, so backing the eye off along `dir` shifts z alone — which is what
    // makes it safe to pick the eye distance by radius as done above.
    Vec3 llo{1e30f, 1e30f, 1e30f};
    Vec3 lhi{-1e30f, -1e30f, -1e30f};
    for (const Vec3& c : corners) {
        const Vec3 l = light_view.transform_point(c);
        llo = llo.min(l);
        lhi = lhi.max(l);
    }

    // Casters standing between the light and the slice must still cast into it.
    // In light space the light looks down -Z, so "toward the light" is +z.
    float z_span = lhi.z - llo.z;
    if (z_span < 1e-4f) z_span = 1e-4f;
    const float extrude = std::clamp(caster_extrusion, 0.0f, 4.0f) * z_span;
    lhi.z += extrude;

    // --- Texel snapping -----------------------------------------------------
    // Both bounds are quantised to the texel grid, the minimum DOWN and the
    // maximum UP. Sub-texel camera translation then moves the projection in
    // discrete steps instead of sliding it continuously, which is what stops
    // shadow edges from shimmering as the camera walks.
    //
    // Snapping only the origin and rounding the *raw* extent up is the subtle
    // way to get this wrong: the origin can drop by almost a full texel while
    // the extent still describes the unshifted span, so the far edge lands
    // short and quietly clips a texel-wide strip off the cascade — visible as
    // a missing sliver of shadow, and only for camera positions that happen to
    // land there. Quantising each end on its own makes the span a whole number
    // of texels by construction, so containment and uniform density both hold.
    float extent_x = lhi.x - llo.x;
    float extent_y = lhi.y - llo.y;
    if (extent_x < 1e-4f) extent_x = 1e-4f;
    if (extent_y < 1e-4f) extent_y = 1e-4f;

    const float wupt = std::max(extent_x, extent_y) / static_cast<float>(tile_size);
    if (!(wupt > 1e-9f)) {
        return fit;
    }

    const float x0 = std::floor(llo.x / wupt) * wupt;
    const float y0 = std::floor(llo.y / wupt) * wupt;
    const float x1 = std::ceil(lhi.x / wupt) * wupt;
    const float y1 = std::ceil(lhi.y / wupt) * wupt;
    extent_x = x1 - x0;
    extent_y = y1 - y0;

    // Mat4::orthographic takes positive distances with -Z forward, so the
    // slice's largest light-space z (nearest the light) is the near plane.
    const float near_d = -lhi.z;
    const float far_d = -llo.z;
    if (!(far_d > near_d)) {
        return fit;
    }

    fit.light_view_proj =
        light_view * Mat4::orthographic(x0, x1, y0, y1, near_d, far_d);
    fit.depth_range = far_d - near_d;
    fit.world_extent = std::max(extent_x, extent_y);
    return fit;
}

CascadeTile cascade_tile(u32 index) {
    const u32 grid = kShadowTileGrid;
    const u32 i = index % (grid * grid);
    const float s = cascade_tile_scale();
    const u32 col = i % grid;
    const u32 row = i / grid;
    CascadeTile t{};
    t.u0 = static_cast<float>(col) * s;
    t.v0 = static_cast<float>(row) * s;
    t.u1 = t.u0 + s;
    t.v1 = t.v0 + s;
    return t;
}

u32 select_cascade(float view_depth, const float* splits, u32 count) {
    count = std::clamp(count, 1u, kMaxShadowCascades);
    if (splits == nullptr) return 0;
    // splits has count+1 boundaries; splits[0] is the near plane and the last
    // cascade is the fallback, so the loop stops one short of the end.
    for (u32 i = 0; i + 1 < count; ++i) {
        if (view_depth <= splits[i + 1]) return i;
    }
    return count - 1;
}

float cascade_auto_bias(const CascadeFit& fit, u32 tile_size, float slope) {
    if (tile_size == 0 || !(fit.depth_range > 1e-6f) || !(fit.world_extent > 1e-6f) ||
        !(slope > 0.0f)) {
        return 0.0f;
    }
    const float texel_world = fit.world_extent / static_cast<float>(tile_size);
    return (texel_world * slope) / fit.depth_range;
}

CascadeConfig sanitize_cascade_config(const CascadeConfig& config) {
    CascadeConfig c = config;
    c.count = std::clamp(c.count, 1u, kMaxShadowCascades);
    c.lambda = std::clamp(c.lambda, 0.0f, 1.0f);
    c.max_distance = std::max(c.max_distance, 0.0f);
    c.fade_range = std::clamp(c.fade_range, 0.0f, 1.0f);
    c.caster_extrusion = std::clamp(c.caster_extrusion, 0.0f, 4.0f);
    return c;
}

} // namespace nf::rendering
