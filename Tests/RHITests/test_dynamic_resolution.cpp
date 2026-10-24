// Tests/RHITests/test_dynamic_resolution.cpp — the render-scale governor
// (Phase 38).
//
// This suite is intentionally GPU-free. The governor is a policy object: it
// consumes frame times in seconds and returns a scale. Every claim worth making
// about it — the cooldown, the hysteresis band, the step direction, the clamps,
// the reset, and what a hitch is allowed to do — is exact arithmetic, so it is
// pinned here without a device.
//
// What this suite deliberately does NOT claim: that the renderer rebuilds at
// the returned scale. That integration is covered by the GPU render-scale test
// (Phase 37) and by the Runtime test that drives real frames through
// Runtime::update(dt).

#include <NF/Test/TestFramework.hpp>
#include <NF/Rendering/DynamicResolution.hpp>
#include <NF/Rendering/RenderScale.hpp>

#include <cmath>
#include <limits>

namespace {

using nf::f32;
using namespace nf::rendering;

DynamicResolutionConfig config(f32 target_ms = 16.6667f, f32 min_scale = 0.5f,
                               f32 max_scale = 1.0f, f32 step = 0.1f,
                               f32 cooldown = 0.5f, f32 tolerance = 0.1f) {
    DynamicResolutionConfig c{};
    c.enabled = true;
    c.target_frame_ms = target_ms;
    c.min_scale = min_scale;
    c.max_scale = max_scale;
    c.step = step;
    c.cooldown_s = cooldown;
    c.tolerance = tolerance;
    return c;
}

/// Feeds `count` frames of `frame_s` and returns the final scale. The caller
/// states the count explicitly because the cooldown is measured in TIME, not
/// frames: a test that says "twenty 25 ms frames" documents the 0.5 s boundary.
f32 feed(DynamicResolution& governor, f32 frame_s, int count) {
    f32 scale = governor.scale();
    for (int i = 0; i < count; ++i) {
        scale = governor.update(frame_s);
    }
    return scale;
}

} // namespace

NF_TEST(dynamic_resolution_is_off_by_default) {
    DynamicResolution governor;
    // A default-constructed governor must not change anything, even if it is
    // fed a catastrophically slow frame. Scenes that never opt in render at
    // exactly the resolution they did before this feature existed.
    NF_CHECK(!governor.enabled());
    NF_CHECK_NEAR(governor.scale(), 1.0f, 1e-6f);
    NF_CHECK_NEAR(governor.update(1.0f), 1.0f, 1e-6f);
    NF_CHECK_NEAR(governor.scale(), 1.0f, 1e-6f);
}

NF_TEST(dynamic_resolution_sanitizes_an_illegal_config) {
    DynamicResolution governor;
    DynamicResolutionConfig bad{};
    bad.enabled = true;
    bad.target_frame_ms = std::numeric_limits<f32>::quiet_NaN();
    bad.min_scale = 2.5f;   // clamps to 2.0, then swaps with the max below
    bad.max_scale = 0.1f;   // clamps to 0.25
    bad.step = std::numeric_limits<f32>::quiet_NaN();
    bad.cooldown_s = -3.0f;
    bad.tolerance = 9.0f;

    governor.configure(bad);
    NF_CHECK(governor.enabled());
    NF_CHECK_NEAR(governor.config().target_frame_ms, DynamicResolution::kDefaultTargetFrameMs,
                  1e-4f);
    NF_CHECK_NEAR(governor.config().min_scale, kMinRenderScale, 1e-6f);
    NF_CHECK_NEAR(governor.config().max_scale, kMaxRenderScale, 1e-6f);
    NF_CHECK_NEAR(governor.config().step, DynamicResolution::kDefaultStep, 1e-6f);
    NF_CHECK_NEAR(governor.config().cooldown_s, 0.0f, 1e-6f);
    NF_CHECK_NEAR(governor.config().tolerance, 1.0f, 1e-6f);
    NF_CHECK_NEAR(governor.scale(), kMaxRenderScale, 1e-6f);
}

NF_TEST(dynamic_resolution_does_not_move_before_the_cooldown) {
    DynamicResolution governor;
    governor.configure(config(16.6667f, 0.5f, 1.0f, 0.1f, 0.5f));
    // Ten 25 ms frames are 0.25 s: the frames are far over budget, but the
    // controller is not allowed to react yet.
    NF_CHECK_NEAR(feed(governor, 0.025f, 10), 1.0f, 1e-6f);
    // Past 0.5 s the same load is accepted as a real miss: one step down.
    NF_CHECK_NEAR(feed(governor, 0.025f, 12), 0.9f, 1e-6f);
}

NF_TEST(dynamic_resolution_lowers_then_raises_one_step_at_a_time) {
    DynamicResolution governor;
    governor.configure(config());
    NF_CHECK_NEAR(feed(governor, 0.025f, 25), 0.9f, 1e-6f);
    // The average was reset to target after the change, so recovery waits for
    // a fresh cooldown of genuinely fast frames. Each 25 ms of 10 ms frames is
    // still within the band for a while.
    NF_CHECK_NEAR(feed(governor, 0.010f, 60), 1.0f, 1e-6f);
}

