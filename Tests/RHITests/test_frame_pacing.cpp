// Tests/RHITests/test_frame_pacing.cpp — rolling frame-time telemetry (Phase
// 41).
//
// This suite is GPU-free on purpose. FramePacing is a pure window over numbers:
// the average, the 1% low, the variance, the ring wrap, and the CPU/GPU verdict
// are all exact arithmetic. The GPU integration is proven by the player's
// windowed acceptance run, not here.

#include <NF/Rendering/FramePacing.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <limits>
#include <string>

namespace {

using nf::f64;
using nf::u32;
using namespace nf::rendering;

void feed(FramePacing& pacing, f64 frame_ms, int count) {
    for (int i = 0; i < count; ++i) {
        pacing.push_frame(frame_ms / 1000.0);
    }
}

} // namespace

NF_TEST(frame_pacing_empty_window_is_unknown) {
    FramePacing pacing;
    const FramePacingStats stats = pacing.stats();
    NF_CHECK(stats.samples == 0u);
    NF_CHECK(stats.gpu_samples == 0u);
    NF_CHECK_NEAR(stats.average_frame_ms, 0.0, 1e-9);
    NF_CHECK_NEAR(stats.average_fps, 0.0, 1e-9);
    NF_CHECK_NEAR(stats.low_1pct_frame_ms, 0.0, 1e-9);
    NF_CHECK(stats.bound == FramePacingBound::Unknown);
}

NF_TEST(frame_pacing_average_and_one_percent_low_are_exact) {
    // 10 frames: five at 10 ms and five at 30 ms. Mean = 20 ms (50 fps), and
    // one 1% slot at this window size is the single slowest frame.
    FramePacing pacing(10);
    feed(pacing, 10.0, 5);
    feed(pacing, 30.0, 5);

    const FramePacingStats stats = pacing.stats();
    NF_CHECK(stats.samples == 10u);
    NF_CHECK_NEAR(stats.average_frame_ms, 20.0, 1e-9);
    NF_CHECK_NEAR(stats.average_fps, 50.0, 1e-9);
    NF_CHECK_NEAR(stats.low_1pct_frame_ms, 30.0, 1e-9);
    NF_CHECK_NEAR(stats.low_1pct_fps, 1000.0 / 30.0, 1e-9);
    NF_CHECK_NEAR(stats.min_frame_ms, 10.0, 1e-9);
    NF_CHECK_NEAR(stats.max_frame_ms, 30.0, 1e-9);
    // Population variance of five 10s and five 30s around 20 is 100 ms².
    NF_CHECK_NEAR(stats.variance_ms2, 100.0, 1e-9);
    NF_CHECK_NEAR(stats.stddev_ms, 10.0, 1e-9);
    NF_CHECK(stats.gpu_samples == 0u);
    NF_CHECK(stats.bound == FramePacingBound::Unknown);
}

NF_TEST(frame_pacing_one_percent_low_averages_the_slowest_slice) {
    // 120 frames means ceil(120/100) = 2 slow frames. The low is their mean,
    // not the single worst frame.
    FramePacing pacing(120);
    feed(pacing, 10.0, 118);
    feed(pacing, 40.0, 1);
    feed(pacing, 60.0, 1);

    const FramePacingStats stats = pacing.stats();
    NF_CHECK(stats.samples == 120u);
    NF_CHECK_NEAR(stats.low_1pct_frame_ms, 50.0, 1e-9);
    NF_CHECK_NEAR(stats.low_1pct_fps, 20.0, 1e-9);

    // At exactly 100 frames the slice is one frame, so the same measurements
    // produce a different low. This is the documented integer ceiling.
    FramePacing hundred(100);
    feed(hundred, 10.0, 98);
    feed(hundred, 40.0, 1);
    feed(hundred, 60.0, 1);
    const FramePacingStats hundred_stats = hundred.stats();
    NF_CHECK_NEAR(hundred_stats.low_1pct_frame_ms, 60.0, 1e-9);
}

NF_TEST(frame_pacing_ring_drops_the_oldest_frames) {
    FramePacing pacing(8);
    feed(pacing, 10.0, 8);
    NF_CHECK_NEAR(pacing.stats().average_frame_ms, 10.0, 1e-9);

    feed(pacing, 20.0, 8);
    const FramePacingStats stats = pacing.stats();
    NF_CHECK(stats.samples == 8u);
    NF_CHECK_NEAR(stats.average_frame_ms, 20.0, 1e-9);
    NF_CHECK_NEAR(stats.min_frame_ms, 20.0, 1e-9);
    NF_CHECK_NEAR(stats.max_frame_ms, 20.0, 1e-9);
}

NF_TEST(frame_pacing_rejects_invalid_cpu_samples) {
    FramePacing pacing(8);
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();
    const f64 inf = std::numeric_limits<f64>::infinity();
    pacing.push_frame(nan);
    pacing.push_frame(inf);
    pacing.push_frame(0.0);
    pacing.push_frame(-1.0);
    NF_CHECK(pacing.stats().samples == 0u);

    pacing.push_frame(0.016);
    NF_CHECK(pacing.stats().samples == 1u);
}

