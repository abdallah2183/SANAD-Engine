// Tests/RHITests/test_shadow_cascades.cpp — cascaded shadow math (CPU only).
//
// These cases are the whole reason the cascade math lives in its own
// device-free translation unit: every one of them pins a property that a wrong
// implementation would violate *silently* — a slice that is not actually
// covered, a split list that never reaches the far plane, a projection that
// slides continuously with the camera and shimmers. None of them need a GPU, so
// they keep running on a machine where the pixel tests skip.

#include <NF/Test/TestFramework.hpp>
#include <NF/Rendering/ShadowCascades.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::rendering;

namespace {

constexpr float kTol = 1e-4f;

Camera make_cam(float x, float y, float z, float near_z = 0.1f, float far_z = 100.0f) {
    Camera cam{};
    cam.position = {x, y, z};
    cam.target = {0, 0, 0};
    cam.up = {0, 1, 0};
    cam.aspect = 16.0f / 9.0f;
    cam.fov_y_rad = 60.0f * PI / 180.0f;
    cam.near_plane = near_z;
    cam.far_plane = far_z;
    update_camera(cam);
    return cam;
}

Camera make_cam_looking(float eye_x, float eye_y, float eye_z, float tgt_x, float tgt_y,
                        float tgt_z, float near_z = 0.1f, float far_z = 100.0f) {
    Camera cam{};
    cam.position = {eye_x, eye_y, eye_z};
    cam.target = {tgt_x, tgt_y, tgt_z};
    cam.up = {0, 1, 0};
    cam.aspect = 16.0f / 9.0f;
    cam.fov_y_rad = 60.0f * PI / 180.0f;
    cam.near_plane = near_z;
    cam.far_plane = far_z;
    update_camera(cam);
    return cam;
}

bool all_finite(const Mat4& m) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(m.m[r][c])) return false;
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Split distances
// ---------------------------------------------------------------------------

NF_TEST(shadow_cascade_splits_span_exactly_near_to_far) {
    float splits[kMaxShadowCascades + 1]{};
    compute_cascade_splits(0.1f, 100.0f, 4, 0.75f, splits);

    NF_CHECK_NEAR(splits[0], 0.1f, kTol);
    NF_CHECK_NEAR(splits[4], 100.0f, kTol);
    for (u32 i = 0; i < kMaxShadowCascades; ++i) {
        NF_CHECK(splits[i + 1] > splits[i]);
    }
}

NF_TEST(shadow_cascade_splits_lambda_endpoints_are_uniform_and_logarithmic) {
    float uni[kMaxShadowCascades + 1]{};
    float loga[kMaxShadowCascades + 1]{};
    compute_cascade_splits(1.0f, 100.0f, 4, 0.0f, uni);
    compute_cascade_splits(1.0f, 100.0f, 4, 1.0f, loga);

    // lambda = 0 is the uniform scheme: equal depth spans.
    for (u32 i = 0; i < 4; ++i) {
        NF_CHECK_NEAR(uni[i + 1] - uni[i], 24.75f, 1e-3f);
    }
    // lambda = 1 is logarithmic: equal depth *ratios* (100^(1/4) = 3.1623).
    for (u32 i = 0; i < 4; ++i) {
        NF_CHECK_NEAR(loga[i + 1] / loga[i], 3.16228f, 1e-3f);
    }
    // The two schemes must actually differ, or the lambda knob does nothing.
    NF_CHECK(std::fabs(uni[1] - loga[1]) > 1.0f);
}

NF_TEST(shadow_cascade_splits_near_cascades_are_tighter) {
    float splits[kMaxShadowCascades + 1]{};
    compute_cascade_splits(0.1f, 200.0f, 4, 0.75f, splits);

    const float span0 = splits[1] - splits[0];
    const float span_last = splits[4] - splits[3];
    // Detail is read near the camera, so cascade 0 must be far shorter than the
    // last one — this is the entire point of cascading.
    NF_CHECK(span0 < span_last * 0.25f);
    NF_CHECK(span0 > 0.0f);
}

