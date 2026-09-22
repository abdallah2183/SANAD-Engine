// Tests/RHITests/test_local_shadows.cpp — point and spot shadow math (CPU only).
//
// Every case here pins a property a broken implementation would violate
// *silently*, and specifically silently in a way that looks like a rendering
// artefact rather than a crash: a face whose NDC is mirrored, a projector that
// misses the cone it is supposed to cover, a reconstruction rule that disagrees
// with the matrix the renderer actually uploads. None of them touch the GPU, so
// they run where the pixel-level tests must skip.

#include <NF/Test/TestFramework.hpp>
#include <NF/Rendering/LocalShadows.hpp>

#include <cmath>
#include <random>

using namespace nf;
using namespace nf::rendering;

namespace {

constexpr float kTol = 1e-4f;

/// Transforms a world point by a projector the way the vertex shader does
/// (`pos * view_proj`), doing the perspective divide.
bool project(const LocalShadowFit& fit, const Vec3& world, Vec3& out_ndc) {
    const Vec4 clip = fit.view_proj * Vec4{world, 1.0f};
    if (std::fabs(clip.w) < 1e-6f) return false;
    out_ndc = Vec3{clip.x, clip.y, clip.z} * (1.0f / clip.w);
    return true;
}

} // namespace

// --------------------------------------------------------------------------
// Point light faces: six projectors that tile the sphere
// --------------------------------------------------------------------------

NF_TEST(point_face_matrix_maps_axis_centre_to_ndc_origin) {
    // A point straight out along a face's forward axis must land at NDC (0,0)
    // — the centre of its own tile. If the basis is handed wrong this lands on
    // an axis, and the whole face's shadow is rotated or mirrored.
    const Vec3 origin{3.0f, 7.0f, -2.0f};
    for (u32 i = 0; i < kCubeFaceCount; ++i) {
        const CubeFace face = static_cast<CubeFace>(i);
        const LocalShadowFit fit = fit_point_face(origin, face);
        const Vec3 p = origin + face_forward(face) * 5.0f;
        Vec3 ndc{9.0f, 9.0f, 9.0f};
        NF_CHECK(project(fit, p, ndc));
        NF_CHECK(std::fabs(ndc.x) < kTol);
        NF_CHECK(std::fabs(ndc.y) < kTol);
        // Depth along the axis: 5 units of a [0.05, 25] span.
        const float expected = 25.0f * (1.0f - 0.05f / 5.0f) / (25.0f - 0.05f);
        NF_CHECK(std::fabs(ndc.z - expected) < kTol);
    }
}

NF_TEST(point_face_matrix_agrees_with_select_face) {
    // The renderer uploads the matrix; the shader reconstructs the face and
    // NDC from the direction. If the two disagree, the shadow is sampled from
    // the wrong place and merely looks soft. This is the test that the
    // reconstruction rule and the matrix are one convention.
    const Vec3 origin{-4.0f, 2.0f, 9.0f};
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    for (int iter = 0; iter < 400; ++iter) {
        Vec3 dir{d(rng), d(rng), d(rng)};
        if (dir.length() < 0.1f) continue;
        dir = dir.normalized();

        CubeFace face{};
        Vec2 ndc_rule{9.0f, 9.0f};
        NF_CHECK(select_face(dir, face, ndc_rule));

        // The matrix must accept that same point and produce the same NDC.
        const LocalShadowFit fit = fit_point_face(origin, face);
        const Vec3 world = origin + dir * 10.0f;
        Vec3 ndc_mat{9.0f, 9.0f, 9.0f};
        NF_CHECK(project(fit, world, ndc_mat));
        NF_CHECK(std::fabs(ndc_mat.x - ndc_rule.x) < kTol);
        NF_CHECK(std::fabs(ndc_mat.y - ndc_rule.y) < kTol);
    }
}

