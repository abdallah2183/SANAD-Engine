// Tests/RHITests/test_frame_pacing_policy.cpp — render scale from the window
// (Phase 42).
//
// The policy exists because an average is the wrong question: a run that is
// usually 60 fps and stutters every second averages fine and plays badly. These
// cases pin the asymmetry that makes it worth having —
//
//   * the 1% LOW decides to go down;
//   * the AVERAGE and the 1% low together decide to come back up;
//   * nothing moves without enough samples, without a finished cooldown, or
//     inside the tolerance band;
//   * the scale never leaves its declared range.
//
// Every case is arithmetic over a FramePacingStats value, so this suite runs
// without a GPU and without a clock.

#include <NF/Rendering/FramePacingPolicy.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <string>

namespace {

using nf::f32;
using nf::u32;
using namespace nf::rendering;

FramePacingStats make_stats(u32 samples, double avg_fps, double low_fps) {
    FramePacingStats s;
    s.samples = samples;
    s.average_fps = avg_fps;
    s.low_1pct_fps = low_fps;
    // The policy reads only the fps fields, but filling the ms ones keeps the
    // fixture honest if it ever starts reading them.
    s.average_frame_ms = avg_fps > 0.0 ? 1000.0 / avg_fps : 0.0;
    s.low_1pct_frame_ms = low_fps > 0.0 ? 1000.0 / low_fps : 0.0;
    return s;
}

FramePolicyConfig config_60() {
    FramePolicyConfig c;
    c.target_fps = 60.0f;
    c.tolerance = 0.10f;
    c.step = 0.05f;
    c.min_scale = 0.5f;
    c.max_scale = 1.0f;
    c.cooldown_seconds = 0.5f;
    c.min_samples = 8;
    return c;
}

} // namespace

NF_TEST(the_policy_holds_before_it_has_a_window) {
    FramePolicyConfig cfg = config_60();
    FramePolicyState state;
    // Seven samples at 20 fps: a terrible window, and still not evidence.
    const FramePolicyResult r = frame_policy_step(make_stats(7, 20.0, 12.0), cfg, state, 1.0f);
    NF_CHECK(!r.changed);
    NF_CHECK(r.reason == FramePolicyReason::HoldNoSamples);
    NF_CHECK_NEAR(r.scale, 1.0f, 1e-6f);

    // Zeroed statistics (a device that reported nothing) must not read as
    // "0 fps, drop everything".
    FramePolicyState zero_state;
    const FramePolicyResult z = frame_policy_step(make_stats(30, 0.0, 0.0), cfg, zero_state, 1.0f);
    NF_CHECK(!z.changed);
    NF_CHECK(z.reason == FramePolicyReason::HoldNoSamples);
}

NF_TEST(a_bad_one_percent_lowers_the_scale_even_when_the_average_is_fine) {
    FramePolicyConfig cfg = config_60();
    FramePolicyState state;
    // 60 fps average, 40 fps 1% low: exactly the stutter the average hides.
    const FramePolicyResult r = frame_policy_step(make_stats(120, 60.0, 40.0), cfg, state, 1.0f);
    NF_CHECK(r.changed);
    NF_CHECK(r.reason == FramePolicyReason::StepDown);
    NF_CHECK_NEAR(r.scale, 0.95f, 1e-6f);
    NF_CHECK_NEAR(state.scale, 0.95f, 1e-6f);
    // The change restarts the cooldown, or the next call would step again
    // immediately and the window that justified this step would be forgotten.
    NF_CHECK_NEAR(state.since_change_seconds, 0.0f, 1e-6f);
}

NF_TEST(recovery_needs_both_the_average_and_the_one_percent) {
    FramePolicyConfig cfg = config_60();
    FramePolicyState state;
    state.scale = 0.8f;
    state.since_change_seconds = 10.0f;

    // Fast average, still-bad 1% low: this is a window that would oscillate if
    // the average alone were allowed to raise the scale.
    const FramePolicyResult hold = frame_policy_step(make_stats(120, 90.0, 55.0), cfg, state, 0.1f);
    NF_CHECK(!hold.changed);
    NF_CHECK(hold.reason == FramePolicyReason::HoldInBand);

    // Both clear the target: now it may climb.
    const FramePolicyResult up = frame_policy_step(make_stats(120, 75.0, 65.0), cfg, state, 0.1f);
    NF_CHECK(up.changed);
    NF_CHECK(up.reason == FramePolicyReason::StepUp);
    NF_CHECK_NEAR(up.scale, 0.85f, 1e-6f);
}