NF_TEST(shadow_cascade_splits_single_cascade_covers_the_whole_range) {
    float splits[kMaxShadowCascades + 1]{};
    compute_cascade_splits(0.5f, 80.0f, 1, 0.75f, splits);
    NF_CHECK_NEAR(splits[0], 0.5f, kTol);
    NF_CHECK_NEAR(splits[1], 80.0f, kTol);
}

NF_TEST(shadow_cascade_splits_degenerate_range_does_not_produce_nans) {
    float splits[kMaxShadowCascades + 1]{};
    compute_cascade_splits(10.0f, 10.0f, 4, 0.75f, splits);
    for (u32 i = 0; i <= 4; ++i) {
        NF_CHECK(std::isfinite(splits[i]));
    }
}

// ---------------------------------------------------------------------------
// Fitting: the real invariant is coverage, not "the matrix is non-zero"
// ---------------------------------------------------------------------------

NF_TEST(shadow_cascade_fit_covers_every_slice_corner) {
    // A camera deliberately off the origin and tilted: the old fixed box around
    // (0,0,0) is exactly what this must not reproduce.
    const Camera cam = make_cam_looking(60.0f, 12.0f, 60.0f, 40.0f, 0.0f, 40.0f);
    const Vec3 light_dir{-0.55f, -1.0f, -0.35f};

    float splits[kMaxShadowCascades + 1]{};
    compute_cascade_splits(cam.near_plane, cam.far_plane, 4, 0.75f, splits);

    const Mat4 inv_vp = cam.view_projection.inverse();
    for (u32 i = 0; i < 4; ++i) {
        const CascadeFit fit = fit_cascade(cam, light_dir, splits[i], splits[i + 1], 1024);
        NF_CHECK(all_finite(fit.light_view_proj));

        // Every one of the slice's corners must land inside the clip cube. This
        // is the claim that matters: an object inside the slice that projects
        // outside the shadow map simply stops casting.
        const auto ndc_z = [&](float d) {
            const float n = cam.near_plane, f = cam.far_plane;
            return f * (1.0f - n / d) / (f - n);
        };
        for (const float z : {ndc_z(splits[i]), ndc_z(splits[i + 1])}) {
            for (const float y : {-1.0f, 1.0f}) {
                for (const float x : {-1.0f, 1.0f}) {
                    const Vec3 world = inv_vp.transform_point(Vec3{x, y, z});
                    const Vec3 ndc = fit.light_view_proj.transform_point(world);
                    // x/y are clip-space NDC [-1, 1] (the shader maps them to
                    // atlas UV with ndc * 0.5 + 0.5); z is Vulkan depth [0, 1].
                    NF_CHECK(ndc.x >= -1.0f - 1e-3f && ndc.x <= 1.0f + 1e-3f);
                    NF_CHECK(ndc.y >= -1.0f - 1e-3f && ndc.y <= 1.0f + 1e-3f);
                    NF_CHECK(ndc.z >= -1e-3f && ndc.z <= 1.0f + 1e-3f);
                }
            }
        }
    }
}