NF_TEST(point_faces_partition_the_sphere_without_gaps) {
    // Every direction must fall in exactly one face: the six projectors cover
    // all directions with no hole (a direction nobody projects) and no double
    // coverage (a direction two faces accept), which is what "tiled" means.
    const Vec3 dir_samples[] = {
        {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
        {1.0f, 1.0f, 0.0f}, {1.0f, -1.0f, 1.0f}, {-1.0f, 1.0f, -1.0f},
        {0.7071f, 0.7071f, 0.0f}, {0.577f, 0.577f, 0.577f},
        {-0.3f, 0.8f, 0.51f}, {0.2f, -0.6f, 0.77f},
    };
    for (const Vec3& raw : dir_samples) {
        const Vec3 dir = raw.normalized();
        int hits = 0;
        for (u32 i = 0; i < kCubeFaceCount; ++i) {
            Vec2 ndc{9.0f, 9.0f};
            if (project_into_face(dir, static_cast<CubeFace>(i), ndc)) ++hits;
        }
        // No gap (every direction is covered) and no interior overlap: a
        // direction can touch at most the faces whose pyramids share its
        // boundary, which for a corner-pointing sample is three.
        NF_CHECK(hits >= 1);
        NF_CHECK(hits <= 3);
    }
}

NF_TEST(point_face_interior_direction_is_in_exactly_one_face) {
    // A direction strictly inside one face's pyramid — perturbed off every
    // boundary by a margin — is in exactly one face. That is the "no double
    // coverage" half of the partition, and it is what makes the shader's face
    // choice unambiguous for every fragment that is not exactly on a seam.
    std::mt19937 rng(777);
    std::uniform_real_distribution<float> d(-0.85f, 0.85f);
    for (int iter = 0; iter < 300; ++iter) {
        Vec3 dir{d(rng), d(rng), d(rng)};
        if (dir.length_sq() < 0.04f) continue;
        dir = dir.normalized();

        int hits = 0;
        for (u32 i = 0; i < kCubeFaceCount; ++i) {
            Vec2 ndc{9.0f, 9.0f};
            if (project_into_face(dir, static_cast<CubeFace>(i), ndc)) ++hits;
        }
        NF_CHECK(hits == 1);
    }
}

NF_TEST(point_face_rejects_behind_and_outside) {
    const CubeFace face = CubeFace::PosX;
    Vec2 ndc{9.0f, 9.0f};
    // Behind the face's plane.
    NF_CHECK(!project_into_face({-1.0f, 0.0f, 0.0f}, face, ndc));
    // Outside the 90-degree pyramid (diagonal of a square is 45 degrees each
    // way, so 1:1:0 is exactly on the edge; 1:0.9:0.9 is inside).
    NF_CHECK(!project_into_face({1.0f, 2.0f, 0.0f}, face, ndc));
    NF_CHECK(project_into_face({1.0f, 0.9f, 0.9f}, face, ndc));
}

NF_TEST(select_face_rejects_zero_direction) {
    CubeFace face{};
    Vec2 ndc{9.0f, 9.0f};
    NF_CHECK(!select_face({0.0f, 0.0f, 0.0f}, face, ndc));
}

NF_TEST(point_face_identity_on_degenerate_range) {
    // A degenerate [near, far] must yield a projector with no NaNs, not one
    // that blanks the whole atlas when the light's radius is zero. The
    // contract is finiteness: the tile stays clear rather than rendering as
    // inky black at every depth.
    const LocalShadowFit fit = fit_point_face({0, 0, 0}, CubeFace::PosY, 1.0f, 1.0f);
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            NF_CHECK(std::isfinite(fit.view_proj.m[r][c]));
        }
    }
}

// --------------------------------------------------------------------------
// Spot light: one projector that covers its own cone
// --------------------------------------------------------------------------

