// RHITests — post FX mirror + camera shake (pure CPU, no device).

#include <NF/Test/TestFramework.hpp>
#include <NF/Rendering/CameraShake.hpp>
#include <NF/Rendering/Renderer3D.hpp>

#include <cmath>

using namespace nf;
using namespace nf::rendering;

// ---------------------------------------------------------------------------
// Post FX
// ---------------------------------------------------------------------------

NF_TEST(postfx_neutral_is_identity) {
    Renderer3D renderer; // defaults are neutral
    NF_CHECK_NEAR(renderer.postfx().saturation, 1.0f, 1e-6f);
    NF_CHECK_NEAR(renderer.postfx().vignette, 0.0f, 1e-6f);
    const Vec3 c{0.4f, 0.7f, 0.2f};
    for (Vec2 uv : {Vec2{0.5f, 0.5f}, Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f}}) {
        const Vec3 out = apply_postfx(c, uv, renderer.postfx());
        NF_CHECK_NEAR(out.x, c.x, 1e-6f);
        NF_CHECK_NEAR(out.y, c.y, 1e-6f);
        NF_CHECK_NEAR(out.z, c.z, 1e-6f);
    }
}

NF_TEST(postfx_zero_saturation_is_luma_grayscale) {
    PostFxParams p;
    p.saturation = 0.0f;
    const Vec3 out = apply_postfx(Vec3{0.8f, 0.2f, 0.1f}, Vec2{0.5f, 0.5f}, p);
    const float luma = 0.8f * 0.2126f + 0.2f * 0.7152f + 0.1f * 0.0722f;
    NF_CHECK_NEAR(out.x, luma, 1e-5f);
    NF_CHECK_NEAR(out.y, luma, 1e-5f);
    NF_CHECK_NEAR(out.z, luma, 1e-5f);
}

NF_TEST(postfx_vignette_darkens_corners_not_center) {
    PostFxParams p;
    p.vignette = 1.0f;
    const Vec3 c{0.6f, 0.6f, 0.6f};
    const Vec3 center = apply_postfx(c, Vec2{0.5f, 0.5f}, p);
    NF_CHECK_NEAR(center.x, c.x, 1e-5f); // untouched
    const Vec3 corner = apply_postfx(c, Vec2{0.0f, 0.0f}, p);
    NF_CHECK(corner.x < c.x * 0.5f); // heavily darkened
    NF_CHECK(corner.x > 0.0f); // never crushed to black
    // Monotonic falloff toward the corner.
    const Vec3 mid = apply_postfx(c, Vec2{0.25f, 0.25f}, p);
    NF_CHECK(mid.x < center.x && mid.x > corner.x);
}

NF_TEST(postfx_setter_roundtrips) {
    Renderer3D renderer;
    PostFxParams p;
    p.saturation = 1.4f;
    p.vignette = 0.35f;
    renderer.set_postfx(p);
    NF_CHECK_NEAR(renderer.postfx().saturation, 1.4f, 1e-6f);
    NF_CHECK_NEAR(renderer.postfx().vignette, 0.35f, 1e-6f);
}

// ---------------------------------------------------------------------------
// Camera shake
// ---------------------------------------------------------------------------

NF_TEST(shake_zero_trauma_is_zero_offset) {
    CameraShake shake;
    shake.update(1.0f);
    const Vec3 p = shake.position_offset();
    const Vec3 r = shake.rotation_offset_deg();
    NF_CHECK_NEAR(p.x, 0.0f, 1e-6f);
    NF_CHECK_NEAR(p.y, 0.0f, 1e-6f);
    NF_CHECK_NEAR(p.z, 0.0f, 1e-6f);
    NF_CHECK_NEAR(r.x, 0.0f, 1e-6f);
}

NF_TEST(shake_trauma_clamps_and_decays) {
    CameraShake shake;
    shake.add_trauma(5.0f);
    NF_CHECK_NEAR(shake.trauma(), 1.0f, 1e-6f);
    shake.add_trauma(-2.0f);
    NF_CHECK_NEAR(shake.trauma(), 0.0f, 1e-6f);
    shake.add_trauma(1.0f);
    shake.update(0.5f); // decay 1.4/s -> 1 - 0.7 = 0.3
    NF_CHECK_NEAR(shake.trauma(), 0.3f, 1e-5f);
    shake.update(10.0f);
    NF_CHECK_NEAR(shake.trauma(), 0.0f, 1e-6f);
}

NF_TEST(shake_offset_scales_with_trauma_squared) {
    CameraShake full;
    full.set_decay_per_second(0.0f); // hold trauma: ratio must be exact
    full.add_trauma(1.0f);
    full.update(0.37f);
    CameraShake half;
    half.set_decay_per_second(0.0f);
    half.add_trauma(0.5f);
    half.update(0.37f);
    const Vec3 pf = full.position_offset();
    const Vec3 ph = half.position_offset();
    // trauma^2 ratio is 4x; noise is identical (same clock) so the ratio
    // holds per channel (guard near-zero denominators).
    for (int i = 0; i < 3; ++i) {
        const float f = i == 0 ? pf.x : (i == 1 ? pf.y : pf.z);
        const float h = i == 0 ? ph.x : (i == 1 ? ph.y : ph.z);
        if (std::abs(f) > 1e-4f) {
            NF_CHECK_NEAR(h / f, 0.25f, 1e-4f);
        }
    }
}

NF_TEST(shake_is_bounded_and_deterministic) {
    CameraShake a;
    a.add_trauma(1.0f);
    CameraShake b;
    b.add_trauma(1.0f);
    for (int i = 0; i < 60; ++i) {
        a.update(1.0f / 60.0f);
        b.update(1.0f / 60.0f);
        const Vec3 pa = a.position_offset();
        const Vec3 pb = b.position_offset();
        NF_CHECK_NEAR(pa.x, pb.x, 1e-6f);
        NF_CHECK_NEAR(pa.y, pb.y, 1e-6f);
        NF_CHECK(std::abs(pa.x) <= 0.36f);
        NF_CHECK(std::abs(pa.y) <= 0.36f);
        const Vec3 ra = a.rotation_offset_deg();
        NF_CHECK(std::abs(ra.x) <= 6.1f);
        NF_CHECK(std::abs(ra.z) <= 3.1f);
    }
}