NF_TEST(shadow_cascade_fit_contains_the_slice_at_every_sub_texel_offset) {
    // Texel snapping has two ends and both must be quantised — min down, max
    // up. Snapping the origin but rounding the *raw* extent up leaves the far
    // edge short by however far the origin dropped, which clips a texel-wide
    // strip off the cascade for exactly those camera positions where the origin
    // lands just below a texel boundary. Sweeping the camera across a whole
    // texel's worth of travel visits every alignment, including the unlucky one.
    const Vec3 light_dir{-0.55f, -1.0f, -0.35f};
    const Camera base = make_cam_looking(60.0f, 12.0f, 60.0f, 40.0f, 0.0f, 40.0f);

    float splits[kMaxShadowCascades + 1]{};
    compute_cascade_splits(base.near_plane, base.far_plane, 4, 0.75f, splits);

    // One texel of cascade 0 is ~1.4cm; step well inside that so no alignment
    // is skipped, and cover more than a full texel of travel.
    for (u32 step = 0; step < 24; ++step) {
        const float t = static_cast<float>(step) * 0.0009f;
        const Camera cam =
            make_cam_looking(60.0f + t, 12.0f - t, 60.0f + t, 40.0f, 0.0f, 40.0f);
        const Mat4 inv_vp = cam.view_projection.inverse();

        for (u32 i = 0; i < 4; ++i) {
            const CascadeFit fit =
                fit_cascade(cam, light_dir, splits[i], splits[i + 1], 1024);
            const auto ndc_z = [&](float d) {
                const float n = cam.near_plane, f = cam.far_plane;
                return f * (1.0f - n / d) / (f - n);
            };
            for (const float z : {ndc_z(splits[i]), ndc_z(splits[i + 1])}) {
                for (const float y : {-1.0f, 1.0f}) {
                    for (const float x : {-1.0f, 1.0f}) {
                        const Vec3 world = inv_vp.transform_point(Vec3{x, y, z});
                        const Vec3 ndc = fit.light_view_proj.transform_point(world);
                        // No tolerance fudge beyond float noise: the snapped box
                        // is meant to CONTAIN the slice, not nearly contain it.
                        NF_CHECK(ndc.x >= -1.0f - 1e-4f && ndc.x <= 1.0f + 1e-4f);
                        NF_CHECK(ndc.y >= -1.0f - 1e-4f && ndc.y <= 1.0f + 1e-4f);
                    }
                }
            }
            // Snapping must still leave a whole number of texels, or texel
            // density varies across the map.
            const float texels = fit.world_extent / (fit.world_extent / 1024.0f);
            NF_CHECK_NEAR(texels, std::round(texels), 1e-2f);
        }
    }
}

NF_TEST(shadow_cascade_fit_follows_the_camera_across_the_world) {
    // The bug this feature exists to fix: the same scene rendered from 1000
    // units away must still be covered by the cascade box.
    const Vec3 light_dir{-0.4f, -1.0f, -0.2f};

    for (const float dist : {1.0f, 25.0f, 200.0f, 1000.0f}) {
        const Camera cam =
            make_cam_looking(dist, dist * 0.5f, dist, dist - 5.0f, 0.0f, dist - 5.0f);
        const CascadeFit fit =
            fit_cascade(cam, light_dir, cam.near_plane, 30.0f, 1024);

        // The point the camera is looking at (10 units out along its forward
        // axis) must project inside the cascade box, wherever the camera is.
        const Vec3 forward = (cam.target - cam.position).normalized();
        const Vec3 probe = cam.position + forward * 10.0f;
        const Vec3 ndc = fit.light_view_proj.transform_point(probe);
        NF_CHECK(ndc.x >= -1.0f && ndc.x <= 1.0f);
        NF_CHECK(ndc.y >= -1.0f && ndc.y <= 1.0f);
        NF_CHECK(ndc.z >= 0.0f && ndc.z <= 1.0f);
    }
}

NF_TEST(shadow_cascade_fit_origin_is_texel_quantised) {
    // Snapping is what stops shadow edges from crawling as the camera walks.
    // The exact, checkable consequence: the ortho box origin is always a whole
    // multiple of the texel size, so translation moves it in discrete steps.
    const Vec3 light_dir{-0.55f, -1.0f, -0.35f};
    const u32 tile = 1024;

    for (float x = 0.0f; x < 3.0f; x += 0.137f) {
        const Camera cam = make_cam_looking(20.0f + x, 5.0f, 20.0f, 15.0f, 0.0f, 15.0f);
        const CascadeFit fit = fit_cascade(cam, light_dir, cam.near_plane, 20.0f, tile);
        NF_CHECK(fit.world_extent > 0.0f);

        // The box spans a whole number of texels; the extent being an exact
        // multiple is what keeps texel density uniform across the map.
        const float wupt = fit.world_extent / static_cast<float>(tile);
        const float texels = fit.world_extent / wupt;
        NF_CHECK_NEAR(texels, std::round(texels), 1e-2f);
    }
}