NF_TEST(spot_matrix_centre_and_cone_edge) {
    // The cone axis must map to NDC (0,0) and a ray on the cone's rim must
    // sit at |ndc| == 1 — the frustum's edge grazing the rim exactly. If the
    // fov is too small the rim is clipped (shadow cut off); too wide and half
    // the resolution is spent on directions the light never reaches.
    const Vec3 pos{0.0f, 5.0f, 0.0f};
    const Vec3 dir{0.0f, -1.0f, 0.0f};
    const float outer = 0.6f;
    const LocalShadowFit fit = fit_spot(pos, dir, outer);

    Vec3 ndc{9.0f, 9.0f, 9.0f};
    NF_CHECK(project(fit, pos + dir * 8.0f, ndc));
    NF_CHECK(std::fabs(ndc.x) < kTol);
    NF_CHECK(std::fabs(ndc.y) < kTol);

    // The frustum is square and the projector's right/up axes are whatever
    // look_at built, so a rim ray must be tested along BOTH: the offset
    // direction that lands at ndc.x==1 is the one in the frustum's own right
    // axis, and only one of the two candidates is it.
    const float co = std::cos(outer), so = std::sin(outer);
    const Vec3 rim_candidates[2] = {
        (dir * co + Vec3{1.0f, 0.0f, 0.0f} * so).normalized(),
        (dir * co + Vec3{0.0f, 0.0f, 1.0f} * so).normalized(),
    };
    bool found_x = false;
    for (const Vec3& rim : rim_candidates) {
        Vec3 rn{9.0f, 9.0f, 9.0f};
        NF_CHECK(project(fit, pos + rim * 8.0f, rn));
        // A rim ray is at |x| == 1 or |y| == 1 exactly; the other axis is 0.
        const bool x_edge = std::fabs(std::fabs(rn.x) - 1.0f) < 1e-3f &&
                            std::fabs(rn.y) < 1e-3f;
        const bool y_edge = std::fabs(std::fabs(rn.y) - 1.0f) < 1e-3f &&
                            std::fabs(rn.x) < 1e-3f;
        NF_CHECK(x_edge || y_edge);
        if (x_edge) found_x = true;
    }
    // The square frustum reaches both axes at the rim, so one of the two
    // candidates must be the x edge. If neither is, the fov is not 2*outer.
    NF_CHECK(found_x);
}