NF_TEST(frame_pacing_requires_enough_gpu_samples_for_a_bound) {
    FramePacing pacing(16);
    // Seven samples look GPU-bound, but one late timestamp is not evidence.
    for (u32 i = 0; i < 7; ++i) {
        pacing.push_frame(0.016, 0.0155, true);
    }
    NF_CHECK(pacing.stats().gpu_samples == 7u);
    NF_CHECK(pacing.stats().bound == FramePacingBound::Unknown);

    pacing.push_frame(0.016, 0.0155, true);
    const FramePacingStats stats = pacing.stats();
    NF_CHECK(stats.gpu_samples == 8u);
    NF_CHECK_NEAR(stats.gpu_average_ms, 15.5, 1e-9);
    NF_CHECK(stats.bound == FramePacingBound::Gpu);
}

NF_TEST(frame_pacing_classifies_cpu_gpu_and_balanced) {
    FramePacing cpu(8);
    for (u32 i = 0; i < 8; ++i) {
        cpu.push_frame(0.016, 0.004, true);
    }
    NF_CHECK(cpu.stats().bound == FramePacingBound::Cpu);

    FramePacing gpu(8);
    for (u32 i = 0; i < 8; ++i) {
        gpu.push_frame(0.016, 0.015, true);
    }
    NF_CHECK(gpu.stats().bound == FramePacingBound::Gpu);

    FramePacing balanced(8);
    for (u32 i = 0; i < 8; ++i) {
        balanced.push_frame(0.020, 0.017, true);
    }
    NF_CHECK(balanced.stats().bound == FramePacingBound::Balanced);
}

NF_TEST(frame_pacing_gpu_samples_age_out_with_the_window) {
    FramePacing pacing(8);
    for (u32 i = 0; i < 8; ++i) {
        pacing.push_frame(0.016, 0.015, true);
    }
    NF_CHECK(pacing.stats().gpu_samples == 8u);
    NF_CHECK(pacing.stats().bound == FramePacingBound::Gpu);

    // No new timestamps: after one full window the old samples are gone rather
    // than pinning a stale GPU-bound verdict forever.
    feed(pacing, 16.0, 8);
    const FramePacingStats stats = pacing.stats();
    NF_CHECK(stats.gpu_samples == 0u);
    NF_CHECK_NEAR(stats.gpu_average_ms, 0.0, 1e-9);
    NF_CHECK(stats.bound == FramePacingBound::Unknown);
}

NF_TEST(frame_pacing_ignores_invalid_gpu_samples) {
    FramePacing pacing(8);
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();
    for (u32 i = 0; i < 8; ++i) {
        pacing.push_frame(0.016, nan, true);
        pacing.push_frame(0.016, 0.0, true);
        pacing.push_frame(0.016, -0.010, true);
        pacing.push_frame(0.016, 0.010, false);
    }
    NF_CHECK(pacing.stats().gpu_samples == 0u);
    NF_CHECK(pacing.stats().bound == FramePacingBound::Unknown);
}

NF_TEST(frame_pacing_can_change_window_and_clear_history) {
    FramePacing pacing(16);
    feed(pacing, 16.0, 16);
    NF_CHECK(pacing.stats().samples == 16u);

    pacing.set_window(32);
    NF_CHECK(pacing.window() == 32u);
    NF_CHECK(pacing.stats().samples == 0u);

    // Clamps, rather than allocating an unbounded history.
    pacing.set_window(0);
    NF_CHECK(pacing.window() == FramePacing::kMinWindow);
    pacing.set_window(100000);
    NF_CHECK(pacing.window() == FramePacing::kMaxWindow);
}

NF_TEST(frame_pacing_reset_forgets_cpu_and_gpu_history) {
    FramePacing pacing(8);
    for (u32 i = 0; i < 8; ++i) {
        pacing.push_frame(0.016, 0.015, true);
    }
    NF_CHECK(pacing.stats().samples == 8u);
    NF_CHECK(pacing.stats().gpu_samples == 8u);

    pacing.reset();
    const FramePacingStats stats = pacing.stats();
    NF_CHECK(stats.samples == 0u);
    NF_CHECK(stats.gpu_samples == 0u);
    NF_CHECK(stats.bound == FramePacingBound::Unknown);
}

NF_TEST(frame_pacing_bound_names_are_stable_for_logs) {
    NF_CHECK(std::string(frame_pacing_bound_name(FramePacingBound::Cpu)) == "cpu");
    NF_CHECK(std::string(frame_pacing_bound_name(FramePacingBound::Gpu)) == "gpu");
    NF_CHECK(std::string(frame_pacing_bound_name(FramePacingBound::Balanced)) == "balanced");
    NF_CHECK(std::string(frame_pacing_bound_name(FramePacingBound::Unknown)) == "unknown");
    NF_CHECK(std::string(frame_pacing_bound_name(static_cast<FramePacingBound>(99))) == "unknown");
}