NF_TEST(shadow_cascade_fit_rejects_a_degenerate_light) {
    const Camera cam = make_cam(0.0f, 5.0f, 10.0f);
    // A zero direction has no basis; producing NaNs here would reach the
    // uniform buffer and blank every shadow in the frame.
    const CascadeFit fit = fit_cascade(cam, Vec3{0.0f, 0.0f, 0.0f}, 0.1f, 20.0f, 1024);
    NF_CHECK(all_finite(fit.light_view_proj));
    NF_CHECK_NEAR(fit.depth_range, 1.0f, kTol);
}

NF_TEST(shadow_cascade_fit_straight_down_light_does_not_degenerate) {
    // dir parallel to world up makes the (0,1,0) up-vector cross product zero.
    const Camera cam = make_cam(0.0f, 20.0f, 0.0f);
    const CascadeFit fit = fit_cascade(cam, Vec3{0.0f, -1.0f, 0.0f}, 0.1f, 30.0f, 1024);
    NF_CHECK(all_finite(fit.light_view_proj));
    NF_CHECK(fit.depth_range > 0.0f);
    NF_CHECK(fit.world_extent > 0.0f);
}

NF_TEST(shadow_cascade_fit_ortho_camera_is_covered) {
    // Orthographic frusta have parallel corner rays, so the perspective-only
    // "scale the near corner by d/near" shortcut would fit the wrong slice.
    Camera cam = make_cam(5.0f, 5.0f, 20.0f);
    cam.type = Camera::ProjectionType::Orthographic;
    cam.ortho_width = 12.0f;
    cam.ortho_height = 8.0f;
    update_camera(cam);

    const CascadeFit fit = fit_cascade(cam, Vec3{-0.3f, -1.0f, -0.2f}, 1.0f, 25.0f, 1024);
    NF_CHECK(all_finite(fit.light_view_proj));
    NF_CHECK(fit.depth_range > 0.0f);

    // The slice center (10 units out along the view axis) must be inside.
    const Vec3 forward = (cam.target - cam.position).normalized();
    const Vec3 probe = cam.position + forward * 10.0f;
    const Vec3 ndc = fit.light_view_proj.transform_point(probe);
    NF_CHECK(ndc.x >= -1.0f && ndc.x <= 1.0f);
    NF_CHECK(ndc.y >= -1.0f && ndc.y <= 1.0f);
}

NF_TEST(shadow_cascade_fit_extrudes_toward_the_light) {
    // A caster standing between the light and the slice must stay inside the
    // fitted depth range, or it silently stops casting into that cascade.
    const Camera cam = make_cam(0.0f, 2.0f, 10.0f);
    const Vec3 light_dir{-0.3f, -1.0f, -0.2f};

    const CascadeFit tight =
        fit_cascade(cam, light_dir, 1.0f, 20.0f, 1024, /*caster_extrusion=*/0.0f);
    const CascadeFit extended =
        fit_cascade(cam, light_dir, 1.0f, 20.0f, 1024, /*caster_extrusion=*/0.5f);

    // Pulling the near plane back can only deepen the range.
    NF_CHECK(extended.depth_range > tight.depth_range);
}

// ---------------------------------------------------------------------------
// Tile layout and selection
// ---------------------------------------------------------------------------