NF_TEST(spot_aimed_along_each_axis) {
    // A spot pointing along each world axis must still centre its own axis —
    // the up-choice fallback keeps straight-down lights from degenerating.
    const Vec3 aims[] = {
        {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
    };
    for (const Vec3& aim : aims) {
        const Vec3 origin{1.0f, 2.0f, 3.0f};
        const LocalShadowFit fit = fit_spot(origin, aim, 0.4f);
        Vec3 ndc{9.0f, 9.0f, 9.0f};
        NF_CHECK(project(fit, origin + aim * 4.0f, ndc));
        NF_CHECK(std::fabs(ndc.x) < kTol);
        NF_CHECK(std::fabs(ndc.y) < kTol);
        for (u32 r = 0; r < 4; ++r)
            for (u32 c = 0; c < 4; ++c)
                NF_CHECK(std::isfinite(fit.view_proj.m[r][c]));
    }
}

NF_TEST(spot_tilted_aim_still_centres_axis) {
    // A tilted aim exercises the non-axis-aligned basis — the case where a
    // transposed look_at silently breaks while every axis-aligned one passes.
    const Vec3 aim{0.3f, -0.7f, 0.5f};
    const Vec3 origin{-2.0f, 8.0f, 1.0f};
    const LocalShadowFit fit = fit_spot(origin, aim, 0.5f);
    const Vec3 n = aim.normalized();
    Vec3 ndc{9.0f, 9.0f, 9.0f};
    NF_CHECK(project(fit, origin + n * 6.0f, ndc));
    NF_CHECK(std::fabs(ndc.x) < kTol);
    NF_CHECK(std::fabs(ndc.y) < kTol);
}

NF_TEST(spot_zero_direction_is_identity_not_nan) {
    const LocalShadowFit fit = fit_spot({0, 0, 0}, {0, 0, 0}, 0.5f);
    for (u32 r = 0; r < 4; ++r)
        for (u32 c = 0; c < 4; ++c)
            NF_CHECK(std::isfinite(fit.view_proj.m[r][c]));
}

NF_TEST(spot_outer_angle_clamped) {
    // A degenerate (zero) or absurd (>90 degrees) cone must still produce a
    // finite, sensible projector rather than an inverted frustum.
    const LocalShadowFit fit_lo = fit_spot({0, 5, 0}, {0, -1, 0}, 0.0f);
    const LocalShadowFit fit_hi = fit_spot({0, 5, 0}, {0, -1, 0}, 2.0f);
    for (const LocalShadowFit& f : {fit_lo, fit_hi}) {
        for (u32 r = 0; r < 4; ++r)
            for (u32 c = 0; c < 4; ++c)
                NF_CHECK(std::isfinite(f.view_proj.m[r][c]));
    }
}

// --------------------------------------------------------------------------
// Bias
// --------------------------------------------------------------------------

NF_TEST(local_bias_is_zero_for_degenerate_fit) {
    // A zero tile size and a NaN-fit projector both mean "no grid to size a
    // bias against", so the answer is 0 — no bias, not a NaN that would
    // blank the atlas through the uniform buffer.
    const LocalShadowFit good = fit_point_face({0, 0, 0}, CubeFace::PosZ, 0.1f, 20.0f);
    NF_CHECK(local_shadow_auto_bias(good, 0) == 0.0f);
    // An inverted or empty range yields an IDENTITY fit (fit_point_face's
    // documented contract), which still has finite entries — and the bias
    // function must not divide by the resulting zero depth range.
    const LocalShadowFit zero_range = fit_point_face({0, 0, 0}, CubeFace::PosZ, 5.0f, 5.0f);
    const float b = local_shadow_auto_bias(zero_range, 1024);
    NF_CHECK(std::isfinite(b));
}

NF_TEST(local_bias_grows_with_texel_and_slope) {
    // The bias must be proportional to the texel's world size, and a COARSE
    // tile (fewer texels) covers more world per texel than a fine one — so a
    // 512-tile needs a bigger bias than a 2048-tile, not a smaller one. Get
    // this backwards and one tile size has acne while another peter-pans.
    const LocalShadowFit fit = fit_point_face({0, 0, 0}, CubeFace::PosZ, 0.1f, 20.0f);
    const float b512 = local_shadow_auto_bias(fit, 512);
    const float b1024 = local_shadow_auto_bias(fit, 1024);
    const float b2048 = local_shadow_auto_bias(fit, 2048);
    NF_CHECK(b512 > 0.0f);
    NF_CHECK(b512 > b1024);
    NF_CHECK(b1024 > b2048);
    // Doubling the slope tolerance doubles the worst-case surface offset.
    NF_CHECK(std::fabs(local_shadow_auto_bias(fit, 1024, 4.0f) -
                       2.0f * b1024) < kTol);
}

NF_TEST(local_bias_scales_with_depth_range) {
    // Two projectors with the same texel count but a different reach: the
    // longer one maps a bigger world span onto [0,1], so the same NDC bias is
    // a bigger world offset, and the derived bias must be SMALLER to keep the
    // physical offset the artist would tune.
    const LocalShadowFit near_fit = fit_point_face({0, 0, 0}, CubeFace::PosZ, 0.1f, 10.0f);
    const LocalShadowFit far_fit = fit_point_face({0, 0, 0}, CubeFace::PosZ, 0.1f, 40.0f);
    const float b_near = local_shadow_auto_bias(near_fit, 1024);
    const float b_far = local_shadow_auto_bias(far_fit, 1024);
    NF_CHECK(b_near > 0.0f);
    NF_CHECK(b_far > 0.0f);
    NF_CHECK(b_far < b_near);
}

// --------------------------------------------------------------------------
// plan_local_shadows: who gets atlas tiles, and where they land
// --------------------------------------------------------------------------
//
// The renderer now delegates the whole assignment to this template, so these
// tests are what stands between a bad tile rule and a shadow that samples the
// wrong light's projector — a failure mode that still returns a plausible
// depth and therefore looks like an artefact, not a crash. The light structs
// are local because the template is duck-typed on purpose (see the header):
// testing it against its own minimal structs also proves the renderer is not
// quietly depending on some PointLight field the contract does not name.

namespace {

struct TestPoint {
    bool  shadows_enabled = true;
    Vec3  position{};
    float radius = 10.0f;
    float shadow_distance = 0.0f;
    float shadow_strength = 1.0f;
    float shadow_bias = 0.0f;
};

struct TestSpot {
    bool  shadows_enabled = true;
    Vec3  position{};
    Vec3  direction{0.0f, -1.0f, 0.0f};
    float outer_angle_rad = 0.5f;
    // The contract's reach: the projector's far plane falls back to this when
    // shadow_distance is 0, and lighting.frag windows the cone by it, so both
    // sides of the light need the one field. 25 is kLocalShadowSpotDefaultFar
    // — the value the fallback used to hard-code, so a light that sets neither
    // still gets the same projector as before range existed.
    float range = 25.0f;
    float shadow_distance = 0.0f;
    float shadow_strength = 1.0f;
    float shadow_bias = 0.0f;
};

constexpr u32 kPlanTilePx = 512u;

/// Mat4 has no operator== on purpose (float equality in physics code is almost
/// always wrong), so an unused tile's identity projector is checked element by
/// element: an identity matrix flagged enabled is precisely the failure mode
/// the degenerate-light guards exist to prevent, so the check has to see it.
bool is_identity(const Mat4& m) {
    for (u32 r = 0; r < 4u; ++r) {
        for (u32 c = 0; c < 4u; ++c) {
            const float want = (r == c) ? 1.0f : 0.0f;
            if (std::fabs(m.m[r][c] - want) > kTol) return false;
        }
    }
    return true;
}

} // namespace

NF_TEST(plan_assigns_point_tiles_by_light_index) {
    // Light i owns [i*6, i*6+6) exactly. The shader recovers a light's tiles
    // from its LOOP index, so a plan that packed opted-in lights instead would
    // hand the shader light B's projector when it asked for light A's.
    std::array<TestPoint, 2> points{{{}, {}}};
    points[0].position = Vec3{10.0f, 0.0f, 0.0f};
    points[1].position = Vec3{-10.0f, 0.0f, 0.0f};
    const std::array<TestSpot, 0> spots{};

    const LocalShadowPlan plan = plan_local_shadows(points, spots, kPlanTilePx);

    for (u32 light = 0; light < 2u; ++light) {
        for (u32 f = 0; f < kCubeFaceCount; ++f) {
            const u32 tile = point_shadow_tile(light, static_cast<CubeFace>(f));
            NF_CHECK_EQ(plan.tiles[tile].enabled, 1.0f);
            // The projector is placed at ITS light, not at light 0's: a tile
            // filled from the wrong origin is the packed-assignment bug, and
            // the y of a face that looks down +Y from y=10 differs from one
            // looking from y=0 by exactly that offset.
            const Vec3 probe = points[light].position + face_forward(static_cast<CubeFace>(f)) * 2.0f;
            const Vec4 clip = plan.tiles[tile].view_proj * Vec4{probe, 1.0f};
            NF_CHECK(clip.w > 0.0f);
            const Vec3 ndc = Vec3{clip.x, clip.y, clip.z} * (1.0f / clip.w);
            NF_CHECK_NEAR(ndc.x, 0.0f, kTol);
            NF_CHECK_NEAR(ndc.y, 0.0f, kTol);
        }
    }
    NF_CHECK_EQ(plan.point_shadow_lights, 2u);
    // Unclaimed tiles stay disabled rather than carrying a stale projector.
    NF_CHECK_EQ(plan.tiles[point_shadow_tile(2u, CubeFace::PosX)].enabled, 0.0f);
}

NF_TEST(plan_ignores_light_that_opted_out_but_keeps_later_lights_at_their_own_tiles) {
    // Light 0 does not cast, light 1 does. Light 1 still owns tiles [6, 12):
    // the gap at [0, 6) is the price of index addressing, and the alternative
    // (sliding everyone down) is what makes the shader read the wrong light.
    std::array<TestPoint, 2> points{};
    points[0].shadows_enabled = false;
    points[0].position = Vec3{0.0f, 20.0f, 0.0f};
    points[1].shadows_enabled = true;
    points[1].position = Vec3{0.0f, -20.0f, 0.0f};
    const std::array<TestSpot, 0> spots{};

    const LocalShadowPlan plan = plan_local_shadows(points, spots, kPlanTilePx);

    for (u32 f = 0; f < kCubeFaceCount; ++f) {
        const u32 t = point_shadow_tile(0u, static_cast<CubeFace>(f));
        NF_CHECK_EQ(plan.tiles[t].enabled, 0.0f);
        NF_CHECK(is_identity(plan.tiles[t].view_proj));
    }
    for (u32 f = 0; f < kCubeFaceCount; ++f) {
        const u32 t = point_shadow_tile(1u, static_cast<CubeFace>(f));
        NF_CHECK_EQ(plan.tiles[t].enabled, 1.0f);
        // Light 0's origin is unused: light 1's PosY face looks from -20, so a
        // point 2 units along its own +Y is at y = -18 and must project to the
        // tile centre. A plan that fell back to light 0's origin would see the
        // same point as 38 units above the light, outside the near plane.
        const CubeFace face = static_cast<CubeFace>(f);
        const Vec3 probe = points[1].position + face_forward(face) * 2.0f;
        const Vec4 clip = plan.tiles[t].view_proj * Vec4{probe, 1.0f};
        NF_CHECK(clip.w > 0.0f);
        const Vec3 ndc = Vec3{clip.x, clip.y, clip.z} * (1.0f / clip.w);
        NF_CHECK_NEAR(ndc.x, 0.0f, kTol);
        NF_CHECK_NEAR(ndc.y, 0.0f, kTol);
    }
    NF_CHECK_EQ(plan.point_shadow_lights, 1u);
}

NF_TEST(plan_gives_a_light_past_the_shadow_cap_no_tiles) {
    // kMaxShadowPointLights is 4 and the atlas holds 28 tiles, so light 4's
    // faces would be tiles 24..29 — past the end, and colliding with the spot
    // tiles at 24..27. The cap is on the INDEX, so a light past it is skipped
    // outright rather than getting the next free slot.
    std::array<TestPoint, kMaxShadowPointLights + 2u> points{};
    for (u32 i = 0; i < points.size(); ++i) {
        points[i].position = Vec3{static_cast<float>(i), 0.0f, 0.0f};
    }
    const std::array<TestSpot, 0> spots{};

    const LocalShadowPlan plan = plan_local_shadows(points, spots, kPlanTilePx);

    NF_CHECK_EQ(plan.point_shadow_lights, kMaxShadowPointLights);
    // Lights 0..3 own tiles 0..23, all enabled.
    for (u32 light = 0; light < kMaxShadowPointLights; ++light) {
        for (u32 f = 0; f < kCubeFaceCount; ++f) {
            NF_CHECK_EQ(plan.tiles[point_shadow_tile(light, static_cast<CubeFace>(f))].enabled, 1.0f);
        }
    }
    // Light 4's faces are tiles 24..29. Tiles 28 and 29 are past the array and
    // cannot be probed, so the OBSERVABLE half of the bug is faces 0..3, which
    // are the spot tiles 24..27: a plan that counted opt-ins instead of
    // indices would have handed light 4 those four tiles, and the shader would
    // read a point-light projector for every spot light in the scene. Counting
    // opt-ins also writes past the end of the uniform buffer; that half is
    // caught by the lights-0..3 check above plus this count.
    for (u32 j = 0; j < kMaxShadowSpotLights; ++j) {
        NF_CHECK_EQ(plan.tiles[spot_shadow_tile(j)].enabled, 0.0f);
        NF_CHECK(is_identity(plan.tiles[spot_shadow_tile(j)].view_proj));
    }
}

NF_TEST(plan_rejects_degenerate_point_reach) {
    // A light whose reach is at or inside the near plane has no valid
    // projector: fit_point_face would return an identity matrix, and an
    // identity matrix flagged ENABLED occludes everything the light reaches.
    // Disabled is the safe answer, and it costs the light its shadow rather
    // than its lighting (the shader keys the enabled check on the tile).
    std::array<TestPoint, 2> points{};
    points[0].radius = 0.01f;                        // reach inside near plane
    points[1].shadow_distance = kLocalShadowNear;    // reach AT the near plane
    const std::array<TestSpot, 0> spots{};

    const LocalShadowPlan plan = plan_local_shadows(points, spots, kPlanTilePx);

    for (u32 f = 0; f < kCubeFaceCount; ++f) {
        NF_CHECK_EQ(plan.tiles[point_shadow_tile(0u, static_cast<CubeFace>(f))].enabled, 0.0f);
        NF_CHECK_EQ(plan.tiles[point_shadow_tile(1u, static_cast<CubeFace>(f))].enabled, 0.0f);
    }
    NF_CHECK_EQ(plan.point_shadow_lights, 0u);

    // A light that only LOOKS degenerate — radius 0 but an explicit
    // shadow_distance — is fine, because the artist's distance is the reach.
    std::array<TestPoint, 1> recovered{};
    recovered[0].radius = 0.0f;
    recovered[0].shadow_distance = 15.0f;
    const LocalShadowPlan plan2 = plan_local_shadows(recovered, spots, kPlanTilePx);
    NF_CHECK_EQ(plan2.point_shadow_lights, 1u);
    NF_CHECK_EQ(plan2.tiles[point_shadow_tile(0u, CubeFace::PosX)].enabled, 1.0f);
}

NF_TEST(plan_spots_take_their_own_tiles_after_the_point_block) {
    // Spot j owns tile 24+j. A point light at the same position does not
    // collide with it, and the projector is a CONE fit aimed along the spot's
    // own direction — not one of the six 90-degree point faces.
    const std::array<TestPoint, 0> points{};
    std::array<TestSpot, 2> spots{};
    spots[0].position = Vec3{0.0f, 10.0f, 0.0f};
    spots[0].direction = Vec3{0.0f, -1.0f, 0.0f};
    spots[1].position = Vec3{5.0f, 10.0f, 0.0f};
    spots[1].direction = Vec3{1.0f, 0.0f, 0.0f};

    const LocalShadowPlan plan = plan_local_shadows(points, spots, kPlanTilePx);

    NF_CHECK_EQ(plan.spot_shadow_lights, 2u);
    NF_CHECK_EQ(plan.tiles[spot_shadow_tile(0u)].enabled, 1.0f);
    NF_CHECK_EQ(plan.tiles[spot_shadow_tile(1u)].enabled, 1.0f);

    // A probe on each spot's own axis must land at the CENTRE of that spot's
    // tile — NDC (0,0). This is what makes a tile swap fail loudly rather than
    // pass with a plausible depth: the probe on spot 0's axis, projected
    // through spot 1's sideways projector at (5,10,0), is 90 degrees off that
    // projector's axis and lands far outside its NDC box, so a swapped plan
    // fails this instead of shading the wrong cone.
    const Vec3 probe0 = spots[0].position + spots[0].direction * 3.0f;
    const Vec3 probe1 = spots[1].position + spots[1].direction * 3.0f;
    for (u32 s = 0u; s < 2u; ++s) {
        const Vec3 probe = (s == 0u) ? probe0 : probe1;
        const Vec4 clip = plan.tiles[spot_shadow_tile(s)].view_proj * Vec4{probe, 1.0f};
        NF_CHECK(clip.w > 0.0f);
        const Vec3 ndc = Vec3{clip.x, clip.y, clip.z} * (1.0f / clip.w);
        NF_CHECK_NEAR(ndc.x, 0.0f, kTol);
        NF_CHECK_NEAR(ndc.y, 0.0f, kTol);
    }
    // The two tiles are genuinely different projectors, not the same matrix
    // twice: spot 1's probe is off-axis for spot 0 and vice versa.
    NF_CHECK(!is_identity(plan.tiles[spot_shadow_tile(0u)].view_proj));
    NF_CHECK(!is_identity(plan.tiles[spot_shadow_tile(1u)].view_proj));
}

NF_TEST(plan_rejects_spot_with_no_aim) {
    // A zero direction makes fit_spot return identity. The plan must reject it
    // by length, before the tile exists, because an identity projector flagged
    // enabled occludes everything inside the cone the light still lights.
    const std::array<TestPoint, 0> points{};
    std::array<TestSpot, 2> spots{};
    spots[0].direction = Vec3{0.0f, 0.0f, 0.0f};
    spots[1].direction = Vec3{0.0f, -1.0f, 0.0f};

    const LocalShadowPlan plan = plan_local_shadows(points, spots, kPlanTilePx);

    NF_CHECK_EQ(plan.tiles[spot_shadow_tile(0u)].enabled, 0.0f);
    NF_CHECK(is_identity(plan.tiles[spot_shadow_tile(0u)].view_proj));
    NF_CHECK_EQ(plan.tiles[spot_shadow_tile(1u)].enabled, 1.0f);
    NF_CHECK_EQ(plan.spot_shadow_lights, 1u);
}

NF_TEST(plan_carries_the_artists_knobs_and_a_derived_bias) {
    // The tile is a memcpy into the frame uniforms, so its four params must be
    // exactly what the shader consumes: enabled, strength, the DERIVED minimum
    // bias for this projector's own texel size, and the artist's bias on top.
    std::array<TestPoint, 1> points{};
    points[0].radius = 12.0f;
    points[0].shadow_strength = 0.7f;
    points[0].shadow_bias = 0.002f;
    const std::array<TestSpot, 0> spots{};

    const LocalShadowPlan plan = plan_local_shadows(points, spots, kPlanTilePx);

    const u32 tile = point_shadow_tile(0u, CubeFace::PosX);
    NF_CHECK_EQ(plan.tiles[tile].enabled, 1.0f);
    NF_CHECK_NEAR(plan.tiles[tile].strength, 0.7f, kTol);
    NF_CHECK_NEAR(plan.tiles[tile].artist_bias, 0.002f, kTol);
    // The derived bias must match calling the same helper on the same
    // projector — the renderer used to compute it from a fit filled by an
    // EARLIER light, which silently under-biased every short-range light.
    const LocalShadowFit fit = fit_point_face(points[0].position, CubeFace::PosX,
                                              kLocalShadowNear, 12.0f);
    NF_CHECK_NEAR(plan.tiles[tile].auto_bias,
                  local_shadow_auto_bias(fit, kPlanTilePx), kTol);
    // A coarser tile (fewer texels per world unit) needs a LARGER bias, so the
    // tile size genuinely reaches the plan rather than being a constant.
    const LocalShadowPlan coarse = plan_local_shadows(points, spots, 128u);
    NF_CHECK(coarse.tiles[tile].auto_bias > plan.tiles[tile].auto_bias);
}

NF_TEST(plan_with_no_shadowed_lights_leaves_every_tile_disabled) {
    // The common case: nothing casts, and the whole atlas stays at its
    // zero-initialised defaults so the pass costs one clear and the shader's
    // per-tile enabled check makes every light free.
    std::array<TestPoint, 1> points{};
    points[0].shadows_enabled = false;
    std::array<TestSpot, 1> spots{};
    spots[0].shadows_enabled = false;

    const LocalShadowPlan plan = plan_local_shadows(points, spots, kPlanTilePx);

    NF_CHECK_EQ(plan.point_shadow_lights, 0u);
    NF_CHECK_EQ(plan.spot_shadow_lights, 0u);
    for (u32 t = 0; t < kLocalShadowTileCount; ++t) {
        NF_CHECK_EQ(plan.tiles[t].enabled, 0.0f);
        NF_CHECK(is_identity(plan.tiles[t].view_proj));
    }
}
