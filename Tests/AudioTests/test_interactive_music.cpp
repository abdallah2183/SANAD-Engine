// Tests/AudioTests/test_interactive_music.cpp — interactive stems and beat-synchronized transitions.

#include <NF/Audio/AudioEngine.hpp>
#include <NF/Audio/InteractiveMusic.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <vector>

namespace {

using nf::f32;
using nf::u32;
using nf::usize;
using nf::audio::AudioBuffer;
using nf::audio::BeatClock;
using nf::audio::InteractiveMusicEngine;
using nf::audio::make_tone_buffer;
using nf::audio::QuantizationMode;

NF_TEST(beat_clock_tempo_and_advance) {
    // 120 BPM = 2 beats per second (0.5s per beat), 4 beats per bar = 2.0s per bar
    BeatClock clock(120.0f, 4u);

    NF_CHECK_NEAR(clock.bpm(), 120.0f, 1e-4f);
    NF_CHECK_NEAR(clock.seconds_per_beat(), 0.5f, 1e-4f);
    NF_CHECK_NEAR(clock.seconds_per_bar(), 2.0f, 1e-4f);
    NF_CHECK_EQ(clock.current_beat(), 0u);
    NF_CHECK_EQ(clock.current_bar(), 0u);

    // Advance 0.25s (halfway through beat 0)
    bool crossed = clock.advance(0.25f);
    NF_CHECK(!crossed);
    NF_CHECK_EQ(clock.current_beat(), 0u);
    NF_CHECK_NEAR(clock.beat_fraction(), 0.5f, 1e-4f);

    // Advance 0.3s (crosses into beat 1)
    crossed = clock.advance(0.3f);
    NF_CHECK(crossed);
    NF_CHECK_EQ(clock.current_beat(), 1u);
    NF_CHECK_EQ(clock.current_bar(), 0u);

    // Advance 1.5s (crosses into beat 4, which is bar 1)
    clock.advance(1.5f);
    NF_CHECK_EQ(clock.current_beat(), 4u);
    NF_CHECK_EQ(clock.current_bar(), 1u);
}

NF_TEST(beat_clock_quantization_time_until) {
    BeatClock clock(120.0f, 4u); // 0.5s per beat, 2.0s per bar

    // At song time 0.0s:
    NF_CHECK_NEAR(clock.time_until(QuantizationMode::Immediate), 0.0f, 1e-4f);
    NF_CHECK_NEAR(clock.time_until(QuantizationMode::NextBeat), 0.5f, 1e-4f);
    NF_CHECK_NEAR(clock.time_until(QuantizationMode::NextBar), 2.0f, 1e-4f);

    // Advance 0.2s -> 0.3s until next beat, 1.8s until next bar
    clock.advance(0.2f);
    NF_CHECK_NEAR(clock.time_until(QuantizationMode::NextBeat), 0.3f, 1e-4f);
    NF_CHECK_NEAR(clock.time_until(QuantizationMode::NextBar), 1.8f, 1e-4f);
}

NF_TEST(stem_registration_and_gain) {
    InteractiveMusicEngine engine(120.0f, 4u);
    AudioBuffer tone = make_tone_buffer(440.0f, 1.0f, 44100, 2);

    engine.add_stem("Ambient", &tone, 0.0f, 0.5f, 1.0f);
    engine.add_stem("Drums", &tone, 0.5f, 1.0f, 0.0f);

    NF_CHECK_EQ(engine.stem_count(), 2u);
    NF_CHECK_NEAR(engine.stem_gain("Ambient"), 1.0f, 1e-4f);
    NF_CHECK_NEAR(engine.stem_gain("Drums"), 0.0f, 1e-4f);

    // Manually adjust gain
    engine.set_stem_gain("Drums", 0.8f, 0.0f); // snap
    engine.update(0.01f);
    NF_CHECK_NEAR(engine.stem_gain("Drums"), 0.8f, 1e-4f);

    // Mute
    engine.set_stem_muted("Ambient", true);

    // Remove
    bool removed = engine.remove_stem("Ambient");
    NF_CHECK(removed);
    NF_CHECK_EQ(engine.stem_count(), 1u);
}

NF_TEST(intensity_mapping_activates_stems) {
    InteractiveMusicEngine engine(120.0f, 4u);
    AudioBuffer tone = make_tone_buffer(440.0f, 1.0f, 44100, 2);

    engine.add_stem("Calm", &tone, 0.0f, 0.4f, 1.0f);
    engine.add_stem("Combat", &tone, 0.41f, 1.0f, 0.0f);

    // Low intensity: Calm is active, Combat is 0
    engine.set_intensity(0.2f, 0.0f); // snap
    engine.update(0.01f);
    NF_CHECK_NEAR(engine.stem_gain("Calm"), 1.0f, 1e-4f);
    NF_CHECK_NEAR(engine.stem_gain("Combat"), 0.0f, 1e-4f);

    // High intensity: Combat is active, Calm fades out
    engine.set_intensity(0.8f, 0.0f); // snap
    engine.update(0.01f);
    NF_CHECK_NEAR(engine.stem_gain("Calm"), 0.0f, 1e-4f);
    NF_CHECK_NEAR(engine.stem_gain("Combat"), 1.0f, 1e-4f);
}

NF_TEST(queued_transition_delays_intensity) {
    InteractiveMusicEngine engine(120.0f, 4u); // 0.5s per beat
    AudioBuffer tone = make_tone_buffer(440.0f, 1.0f, 44100, 2);
    engine.add_stem("Combat", &tone, 0.5f, 1.0f, 0.0f);

    // Queue transition to intensity 1.0 at next beat (in 0.5s)
    engine.queue_intensity_transition(1.0f, QuantizationMode::NextBeat, 0.0f);

    // Step 0.2s: transition should NOT have triggered yet
    engine.update(0.2f);
    NF_CHECK_NEAR(engine.intensity(), 0.0f, 1e-4f);

    // Step another 0.35s (total 0.55s > 0.5s): transition triggers
    engine.update(0.35f);
    NF_CHECK_NEAR(engine.intensity(), 1.0f, 1e-4f);
    NF_CHECK_NEAR(engine.stem_gain("Combat"), 1.0f, 1e-4f);
}

NF_TEST(stem_mixing_produces_audio) {
    InteractiveMusicEngine engine(120.0f, 4u);
    AudioBuffer tone = make_tone_buffer(440.0f, 1.0f, 44100, 2);
    engine.add_stem("Lead", &tone, 0.0f, 1.0f, 1.0f);

    std::vector<f32> left(512, 0.0f);
    std::vector<f32> right(512, 0.0f);

    engine.mix(left.data(), right.data(), 512, 44100);

    // Verify non-zero output
    f32 max_sample = 0.0f;
    for (usize i = 0; i < 512; ++i) {
        max_sample = std::max(max_sample, std::abs(left[i]));
    }
    NF_CHECK(max_sample > 0.1f);
}

NF_TEST(stem_muting_silences_channel) {
    InteractiveMusicEngine engine(120.0f, 4u);
    AudioBuffer tone = make_tone_buffer(440.0f, 1.0f, 44100, 2);
    engine.add_stem("Lead", &tone, 0.0f, 1.0f, 1.0f);
    engine.set_stem_muted("Lead", true);

    std::vector<f32> left(512, 0.0f);
    std::vector<f32> right(512, 0.0f);

    engine.mix(left.data(), right.data(), 512, 44100);

    for (usize i = 0; i < 512; ++i) {
        NF_CHECK_NEAR(left[i], 0.0f, 1e-6f);
        NF_CHECK_NEAR(right[i], 0.0f, 1e-6f);
    }
}

NF_TEST(stinger_one_shot_playback) {
    InteractiveMusicEngine engine(120.0f, 4u);
    // Short stinger: 0.05s buffer (~2205 frames)
    AudioBuffer stinger_buf = make_tone_buffer(880.0f, 0.05f, 44100, 2);

    engine.play_stinger(&stinger_buf, 1.0f, QuantizationMode::Immediate);

    std::vector<f32> left(1024, 0.0f);
    std::vector<f32> right(1024, 0.0f);

    // Mix first block: should have audio
    engine.mix(left.data(), right.data(), 1024, 44100);
    f32 max_val1 = 0.0f;
    for (usize i = 0; i < 1024; ++i) {
        max_val1 = std::max(max_val1, std::abs(left[i]));
    }
    NF_CHECK(max_val1 > 0.1f);

    // Mix several blocks until stinger finishes
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    engine.mix(left.data(), right.data(), 1024, 44100);
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    engine.mix(left.data(), right.data(), 1024, 44100);

    // Third block past end of buffer: should be silent
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    engine.mix(left.data(), right.data(), 1024, 44100);
    f32 max_val_end = 0.0f;
    for (usize i = 0; i < 1024; ++i) {
        max_val_end = std::max(max_val_end, std::abs(left[i]));
    }
    NF_CHECK_NEAR(max_val_end, 0.0f, 1e-6f);
}

} // namespace