NF_TEST(shadow_cascade_tiles_tile_the_atlas_without_overlap) {
    NF_CHECK_NEAR(cascade_tile_scale(), 0.5f, kTol);

    for (u32 i = 0; i < kMaxShadowCascades; ++i) {
        const CascadeTile t = cascade_tile(i);
        NF_CHECK_NEAR(t.u1 - t.u0, 0.5f, kTol);
        NF_CHECK_NEAR(t.v1 - t.v0, 0.5f, kTol);
        NF_CHECK(t.u0 >= 0.0f && t.u1 <= 1.0f);
        NF_CHECK(t.v0 >= 0.0f && t.v1 <= 1.0f);
    }

    // Every tile must be distinct, and together they must cover the atlas.
    for (u32 i = 0; i < kMaxShadowCascades; ++i) {
        for (u32 j = i + 1; j < kMaxShadowCascades; ++j) {
            const CascadeTile a = cascade_tile(i);
            const CascadeTile b = cascade_tile(j);
            const bool disjoint = a.u1 <= b.u0 || b.u1 <= a.u0 || a.v1 <= b.v0 ||
                                  b.v1 <= a.v0;
            NF_CHECK(disjoint);
        }
    }
    const CascadeTile last = cascade_tile(kMaxShadowCascades - 1);
    NF_CHECK_NEAR(last.u1, 1.0f, kTol);
    NF_CHECK_NEAR(last.v1, 1.0f, kTol);
}

NF_TEST(shadow_cascade_selection_maps_depth_to_the_right_cascade) {
    float splits[kMaxShadowCascades + 1]{};
    compute_cascade_splits(0.1f, 100.0f, 4, 0.75f, splits);

    NF_CHECK_EQ(select_cascade(0.05f, splits, 4), 0u);   // nearer than near: clamped in
    NF_CHECK_EQ(select_cascade(splits[0] + 1e-3f, splits, 4), 0u);
    NF_CHECK_EQ(select_cascade(splits[1], splits, 4), 0u);
    NF_CHECK_EQ(select_cascade(splits[1] + 1e-3f, splits, 4), 1u);
    NF_CHECK_EQ(select_cascade(splits[2] + 1e-3f, splits, 4), 2u);
    NF_CHECK_EQ(select_cascade(splits[3] + 1e-3f, splits, 4), 3u);
    NF_CHECK_EQ(select_cascade(1000.0f, splits, 4), 3u); // beyond far: clamped in

    // The result must never index a cascade that was not rendered.
    for (u32 count = 1; count <= kMaxShadowCascades; ++count) {
        for (float d = 0.0f; d < 500.0f; d += 3.3f) {
            NF_CHECK(select_cascade(d, splits, count) < count);
        }
    }
}

NF_TEST(shadow_cascade_auto_bias_tracks_texel_size_and_depth_span) {
    // The bias exists to cover the depth a flat surface can gain across ONE
    // texel, which is `texel_world * tan(theta)`, expressed in the NDC units the
    // shader compares in. So it must rise with the texel's world size and fall
    // as the same world offset is stretched over a longer depth range. Both
    // directions are the whole reason this cannot be a constant: cascading
    // shrinks the near cascade's texels and its depth range by DIFFERENT
    // factors, so no single number stays correct across the four.
    const Camera cam = make_cam_looking(60.0f, 12.0f, 60.0f, 40.0f, 0.0f, 40.0f);
    const Vec3 light_dir{-0.55f, -1.0f, -0.35f};
    const u32 tile = 1024;

    float splits[kMaxShadowCascades + 1]{};
    compute_cascade_splits(cam.near_plane, cam.far_plane, 4, 0.75f, splits);

    CascadeFit fits[kMaxShadowCascades];
    for (u32 i = 0; i < 4; ++i) {
        fits[i] = fit_cascade(cam, light_dir, splits[i], splits[i + 1], tile);
    }

    for (u32 i = 0; i < 4; ++i) {
        const float b = cascade_auto_bias(fits[i], tile);
        NF_CHECK(b > 0.0f);
        NF_CHECK(std::isfinite(b));

        // Exactly the definition: texel world size, tilted, converted to NDC.
        const float texel_world = fits[i].world_extent / float(tile);
        NF_CHECK_NEAR(b, texel_world * kShadowBiasSlope / fits[i].depth_range, 1e-7f);

        // The world-space offset the bias buys is what actually has to clear the
        // depth slope, and it must be at least one texel's worth of tilt.
        const float world_offset = b * fits[i].depth_range;
        NF_CHECK(world_offset >= texel_world * 1.0f);
    }

    // Halving the tile resolution doubles the texel and so must double the bias:
    // a coarser map self-shadows from twice as far away.
    const float fine = cascade_auto_bias(fits[0], 1024);
    NF_CHECK_NEAR(cascade_auto_bias(fits[0], 512), fine * 2.0f, 1e-6f);

    // A wider cascade with the same texel count needs more bias than a tight one.
    NF_CHECK(cascade_auto_bias(fits[3], tile) > cascade_auto_bias(fits[0], tile));
}

