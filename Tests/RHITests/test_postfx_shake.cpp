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
// Tone mapping (the pre-gamma operator; the four modes of tonemap.frag)
// ---------------------------------------------------------------------------

NF_TEST(tonemap_default_mode_is_exponential) {
    // Exponential is mode 0 and is what every golden pixel was authored
    // against, so a renderer that never touches this renders as before.
    Renderer3D renderer;
    NF_CHECK(renderer.tonemap_mode() == TonemapMode::Exponential);
    const Vec3 c{0.4f, 0.7f, 0.2f};
    const Vec3 out = tonemap(c, 1.0f, renderer.tonemap_mode());
    NF_CHECK_NEAR(out.x, 1.0f - std::exp(-0.4f), 1e-6f);
    NF_CHECK_NEAR(out.y, 1.0f - std::exp(-0.7f), 1e-6f);
    NF_CHECK_NEAR(out.z, 1.0f - std::exp(-0.2f), 1e-6f);
}

NF_TEST(tonemap_exponential_applies_exposure_then_the_operator) {
    // 1 - exp(-x*e) for a range of exposures, per channel.
    const float exposures[3] = {0.5f, 1.0f, 4.0f};
    for (float e : exposures) {
        const Vec3 out = tonemap(Vec3{0.3f, 1.0f, 2.5f}, e, TonemapMode::Exponential);
        NF_CHECK_NEAR(out.x, 1.0f - std::exp(-0.3f * e), 1e-6f);
        NF_CHECK_NEAR(out.y, 1.0f - std::exp(-1.0f * e), 1e-6f);
        NF_CHECK_NEAR(out.z, 1.0f - std::exp(-2.5f * e), 1e-6f);
    }
}

NF_TEST(tonemap_reinhard_and_aces_match_their_published_forms) {
    // Reinhard: x/(1+x) — 1.0 is exactly the half-point, 3.0 exactly 0.75.
    NF_CHECK_NEAR(tonemap(Vec3{1.0f, 0.0f, 0.0f}, 1.0f, TonemapMode::Reinhard).x, 0.5f, 1e-6f);
    NF_CHECK_NEAR(tonemap(Vec3{3.0f, 0.0f, 0.0f}, 1.0f, TonemapMode::Reinhard).x, 0.75f, 1e-6f);
    // ACES (Narkowicz): at x=1 the fit is (2.51 + 0.03) / (2.43 + 0.59 + 0.14).
    // Pinned from the constants rather than copied as a decimal so a typo in
    // either side of the mirror fails here, not just "looks filmic".
    const float expected_aces = (1.0f * (2.51f * 1.0f + 0.03f)) / (1.0f * (2.43f * 1.0f + 0.59f) + 0.14f);
    NF_CHECK_NEAR(tonemap(Vec3{1.0f, 0.0f, 0.0f}, 1.0f, TonemapMode::ACES).x, expected_aces, 1e-6f);
}

NF_TEST(tonemap_operators_are_monotone_and_bounded) {
    // Exponential, ACES and Reinhard all map [0, inf) into [0, 1) and rise
    // monotonically — that is what makes them display curves. Linear is
    // deliberately excluded: it is a diagnostic mode and must stay unclamped,
    // which is asserted separately below.
    const TonemapMode bounded[3] = {TonemapMode::Exponential, TonemapMode::ACES, TonemapMode::Reinhard};
    for (TonemapMode mode : bounded) {
        float prev = 0.0f;
        for (float x = 0.0f; x < 8.0f; x += 0.25f) {
            const float v = tonemap(Vec3{x, 0.0f, 0.0f}, 1.0f, mode).x;
            NF_CHECK(v >= 0.0f && v <= 1.0f);
            NF_CHECK(v >= prev); // monotone (equal only at x = 0)
            prev = v;
        }
        // Exponential and Reinhard approach 1 asymptotically and never reach it
        // at a finite input — in the reals. In float32 the ULP at 1.0 is
        // 1.19e-7, so 1 - exp(-x) rounds to exactly 1.0 once x passes ~17; the
        // probe is at 10, the last decade where the difference still resolves.
        // ACES is excluded from this clause: the Narkowicz fit is a polynomial
        // that overshoots, so both the shader and the CPU mirror clamp it to
        // [0, 1] and it saturates to exactly 1.0 by x = 1.
        if (mode != TonemapMode::ACES) {
            NF_CHECK(tonemap(Vec3{10.0f, 0.0f, 0.0f}, 1.0f, mode).x < 1.0f);
        }
    }
}

NF_TEST(tonemap_linear_is_exposure_only_and_unclamped) {
    // Linear must NOT clamp in the operator: the UNorm target clamps on write,
    // so clamping here would hide an over-bright scene from a diagnostic
    // capture. Exposure multiplies straight through.
    const Vec3 out = tonemap(Vec3{1.0f, 0.5f, 0.25f}, 3.0f, TonemapMode::Linear);
    NF_CHECK_NEAR(out.x, 3.0f, 1e-6f);
    NF_CHECK_NEAR(out.y, 1.5f, 1e-6f);
    NF_CHECK_NEAR(out.z, 0.75f, 1e-6f);
    NF_CHECK(tonemap(Vec3{10.0f, 0.0f, 0.0f}, 1.0f, TonemapMode::Linear).x > 1.0f);
}

NF_TEST(tonemap_modes_produce_distinct_results) {
    // The mode switch is load-bearing: if a branch collapses, these four
    // stops at a mid-grey input must not agree.
    const float x = tonemap(Vec3{0.6f, 0.6f, 0.6f}, 1.0f, TonemapMode::Exponential).x;
    float prev = x;
    for (TonemapMode mode : {TonemapMode::ACES, TonemapMode::Reinhard, TonemapMode::Linear}) {
        const float v = tonemap(Vec3{0.6f, 0.6f, 0.6f}, 1.0f, mode).x;
        NF_CHECK(std::abs(v - prev) > 1e-3f);
        prev = v;
    }
}

NF_TEST(tonemap_mode_setter_roundtrips) {
    Renderer3D renderer;
    renderer.set_tonemap_mode(TonemapMode::Reinhard);
    NF_CHECK(renderer.tonemap_mode() == TonemapMode::Reinhard);
    renderer.set_tonemap_mode(TonemapMode::Linear);
    NF_CHECK(renderer.tonemap_mode() == TonemapMode::Linear);
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
