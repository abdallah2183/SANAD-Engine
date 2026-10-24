// AudioTests — master-bus dynamics.
//
// The contract this file pins:
//
//   1. both stages OFF is a bit-exact passthrough (the author's master);
//   2. the compressor's static curve (threshold / ratio / soft knee) is
//      arithmetic, not a vibe, and the detector is stereo-linked;
//   3. attack and release are measured, and the times do not change when the
//      mixer's block size changes;
//   4. the limiter is a brickwall: no output sample ever exceeds the ceiling,
//      it reports the latency it adds, and the latency is real;
//   5. non-finite settings cannot poison the device buffer;
//   6. an AudioScene really applies the stages to its own mix and leaves the
//      caller's base mix alone.

#include <NF/Test/TestFramework.hpp>
#include <NF/Audio/AudioScene.hpp>
#include <NF/Audio/Dynamics.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace nf;
using namespace nf::audio;

namespace {

constexpr f32 kEps = 1e-5f;

f32 db_to_linear(f32 db) {
    return std::pow(10.0f, db * 0.05f);
}

std::vector<f32> constant_signal(f32 value, usize frames) {
    return std::vector<f32>(frames, value);
}

void check_finite(const std::vector<f32>& samples) {
    for (f32 v : samples) {
        NF_CHECK(std::isfinite(v));
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Neutral settings
// ---------------------------------------------------------------------------

NF_TEST(dynamics_disabled_is_a_bit_exact_passthrough) {
    MasterDynamics dyn;
    const std::vector<f32> original_left = {0.0f, 1.0f, -0.25f, 0.125f, -1.0f, 3.4e-5f};
    const std::vector<f32> original_right = {0.5f, -0.5f, 0.0f, -0.75f, 0.333333f, -2.2e-5f};
    std::vector<f32> left = original_left;
    std::vector<f32> right = original_right;

    dyn.process(left.data(), right.data(), left.size(), 48000);

    for (usize i = 0; i < left.size(); ++i) {
        NF_CHECK(left[i] == original_left[i]);
        NF_CHECK(right[i] == original_right[i]);
    }
    NF_CHECK_NEAR(dyn.compressor_gain_reduction_db(), 0.0f, 1e-9f);
    NF_CHECK_NEAR(dyn.limiter_gain_reduction_db(), 0.0f, 1e-9f);
}

NF_TEST(dynamics_compressor_at_unity_ratio_and_zero_makeup_is_identity) {
    MasterDynamics dyn;
    MasterDynamicsSettings settings;
    settings.compressor.enabled = true;
    settings.compressor.threshold_db = -40.0f;
    settings.compressor.ratio = 1.0f;
    settings.compressor.attack_ms = 0.0f;
    settings.compressor.release_ms = 0.0f;
    settings.compressor.knee_db = 0.0f;
    settings.compressor.makeup_db = 0.0f;
    dyn.set_settings(settings);

    std::vector<f32> left = {1.0f, -1.0f, 0.5f, -0.25f};
    std::vector<f32> right = {-1.0f, 1.0f, -0.5f, 0.25f};
    const auto original_left = left;
    const auto original_right = right;

    dyn.process(left.data(), right.data(), left.size(), 48000);

    for (usize i = 0; i < left.size(); ++i) {
        NF_CHECK(left[i] == original_left[i]);
        NF_CHECK(right[i] == original_right[i]);
    }
    NF_CHECK_NEAR(dyn.compressor_gain_reduction_db(), 0.0f, 1e-9f);
}

// ---------------------------------------------------------------------------
// Compressor curve
// ---------------------------------------------------------------------------

NF_TEST(dynamics_compressor_threshold_ratio_and_soft_knee_follow_the_curve) {
    MasterDynamicsSettings settings;
    settings.compressor.enabled = true;
    settings.compressor.threshold_db = -12.0f;
    settings.compressor.ratio = 4.0f;
    settings.compressor.attack_ms = 0.0f;
    settings.compressor.release_ms = 0.0f;
    settings.compressor.knee_db = 0.0f;
    settings.compressor.makeup_db = 0.0f;

    // Below the threshold: the detector should not turn the gain at all.
    {
        MasterDynamics dyn;
        dyn.set_settings(settings);
        std::vector<f32> left = constant_signal(0.25f, 8); // -12.04 dBFS
        std::vector<f32> right = constant_signal(0.25f, 8);
        dyn.process(left.data(), right.data(), left.size(), 48000);
        for (f32 v : left) {
            NF_CHECK_NEAR(v, 0.25f, kEps);
        }
        NF_CHECK_NEAR(dyn.compressor_gain_reduction_db(), 0.0f, kEps);
    }

    // Above the threshold: reduction = (level - threshold) * (1 - 1/ratio).
    {
        MasterDynamics dyn;
        dyn.set_settings(settings);
        std::vector<f32> left = constant_signal(0.5f, 8); // -6.02 dBFS
        std::vector<f32> right = constant_signal(0.5f, 8);
        dyn.process(left.data(), right.data(), left.size(), 48000);

        const f32 level_db = 20.0f * std::log10(0.5f);
        const f32 expected_gr = (level_db + 12.0f) * 0.75f;
        const f32 expected = 0.5f * db_to_linear(-expected_gr);
        NF_CHECK_NEAR(dyn.compressor_gain_reduction_db(), expected_gr, 1e-4f);
        for (f32 v : left) {
            NF_CHECK_NEAR(v, expected, 1e-4f);
        }
    }

    // A soft knee starts reducing before the threshold. Exactly AT the
    // threshold the quadratic curve gives slope * knee / 8, not zero, which is
    // the whole point of a knee: no audible corner to hear.
    {
        MasterDynamicsSettings soft = settings;
        soft.compressor.threshold_db = -20.0f;
        soft.compressor.ratio = 5.0f;
        soft.compressor.knee_db = 10.0f;
        MasterDynamics dyn;
        dyn.set_settings(soft);
        std::vector<f32> left = constant_signal(0.1f, 8); // -20 dBFS
        std::vector<f32> right = constant_signal(0.1f, 8);
        dyn.process(left.data(), right.data(), left.size(), 48000);
        const f32 slope = 1.0f - 1.0f / 5.0f;
        const f32 expected_gr = slope * 10.0f / 8.0f;
        NF_CHECK_NEAR(dyn.compressor_gain_reduction_db(), expected_gr, 1e-4f);
    }
}

NF_TEST(dynamics_compressor_detector_is_stereo_linked) {
    MasterDynamicsSettings settings;
    settings.compressor.enabled = true;
    settings.compressor.threshold_db = -12.0f;
    settings.compressor.ratio = 4.0f;
    settings.compressor.attack_ms = 0.0f;
    settings.compressor.release_ms = 0.0f;
    settings.compressor.knee_db = 0.0f;
    settings.compressor.makeup_db = 0.0f;

    MasterDynamics dyn;
    dyn.set_settings(settings);
    std::vector<f32> left(16, 1.0f);  // full scale
    std::vector<f32> right(16, 0.1f); // -20 dBFS, below the threshold alone
    dyn.process(left.data(), right.data(), left.size(), 48000);

    // The loud channel sets the detector, and BOTH channels get the same gain.
    // A per-channel detector would leave `right` at 0.1 and walk the image.
    const f32 expected_gr = (0.0f + 12.0f) * 0.75f;
    const f32 expected_gain = db_to_linear(-expected_gr);
    NF_CHECK_NEAR(dyn.compressor_gain_reduction_db(), expected_gr, 1e-4f);
    NF_CHECK_NEAR(left[8], expected_gain, 1e-4f);
    NF_CHECK_NEAR(right[8], 0.1f * expected_gain, 1e-4f);
    NF_CHECK(right[8] < 0.1f);
}

NF_TEST(dynamics_compressor_attack_and_release_use_the_configured_times) {
    MasterDynamicsSettings settings;
    settings.compressor.enabled = true;
    settings.compressor.threshold_db = -20.0f;
    settings.compressor.ratio = 100.0f; // slope 0.99, target reduction 19.8 dB
    settings.compressor.attack_ms = 10.0f;
    settings.compressor.release_ms = 100.0f;
    settings.compressor.knee_db = 0.0f;
    settings.compressor.makeup_db = 0.0f;

    MasterDynamics dyn;
    dyn.set_settings(settings);

    std::vector<f32> left = constant_signal(1.0f, 1);
    std::vector<f32> right = constant_signal(1.0f, 1);
    dyn.process(left.data(), right.data(), left.size(), 1000);

    const f32 target = 19.8f;
    const f32 attack_coefficient = std::exp(-0.1f); // 10 ms at 1 kHz
    const f32 expected_first = target * (1.0f - attack_coefficient);
    NF_CHECK_NEAR(dyn.compressor_gain_reduction_db(), expected_first, 1e-4f);
    NF_CHECK_NEAR(left[0], db_to_linear(-expected_first), 1e-4f);

    // Ten time constants: the envelope has effectively arrived.
    left.assign(100, 1.0f);
    right.assign(100, 1.0f);
    dyn.process(left.data(), right.data(), left.size(), 1000);
    NF_CHECK_NEAR(dyn.compressor_gain_reduction_db(), target, 0.01f);

    // Silence: the envelope lets go at the release time, not at the attack
    // time. One release constant leaves 63% of the reduction.
    left.assign(1, 0.0f);
    right.assign(1, 0.0f);
    const f32 before = dyn.compressor_gain_reduction_db();
    dyn.process(left.data(), right.data(), left.size(), 1000);
    const f32 release_coefficient = std::exp(-0.01f); // 100 ms at 1 kHz
    NF_CHECK_NEAR(dyn.compressor_gain_reduction_db(), before * release_coefficient, 0.01f);
}

NF_TEST(dynamics_attack_time_is_the_same_at_every_sample_rate_and_block_size) {
    MasterDynamicsSettings settings;
    settings.compressor.enabled = true;
    settings.compressor.threshold_db = -20.0f;
    settings.compressor.ratio = 100.0f;
    settings.compressor.attack_ms = 10.0f;
    settings.compressor.release_ms = 100.0f;
    settings.compressor.knee_db = 0.0f;
    settings.compressor.makeup_db = 0.0f;

    auto reduction_after_ten_ms = [&](u32 rate, usize block) {
        MasterDynamics dyn;
        dyn.set_settings(settings);
        const usize frames = rate / 100; // exactly 10 ms
        std::vector<f32> left(frames, 1.0f);
        std::vector<f32> right(frames, 1.0f);
        usize done = 0;
        while (done < frames) {
            const usize n = std::min(block, frames - done);
            dyn.process(left.data() + done, right.data() + done, n, rate);
            done += n;
        }
        return dyn.compressor_gain_reduction_db();
    };

    const f32 at_1k = reduction_after_ten_ms(1000, 1000);
    const f32 at_48k = reduction_after_ten_ms(48000, 43);
    const f32 at_96k = reduction_after_ten_ms(96000, 1024);
    // One attack time reaches 1 - 1/e of the target (19.8 dB -> 12.51 dB).
    NF_CHECK_NEAR(at_1k, 12.5134f, 0.02f);
    NF_CHECK_NEAR(at_48k, at_1k, 0.02f);
    NF_CHECK_NEAR(at_96k, at_1k, 0.02f);

    // Splitting the block must not change a single envelope step.
    auto split = [&](usize block) {
        MasterDynamics dyn;
        dyn.set_settings(settings);
        std::vector<f32> left(600, 1.0f);
        std::vector<f32> right(600, 1.0f);
        usize done = 0;
        while (done < left.size()) {
            const usize n = std::min(block, left.size() - done);
            dyn.process(left.data() + done, right.data() + done, n, 48000);
            done += n;
        }
        return std::pair<f32, std::vector<f32>>{
            dyn.compressor_gain_reduction_db(), left};
    };
    const auto one = split(600);
    const auto chunks = split(37);
    NF_CHECK_NEAR(one.first, chunks.first, 1e-5f);
    for (usize i = 0; i < one.second.size(); ++i) {
        NF_CHECK_NEAR(one.second[i], chunks.second[i], 1e-5f);
    }
}

// ---------------------------------------------------------------------------
// Limiter
// ---------------------------------------------------------------------------

NF_TEST(dynamics_limiter_is_a_brickwall_and_reports_its_latency) {
    MasterDynamicsSettings settings;
    settings.limiter.enabled = true;
    settings.limiter.ceiling_db = -6.0f;
    settings.limiter.release_ms = 80.0f;
    settings.limiter.lookahead_ms = 2.0f; // 2 samples at 1 kHz

    MasterDynamics dyn;
    dyn.set_settings(settings);

    std::vector<f32> left(12, 0.0f);
    std::vector<f32> right(12, 0.0f);
    left[5] = 1.0f;
    right[7] = -1.0f;
    dyn.process(left.data(), right.data(), left.size(), 1000);

    const f32 ceiling = db_to_linear(-6.0f);
    NF_CHECK_EQ(dyn.limiter_delay_samples(), 2u);

    // The impulse entered at frame 5, so it cannot leave before frame 7. The
    // latency is the delay line, not a comment.
    for (usize i = 0; i < 7; ++i) {
        NF_CHECK_NEAR(left[i], 0.0f, 1e-7f);
    }
    NF_CHECK_NEAR(left[7], ceiling, 1e-5f);
    NF_CHECK_NEAR(right[9], -ceiling, 1e-5f);
    for (usize i = 0; i < left.size(); ++i) {
        NF_CHECK(std::fabs(left[i]) <= ceiling + 1e-6f);
        NF_CHECK(std::fabs(right[i]) <= ceiling + 1e-6f);
    }
    NF_CHECK(dyn.limiter_gain_reduction_db() > 0.0f);
}

NF_TEST(dynamics_limiter_with_no_lookahead_still_never_overshoots) {
    MasterDynamicsSettings settings;
    settings.limiter.enabled = true;
    settings.limiter.ceiling_db = -6.0f;
    settings.limiter.release_ms = 0.0f;
    settings.limiter.lookahead_ms = 0.0f;

    MasterDynamics dyn;
    dyn.set_settings(settings);
    std::vector<f32> left = {0.25f, 1.0f, -1.5f, 0.0f, 0.75f, -0.75f};
    std::vector<f32> right = {-0.25f, -1.0f, 1.5f, 0.0f, -0.75f, 0.75f};
    dyn.process(left.data(), right.data(), left.size(), 48000);

    const f32 ceiling = db_to_linear(-6.0f);
    NF_CHECK_EQ(dyn.limiter_delay_samples(), 0u);
    for (usize i = 0; i < left.size(); ++i) {
        NF_CHECK(std::fabs(left[i]) <= ceiling + 1e-6f);
        NF_CHECK(std::fabs(right[i]) <= ceiling + 1e-6f);
    }
    // 1.5 at full scale must be turned down, not clipped: the waveform keeps
    // its shape (the two channels remain exact negatives).
    for (usize i = 0; i < left.size(); ++i) {
        NF_CHECK_NEAR(left[i], -right[i], 1e-6f);
    }
}

NF_TEST(dynamics_limiter_releases_but_leaves_the_ceiling_intact) {
    MasterDynamicsSettings settings;
    settings.limiter.enabled = true;
    settings.limiter.ceiling_db = -6.0f;
    settings.limiter.release_ms = 50.0f;
    settings.limiter.lookahead_ms = 0.0f;

    MasterDynamics dyn;
    dyn.set_settings(settings);

    std::vector<f32> left;
    std::vector<f32> right;
    for (usize i = 0; i < 5; ++i) {
        left.push_back(1.0f);
        right.push_back(1.0f);
    }
    for (usize i = 0; i < 500; ++i) {
        left.push_back(0.25f);
        right.push_back(0.25f);
    }
    dyn.process(left.data(), right.data(), left.size(), 1000);

    // The quiet tail is below the ceiling and must come back to its original
    // level as the gain is released; it must NOT stay squashed at 0.501.
    NF_CHECK_NEAR(left.back(), 0.25f, 1e-3f);
    NF_CHECK_NEAR(dyn.limiter_gain_reduction_db(), 0.0f, 0.01f);
    for (f32 v : left) {
        NF_CHECK(std::fabs(v) <= db_to_linear(-6.0f) + 1e-6f);
    }
}

NF_TEST(dynamics_limiter_is_stereo_linked_and_delays_both_channels) {
    MasterDynamicsSettings settings;
    settings.limiter.enabled = true;
    settings.limiter.ceiling_db = -6.0f;
    settings.limiter.release_ms = 80.0f;
    settings.limiter.lookahead_ms = 1.0f;

    MasterDynamics dyn;
    dyn.set_settings(settings);
    std::vector<f32> left(8, 0.0f);
    std::vector<f32> right(8, 0.0f);
    left[3] = 1.0f;
    right[3] = 0.1f;
    dyn.process(left.data(), right.data(), left.size(), 1000);

    NF_CHECK_EQ(dyn.limiter_delay_samples(), 1u);
    const f32 ceiling = db_to_linear(-6.0f);
    NF_CHECK_NEAR(left[4], ceiling, 1e-5f);
    // The quiet channel is turned down by the SAME gain, preserving 10:1.
    NF_CHECK_NEAR(right[4], 0.1f * ceiling, 1e-5f);
}

// ---------------------------------------------------------------------------
// Hostile settings
// ---------------------------------------------------------------------------

NF_TEST(dynamics_non_finite_settings_are_neutralised_not_propagated) {
    MasterDynamicsSettings settings;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    settings.compressor.enabled = true;
    settings.compressor.threshold_db = nan;
    settings.compressor.ratio = nan;
    settings.compressor.attack_ms = inf;
    settings.compressor.release_ms = -inf;
    settings.compressor.knee_db = nan;
    settings.compressor.makeup_db = inf;
    settings.limiter.enabled = true;
    settings.limiter.ceiling_db = nan;
    settings.limiter.release_ms = inf;
    settings.limiter.lookahead_ms = nan;

    MasterDynamics dyn;
    dyn.set_settings(settings);
    const std::vector<f32> original_left = {0.5f, -0.5f, 0.25f, 0.0f, 1.0f};
    const std::vector<f32> original_right = {-0.5f, 0.5f, -0.25f, 0.0f, -1.0f};
    std::vector<f32> left = original_left;
    std::vector<f32> right = original_right;
    dyn.process(left.data(), right.data(), left.size(), 0); // 0 falls back to 48 kHz

    check_finite(left);
    check_finite(right);
    // Every broken control degraded to neutral: unity ratio, 0 dB ceiling,
    // 0 ms lookahead. A finite signal therefore emerges unchanged.
    for (usize i = 0; i < left.size(); ++i) {
        NF_CHECK(left[i] == original_left[i]);
        NF_CHECK(right[i] == original_right[i]);
    }
    NF_CHECK_EQ(dyn.limiter_delay_samples(), 0u);
}

NF_TEST(dynamics_reset_clears_the_envelopes_and_delay) {
    MasterDynamicsSettings settings;
    settings.compressor.enabled = true;
    settings.compressor.threshold_db = -20.0f;
    settings.compressor.ratio = 10.0f;
    settings.compressor.attack_ms = 0.0f;
    settings.compressor.release_ms = 0.0f;
    settings.limiter.enabled = true;
    settings.limiter.ceiling_db = -6.0f;
    settings.limiter.lookahead_ms = 2.0f;

    MasterDynamics dyn;
    dyn.set_settings(settings);
    std::vector<f32> left(16, 1.0f);
    std::vector<f32> right(16, 1.0f);
    dyn.process(left.data(), right.data(), left.size(), 1000);
    NF_CHECK(dyn.compressor_gain_reduction_db() > 0.0f);

    dyn.reset();
    NF_CHECK_NEAR(dyn.compressor_gain_reduction_db(), 0.0f, 1e-9f);
    NF_CHECK_NEAR(dyn.limiter_gain_reduction_db(), 0.0f, 1e-9f);
    // Reset keeps the configuration (the latency does not change) but clears
    // the delay line: the next block starts with two zero samples, not audio
    // from before the scene change. Disable the compressor so the check below
    // measures the limiter's delay rather than the compressor's gain.
    settings.compressor.enabled = false;
    dyn.set_settings(settings);
    NF_CHECK_EQ(dyn.limiter_delay_samples(), 2u);
    std::vector<f32> after_left(4, 0.5f);
    std::vector<f32> after_right(4, 0.5f);
    dyn.process(after_left.data(), after_right.data(), after_left.size(), 1000);
    NF_CHECK_NEAR(after_left[0], 0.0f, 1e-7f);
    NF_CHECK_NEAR(after_left[1], 0.0f, 1e-7f);
    NF_CHECK_NEAR(after_left[2], 0.5f, 1e-5f);
}

// ---------------------------------------------------------------------------
// AudioScene integration
// ---------------------------------------------------------------------------

NF_TEST(scene_applies_master_dynamics_to_its_own_mix_only) {
    constexpr usize frames = 64;
    const AudioBuffer source = [frames] {
        AudioBuffer b;
        b.channels = 1;
        b.sample_rate = 48000;
        b.samples.assign(frames, 1.0f);
        return b;
    }();

    AudioListener listener;
    listener.position = {0, 0, 0};
    listener.forward = {0, 0, -1};
    listener.up = {0, 1, 0};

    Emitter emitter;
    emitter.buffer = &source;
    emitter.playing = true;
    emitter.looping = true;
    emitter.bus = BusId::Sfx;

    // 1. Disabled: the scene's output is exactly the bus sum (1.0).
    {
        AudioScene scene;
        scene.set_listener(listener);
        emitter.sample_cursor = 0;
        emitter.playing = true;
        scene.begin_block(frames, 48000);
        scene.mix_emitter(emitter);
        std::vector<f32> left(frames, 0.0f);
        std::vector<f32> right(frames, 0.0f);
        scene.finalize(left.data(), right.data());
        for (f32 v : left) {
            NF_CHECK_NEAR(v, 1.0f, 1e-6f);
        }
    }

    // 2. Compressor only. The full-scale bus is brought down by the curve.
    {
        AudioScene scene;
        scene.set_listener(listener);
        MasterDynamicsSettings dyn;
        dyn.compressor.enabled = true;
        dyn.compressor.threshold_db = -20.0f;
        dyn.compressor.ratio = 100.0f;
        dyn.compressor.attack_ms = 0.0f;
        dyn.compressor.release_ms = 0.0f;
        dyn.compressor.knee_db = 0.0f;
        scene.set_dynamics(dyn);
        emitter.sample_cursor = 0;
        emitter.playing = true;
        scene.begin_block(frames, 48000);
        scene.mix_emitter(emitter);
        std::vector<f32> left(frames, 0.0f);
        std::vector<f32> right(frames, 0.0f);
        scene.finalize(left.data(), right.data());
        const f32 expected = db_to_linear(-19.8f);
        for (f32 v : left) {
            NF_CHECK_NEAR(v, expected, 1e-4f);
        }
        NF_CHECK_NEAR(scene.dynamics().compressor_gain_reduction_db(), 19.8f, 1e-3f);
    }

    // 3. Limiter only. The scene's full-scale signal hits a -6 dB brickwall.
    {
        AudioScene scene;
        scene.set_listener(listener);
        MasterDynamicsSettings dyn;
        dyn.limiter.enabled = true;
        dyn.limiter.ceiling_db = -6.0f;
        dyn.limiter.release_ms = 80.0f;
        dyn.limiter.lookahead_ms = 0.0f;
        scene.set_dynamics(dyn);
        emitter.sample_cursor = 0;
        emitter.playing = true;
        scene.begin_block(frames, 48000);
        scene.mix_emitter(emitter);
        std::vector<f32> left(frames, 0.0f);
        std::vector<f32> right(frames, 0.0f);
        // The caller primed the buffer with its own base mix: dynamics must
        // process the scene's contribution, not this 0.1.
        std::fill(left.begin(), left.end(), 0.1f);
        std::fill(right.begin(), right.end(), 0.1f);
        scene.finalize(left.data(), right.data());
        const f32 ceiling = db_to_linear(-6.0f);
        for (f32 v : left) {
            NF_CHECK_NEAR(v, 0.1f + ceiling, 1e-5f);
        }
        NF_CHECK_EQ(scene.dynamics().limiter_delay_samples(), 0u);
    }
}