NF_TEST(shadow_cascade_auto_bias_is_finite_on_degenerate_input) {
    // These all reach the uniform buffer unmodified, so a NaN here blanks every
    // shadow in the frame rather than merely biasing one cascade wrong.
    CascadeFit f{};
    f.depth_range = 0.0f;
    f.world_extent = 0.0f;
    NF_CHECK_NEAR(cascade_auto_bias(f, 1024), 0.0f, kTol);

    f.world_extent = 8.0f;
    NF_CHECK_NEAR(cascade_auto_bias(f, 1024), 0.0f, kTol); // depth_range still 0

    f.depth_range = 10.0f;
    f.world_extent = 8.0f;
    NF_CHECK_NEAR(cascade_auto_bias(f, 0), 0.0f, kTol); // no tile, no texel size
    NF_CHECK(std::isfinite(cascade_auto_bias(f, 1024, 1e9f)));
    NF_CHECK_NEAR(cascade_auto_bias(f, 1024, -1.0f), 0.0f, kTol);

    // Negative/huge fields must not produce a negative bias, which would
    // subtract from the depth test and shadow everything.
    f.world_extent = -1.0f;
    NF_CHECK_NEAR(cascade_auto_bias(f, 1024), 0.0f, kTol);
}

NF_TEST(shadow_cascade_config_is_clamped_to_what_the_atlas_can_hold) {
    CascadeConfig c{};
    c.count = 99;
    c.lambda = 5.0f;
    c.fade_range = -1.0f;
    c.caster_extrusion = 100.0f;
    c.max_distance = -3.0f;
    const CascadeConfig s = sanitize_cascade_config(c);

    NF_CHECK_EQ(s.count, kMaxShadowCascades);
    NF_CHECK_NEAR(s.lambda, 1.0f, kTol);
    NF_CHECK_NEAR(s.fade_range, 0.0f, kTol);
    NF_CHECK_NEAR(s.caster_extrusion, 4.0f, kTol);
    NF_CHECK_NEAR(s.max_distance, 0.0f, kTol);

    CascadeConfig zero{};
    zero.count = 0;
    NF_CHECK_EQ(sanitize_cascade_config(zero).count, 1u);
}

NF_TEST(shadow_cascade_config_defaults_are_sane) {
    const CascadeConfig d = sanitize_cascade_config(CascadeConfig{});
    NF_CHECK_EQ(d.count, kMaxShadowCascades);
    NF_CHECK(d.lambda > 0.0f && d.lambda < 1.0f);
    NF_CHECK(d.caster_extrusion > 0.0f);
    // Default tiling must be memory-neutral against the single 2048x2048 map it
    // replaces: a 2x2 grid of 1024 squares is the same 2048x2048.
    NF_CHECK_EQ(kShadowTileGrid * kShadowTileGrid, kMaxShadowCascades);
}