NF_TEST(the_tolerance_band_is_dead_and_the_range_is_respected) {
    FramePolicyConfig cfg = config_60();
    FramePolicyState state;
    state.since_change_seconds = 5.0f;

    // 58 fps average and 56 fps 1% low are both inside 60 +/- 10%: no change.
    const FramePolicyResult in_band = frame_policy_step(make_stats(120, 58.0, 56.0), cfg, state, 0.1f);
    NF_CHECK(!in_band.changed);
    NF_CHECK(in_band.reason == FramePolicyReason::HoldInBand);

    // At the floor a bad window holds instead of going lower, and says so.
    state.scale = cfg.min_scale;
    const FramePolicyResult at_floor =
        frame_policy_step(make_stats(120, 30.0, 20.0), cfg, state, 0.1f);
    NF_CHECK(!at_floor.changed);
    NF_CHECK(at_floor.reason == FramePolicyReason::HoldAtFloor);

    // At the ceiling a good window holds instead of supersampling past max.
    state.scale = cfg.max_scale;
    const FramePolicyResult at_ceiling =
        frame_policy_step(make_stats(120, 120.0, 100.0), cfg, state, 0.1f);
    NF_CHECK(!at_ceiling.changed);
    NF_CHECK(at_ceiling.reason == FramePolicyReason::HoldAtCeiling);
}

NF_TEST(the_cooldown_blocks_a_second_step_and_a_pause_does_not_advance_it) {
    FramePolicyConfig cfg = config_60();
    FramePolicyState state;
    state.since_change_seconds = 10.0f;

    const FramePolicyResult first =
        frame_policy_step(make_stats(120, 60.0, 30.0), cfg, state, 0.1f);
    NF_CHECK(first.changed);

    // Immediately after, the same bad window must NOT step again: that is the
    // oscillation the cooldown exists to prevent.
    const FramePolicyResult second =
        frame_policy_step(make_stats(120, 60.0, 30.0), cfg, state, 0.1f);
    NF_CHECK(!second.changed);
    NF_CHECK(second.reason == FramePolicyReason::HoldCooldown);

    // A paused frame advances nothing, so it cannot expire the cooldown.
    const f32 before = state.since_change_seconds;
    frame_policy_step(make_stats(120, 60.0, 30.0), cfg, state, 0.0f);
    NF_CHECK_NEAR(state.since_change_seconds, before, 1e-6f);

    // Enough wall time and the policy may act again.
    const FramePolicyResult third =
        frame_policy_step(make_stats(120, 60.0, 30.0), cfg, state, 1.0f);
    NF_CHECK(third.changed);
    NF_CHECK_NEAR(third.scale, 0.90f, 1e-6f);
}

NF_TEST(policy_configs_are_sanitized_rather_than_trusted) {
    FramePolicyConfig cfg;
    cfg.target_fps = std::nanf("");
    cfg.tolerance = 5.0f;      // absurd, must clamp
    cfg.step = -1.0f;          // absurd, must fall back
    cfg.min_scale = 1.4f;      // inverted pair, must swap
    cfg.max_scale = 0.6f;
    cfg.cooldown_seconds = -3.0f;

    FramePolicyState state;
    state.scale = 1.0f;
    const FramePolicyResult r = frame_policy_step(make_stats(120, 60.0, 60.0), cfg, state, 1.0f);
    // Sanitized: target 60, tolerance 0.5, step 0.05, range [0.6, 1.4],
    // cooldown 0.5. A 60/60 window sits inside the band, so it holds.
    NF_CHECK(!r.changed);
    NF_CHECK(r.reason == FramePolicyReason::HoldInBand);
    NF_CHECK(r.scale >= 0.6f && r.scale <= 1.4f);
}

NF_TEST(reason_names_are_stable_for_logs) {
    NF_CHECK(std::string(frame_policy_reason_name(FramePolicyReason::StepDown)) == "step-down");
    NF_CHECK(std::string(frame_policy_reason_name(FramePolicyReason::StepUp)) == "step-up");
    NF_CHECK(std::string(frame_policy_reason_name(FramePolicyReason::HoldCooldown)) ==
             "hold-cooldown");
    NF_CHECK(std::string(frame_policy_reason_name(FramePolicyReason::HoldAtFloor)) ==
             "hold-at-floor");
}