NF_TEST(dynamic_resolution_stays_inside_the_hysteresis_band) {
    DynamicResolution governor;
    governor.configure(config());
    // 17 ms is 2% over a 16.67 ms target, well inside the 10% band. More than
    // a second of it must not move the scale.
    NF_CHECK_NEAR(feed(governor, 0.017f, 60), 1.0f, 1e-6f);
    // 20 ms is 20% over: outside the band, so the next step applies. Ten more
    // frames are enough to cross the band but not enough to earn a second step
    // after the cooldown resets — that is what "one step at a time" means.
    NF_CHECK_NEAR(feed(governor, 0.020f, 10), 0.9f, 1e-6f);
}

NF_TEST(dynamic_resolution_clamps_at_both_bounds) {
    DynamicResolution governor;
    governor.configure(config(16.6667f, 0.8f, 0.9f, 0.1f, 0.2f));
    NF_CHECK_NEAR(governor.scale(), 0.9f, 1e-6f);
    NF_CHECK_NEAR(feed(governor, 0.100f, 10), 0.8f, 1e-6f);
    // At the floor and still slow: no further step, no invalid scale.
    NF_CHECK_NEAR(feed(governor, 0.100f, 10), 0.8f, 1e-6f);
    // At the ceiling and fast: no further step either.
    NF_CHECK_NEAR(feed(governor, 0.001f, 20), 0.9f, 1e-6f);
    NF_CHECK_NEAR(feed(governor, 0.001f, 20), 0.9f, 1e-6f);
}

NF_TEST(dynamic_resolution_reset_clears_cooldown_and_average) {
    DynamicResolution governor;
    governor.configure(config());
    NF_CHECK_NEAR(feed(governor, 0.025f, 25), 0.9f, 1e-6f);
    governor.reset(1.0f);
    NF_CHECK_NEAR(governor.scale(), 1.0f, 1e-6f);
    // The first slow frame after reset must not immediately spend a step: the
    // reset also cleared the time accumulated toward the cooldown.
    NF_CHECK_NEAR(governor.update(0.100f), 1.0f, 1e-6f);
    NF_CHECK_NEAR(governor.update(0.100f), 1.0f, 1e-6f);
}

NF_TEST(dynamic_resolution_ignores_non_finite_and_non_positive_frame_times) {
    DynamicResolution governor;
    governor.configure(config());
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    NF_CHECK_NEAR(governor.update(0.0f), 1.0f, 1e-6f);
    NF_CHECK_NEAR(governor.update(-1.0f), 1.0f, 1e-6f);
    NF_CHECK_NEAR(governor.update(nan), 1.0f, 1e-6f);
    NF_CHECK_NEAR(governor.update(inf), 1.0f, 1e-6f);
    // None of those were samples, so the cooldown still needs real frames.
    NF_CHECK_NEAR(feed(governor, 0.025f, 10), 1.0f, 1e-6f);
}

NF_TEST(dynamic_resolution_caps_a_hitch_before_it_counts_as_load) {
    DynamicResolution governor;
    governor.configure(config());
    // Ten seconds is not ten seconds of evidence. The sample is capped at
    // max(target*4, 0.25 s) = 0.25 s here, so this single freeze cannot both
    // pass the cooldown and poison the average on its own.
    NF_CHECK_NEAR(governor.update(10.0f), 1.0f, 1e-6f);
    // One more 0.25 s of ordinary slow frames reaches the 0.5 s boundary.
    NF_CHECK_NEAR(feed(governor, 0.025f, 11), 0.9f, 1e-6f);
}

NF_TEST(dynamic_resolution_reenabling_starts_at_the_ceiling) {
    DynamicResolution governor;
    governor.configure(config());
    NF_CHECK_NEAR(feed(governor, 0.025f, 25), 0.9f, 1e-6f);

    DynamicResolutionConfig off = governor.config();
    off.enabled = false;
    governor.configure(off);
    off.enabled = true;
    governor.configure(off);
    NF_CHECK_NEAR(governor.scale(), governor.config().max_scale, 1e-6f);
}

NF_TEST(dynamic_resolution_keeps_the_scale_when_new_bounds_still_allow_it) {
    DynamicResolution governor;
    governor.configure(config(16.6667f, 0.5f, 1.0f, 0.1f, 0.2f));
    governor.reset(0.8f);

    DynamicResolutionConfig tighter = governor.config();
    tighter.min_scale = 0.7f;
    tighter.max_scale = 0.95f;
    governor.configure(tighter);
    NF_CHECK_NEAR(governor.scale(), 0.8f, 1e-6f);

    // Moving the ceiling below the current value clamps it, rather than leaving
    // the renderer above a bound the author just set.
    tighter.max_scale = 0.75f;
    governor.configure(tighter);
    NF_CHECK_NEAR(governor.scale(), 0.75f, 1e-6f);
}

NF_TEST(dynamic_resolution_bounds_use_the_render_scale_range) {
    DynamicResolution governor;
    DynamicResolutionConfig c{};
    c.enabled = true;
    c.min_scale = 0.01f;
    c.max_scale = 9.0f;
    governor.configure(c);
    NF_CHECK_NEAR(governor.config().min_scale, kMinRenderScale, 1e-6f);
    NF_CHECK_NEAR(governor.config().max_scale, kMaxRenderScale, 1e-6f);
    // The starting value is the sanitized ceiling, not the raw 9.0.
    NF_CHECK_NEAR(governor.scale(), kMaxRenderScale, 1e-6f);
}
