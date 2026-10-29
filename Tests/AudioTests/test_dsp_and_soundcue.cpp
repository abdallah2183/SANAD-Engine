#include <NF/Test/TestFramework.hpp>
#include <NF/Audio/AudioDSP.hpp>
#include <NF/Audio/SoundCue.hpp>
#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::audio;

NF_TEST(biquad_lowpass_attenuation_and_dc_unity) {
    const f32 sample_rate = 44100.0f;
    const f32 cutoff = 500.0f;
    BiquadFilter lpf(FilterType::LowPass, cutoff, sample_rate, 0.7071f);

    // 1. DC signal (constant 1.0) should settle to 1.0 (unity gain)
    f32 out = 0.0f;
    for (int i = 0; i < 500; ++i) {
        out = lpf.process_sample(1.0f);
    }
    NF_CHECK_NEAR(out, 1.0f, 0.01f);

    // 2. Reset and test High frequency signal (alternating +1, -1 at Nyquist 22050 Hz)
    lpf.reset();
    f32 hf_max = 0.0f;
    for (int i = 0; i < 200; ++i) {
        const f32 in = (i % 2 == 0) ? 1.0f : -1.0f;
        out = lpf.process_sample(in);
        if (i > 50) {
            hf_max = std::max(hf_max, std::abs(out));
        }
    }
    // High frequency should be heavily attenuated (< 0.05)
    NF_CHECK(hf_max < 0.05f);
}

NF_TEST(biquad_highpass_blocks_dc_and_passes_high) {
    const f32 sample_rate = 44100.0f;
    const f32 cutoff = 2000.0f;
    BiquadFilter hpf(FilterType::HighPass, cutoff, sample_rate, 0.7071f);

    // 1. DC signal (constant 1.0) should be blocked (settle near 0)
    f32 out = 0.0f;
    for (int i = 0; i < 500; ++i) {
        out = hpf.process_sample(1.0f);
    }
    NF_CHECK_NEAR(out, 0.0f, 0.01f);

    // 2. High frequency signal (alternating +1, -1 at 22050 Hz) should pass through
    hpf.reset();
    f32 hf_amp = 0.0f;
    for (int i = 0; i < 200; ++i) {
        const f32 in = (i % 2 == 0) ? 1.0f : -1.0f;
        out = hpf.process_sample(in);
        if (i > 100) {
            hf_amp = std::abs(out);
        }
    }
    NF_CHECK(hf_amp > 0.85f);
}

NF_TEST(biquad_multichannel_interleaved_processing) {
    const f32 sample_rate = 44100.0f;
    BiquadFilter lpf(FilterType::LowPass, 1000.0f, sample_rate);

    // Stereo buffer (2 channels, 100 frames = 200 samples)
    std::vector<f32> buffer(200, 1.0f);
    lpf.process_buffer(buffer.data(), buffer.size(), 2);

    // Tail samples should be close to 1.0
    NF_CHECK_NEAR(buffer[198], 1.0f, 0.02f);
    NF_CHECK_NEAR(buffer[199], 1.0f, 0.02f);
}

NF_TEST(delay_echo_feedback_and_wet_mix) {
    const f32 sample_rate = 44100.0f;
    DelayEffect delay(500.0f, sample_rate);

    // 10 ms delay = 441 samples
    delay.configure(10.0f, 0.5f, 1.0f, 0.0f); // pure wet

    // Buffer with a single impulse at frame 0
    std::vector<f32> buffer(1000, 0.0f);
    buffer[0] = 1.0f;

    delay.process(buffer.data(), buffer.size(), 1);

    // At frame 0, pure wet is 0
    NF_CHECK_NEAR(buffer[0], 0.0f, 0.01f);

    // At frame 441, first delayed echo should appear
    NF_CHECK(buffer[441] > 0.9f);

    // At frame 441 + 441 = 882, second delayed echo with 0.5 feedback should appear (~0.5)
    NF_CHECK_NEAR(buffer[882], 0.5f, 0.1f);
}

NF_TEST(soundcue_weighted_selection) {
    SoundCue cue("footstep");
    cue.add_variant("step_soft", 10.0f); // heavy weight
    cue.add_variant("step_hard", 1.0f);  // low weight

    // Roll 0.1 should pick the heavy weight
    auto r1 = cue.evaluate(0.1f);
    NF_CHECK_EQ(r1.sound_name, "step_soft");
    NF_CHECK_EQ(r1.variant_index, 0u);
}

NF_TEST(soundcue_pitch_and_volume_randomization_bounds) {
    SoundCue cue("explosion");
    cue.add_variant("boom_mono", 1.0f, 1.2f, 1.0f); // vol_scale=1.2
    cue.set_volume_range(0.8f, 1.0f);
    cue.set_pitch_range(0.9f, 1.1f);

    auto r_min = cue.evaluate(0.0f, 0.0f, 0.0f);
    NF_CHECK_NEAR(r_min.final_volume, 1.2f * 0.8f, 1e-4f);
    NF_CHECK_NEAR(r_min.final_pitch, 1.0f * 0.9f, 1e-4f);

    auto r_max = cue.evaluate(0.0f, 1.0f, 1.0f);
    NF_CHECK_NEAR(r_max.final_volume, 1.2f * 1.0f, 1e-4f);
    NF_CHECK_NEAR(r_max.final_pitch, 1.0f * 1.1f, 1e-4f);
}

NF_TEST(soundcue_anti_repetition_history) {
    SoundCue cue("swords");
    cue.add_variant("slash_01", 1.0f);
    cue.add_variant("slash_02", 1.0f);
    cue.add_variant("slash_03", 1.0f);
    cue.set_no_repeat_count(1); // Do not repeat immediately

    // Force roll 0.0 repeatedly
    auto r1 = cue.evaluate(0.0f);
    auto r2 = cue.evaluate(0.0f);
    auto r3 = cue.evaluate(0.0f);

    // Consecutive picks must not be identical
    NF_CHECK(r1.sound_name != r2.sound_name);
    NF_CHECK(r2.sound_name != r3.sound_name);
}
