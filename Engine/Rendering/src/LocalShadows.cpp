#include <NF/Rendering/LocalShadows.hpp>

#include <algorithm>
#include <cmath>

namespace nf::rendering {
namespace {

/// The direction each face's projector looks along, and the `up` that keeps
/// its basis non-degenerate. Every face uses an `up` that is never parallel to
/// its own axis, so no face can produce a zero cross product while another is
/// healthy.
struct FaceBasis {
    Vec3 forward;
    Vec3 up;
};

constexpr FaceBasis face_basis(CubeFace face) {
    switch (face) {
        // The Y faces look along the world's steepest axis, so +Y is parallel
        // to their own forward and cannot serve as up. +Z is always
        // perpendicular to both Y faces, and both X/Z faces are safe with +Y.
        case CubeFace::PosX: return {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
        case CubeFace::NegX: return {{-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
        case CubeFace::PosY: return {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        case CubeFace::NegY: return {{0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        case CubeFace::PosZ: return {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}};
        case CubeFace::NegZ: return {{0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}};
    }
    return {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}};
}

/// The two axes a face's NDC xy lies in. For a projector looking along `f` with
/// up `u`, the shader's ndc.x measures displacement along the right-hand axis
/// and ndc.y along up — both divided by the distance along `f` (the perspective
/// divide). Parallel to `f`, so `f.cross(up)` is the right-hand axis.
struct FacePlane {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
};

constexpr FacePlane face_plane(CubeFace face) {
    const FaceBasis b = face_basis(face);
    return {b.forward.cross(b.up), b.up, b.forward};
}

/// Half-angle a spot light's square frustum must open to cover a cone of
/// `outer_angle_rad`. The cone's rim is the frustum's edge ray, so the frustum
/// fov is twice the half-angle, clamped inside a hemisphere.
float spot_fov(float outer_angle_rad) {
    constexpr float kMaxHalfAngle = 1.55334f; // just under 89 degrees
    const float half = std::clamp(outer_angle_rad, 1e-4f, kMaxHalfAngle);
    return half * 2.0f;
}

} // namespace

Vec3 face_forward(CubeFace face) {
    return face_basis(face).forward;
}

LocalShadowFit fit_point_face(const Vec3& position, CubeFace face,
                              float near_z, float far_z) {
    LocalShadowFit fit;
    if (!(far_z > near_z) || near_z <= 0.0f || far_z <= 0.0f) {
        return fit; // identity projector, leaves the tile clear instead of NaN
    }
    const FaceBasis b = face_basis(face);
    // A 90-degree square projector: the six faces tile the full sphere
    // exactly, with no overlap and no gap, when each opens a right angle.
    constexpr float kHalfCubeAngle = 1.5707963267948966f; // pi/2
    // Row-vector application composes world -> view -> clip as
    // view * projection (see update_camera); the reverse order applies the
    // projection to world space and lands every point outside the frustum.
    fit.view_proj =
        Mat4::look_at(position, position + b.forward, b.up) *
        Mat4::perspective(kHalfCubeAngle, 1.0f, near_z, far_z);
    fit.near_z = near_z;
    fit.far_z = far_z;
    return fit;
}

LocalShadowFit fit_spot(const Vec3& position, const Vec3& direction,
                        float outer_angle_rad, float near_z, float far_z) {
    LocalShadowFit fit;
    if (!(far_z > near_z) || near_z <= 0.0f || far_z <= 0.0f) return fit;

    const float length = direction.length();
    if (length <= 1e-6f) return fit; // no aim: identity, not a NaN

    // Aim up the same way look_at does, so a spotlight pointing straight down
    // does not degenerate. This mirrors ShadowCascades' light_up_for.
    const Vec3 dir = direction * (1.0f / length);
    const Vec3 up = (std::fabs(dir.y) > 0.98f) ? Vec3{1.0f, 0.0f, 0.0f}
                                               : Vec3{0.0f, 1.0f, 0.0f};

    fit.view_proj =
        Mat4::look_at(position, position + dir, up) *
        Mat4::perspective(spot_fov(outer_angle_rad), 1.0f, near_z, far_z);
    fit.near_z = near_z;
    fit.far_z = far_z;
    return fit;
}

bool project_into_face(const Vec3& dir, CubeFace face, Vec2& out_ndc) {
    const FacePlane p = face_plane(face);
    const float depth = dir.dot(p.forward);
    if (depth <= 1e-6f) return false; // behind this face: it cannot occlude

    // ndc = (offset / depth) at 90 degrees, where perspective divides the
    // screen-space offset by the depth along the view axis. |ndc| <= 1 is
    // "inside the face's pyramid" exactly.
    const float x = dir.dot(p.right) / depth;
    float y = dir.dot(p.up) / depth;

    // The engine's perspective negates m[1][1] for Vulkan (NDC y = -1 at the
    // TOP of the framebuffer), so a matrix-projected point has its NDC y
    // sign-flipped relative to the raw offset/up ratio. The renderer uploads
    // the matrix and the shader reconstructs the face index and UV from the
    // direction; if this rule does not mirror the flip, the two disagree and
    // every point-light shadow is sampled from a mirrored tile — visibly
    // wrong, silently, since a depth value is still returned.
    y = -y;

    if (x < -1.0f || x > 1.0f || y < -1.0f || y > 1.0f) return false;

    out_ndc = Vec2{x, y};
    return true;
}

bool select_face(const Vec3& dir, CubeFace& out_face, Vec2& out_ndc) {
    const Vec3 a = dir.abs();
    if (a.max_component() <= 1e-6f) return false;

    // The face whose axis the direction is most aligned with is the one whose
    // pyramid contains it: at 90 degrees per face, the dominant axis is the
    // partition of the sphere. Checked in atlas order so a tie (exactly on a
    // face boundary) picks the lower index deterministically rather than by
    // float noise.
    const CubeFace candidates[6] = {
        CubeFace::PosX, CubeFace::NegX, CubeFace::PosY,
        CubeFace::NegY, CubeFace::PosZ, CubeFace::NegZ,
    };
    CubeFace best = candidates[0];
    float best_dot = -2.0f;
    for (const CubeFace c : candidates) {
        const float d = dir.dot(face_forward(c));
        if (d > best_dot) {
            best_dot = d;
            best = c;
        }
    }

    // A point on the boundary between two faces reports a dot below the
    // pyramid's edge for the selected one; project_into_face catches that
    // instead of silently sampling through the seam.
    if (best_dot <= 0.0f) return false;
    Vec2 ndc{0.0f, 0.0f};
    if (!project_into_face(dir, best, ndc)) return false;

    out_face = best;
    out_ndc = ndc;
    return true;
}

float local_shadow_auto_bias(const LocalShadowFit& fit, u32 tile_size,
                             float slope) {
    if (tile_size == 0 || !(fit.far_z > fit.near_z) || fit.near_z <= 0.0f) {
        return 0.0f;
    }
    // One texel covers `far / tile` world units at the far plane, which is the
    // coarsest the projector gets; nearer texels are finer, so this sizes the
    // bias for the worst case rather than under-biasing the far field.
    const float texel_world = fit.far_z / static_cast<float>(tile_size);
    const float world_bias = texel_world * slope;

    // Convert the world offset along the view axis into the NDC units the
    // shader compares in: the projection maps the [near, far] span onto
    // [0, 1], so an NDC unit is `(far - near)` world units.
    return world_bias / (fit.far_z - fit.near_z);
}

} // namespace nf::rendering
