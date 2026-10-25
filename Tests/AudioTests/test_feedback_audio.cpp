// Tests/AudioTests/test_feedback_audio.cpp - Phase 56: the procedural one-shots.
//
// A scene that opts into combat feedback needs a gunshot and an impact without
// an asset pipeline (WAV import is an explicit non-goal). The buffers are built
// from a fixed-seed xorshift so the same binary produces the same samples on
// every run. These cases pin the contract the mixer relies on:
//
//   * a zero sample rate is refused rather than inventing a clock;
//   * the buffer is mono, the requested rate, and non-silent;
//   * every sample is bounded (the bus, not the source, owns clipping);
//   * the tail decays, so a one-shot ends instead of looping a constant tone;
//   * two calls are bit-identical (replay/debug determinism).

#include <NF/Test/TestFramework.hpp>
#include <NF/Audio/AudioEngine.hpp>

#include <cmath>
#include <algorithm>
#include <vector>

using namespace nf;
using namespace nf::audio;

namespace {

float peak_of(const AudioBuffer& buffer) {
    float peak = 0.0f;
    for (const float sample : buffer.samples) {
        peak = std::max(peak, std::abs(sample));
    }
    return peak;
}

float rms(const std::vector<float>& samples, size_t begin, size_t end) {
    if (end <= begin) {
        return 0.0f;
    }
    double sum = 0.0;
    for (size_t i = begin; i < end; ++i) {
        sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(end - begin)));
}

} // namespace

NF_TEST(procedural_feedback_buffers_refuse_a_zero_sample_rate) {
    NF_CHECK(make_gunshot_buffer(0u).samples.empty());
    NF_CHECK(make_impact_buffer(0u).samples.empty());
    NF_CHECK_EQ(make_gunshot_buffer(0u).sample_rate, 0u);
}

NF_TEST(procedural_gunshot_is_mono_non_silent_and_bounded) {
    const AudioBuffer buffer = make_gunshot_buffer(44100u);
    NF_CHECK_EQ(buffer.sample_rate, 44100u);
    NF_CHECK_EQ(buffer.channels, 1u);
    NF_CHECK(buffer.frame_count() > 0u);
    NF_CHECK(buffer.duration_seconds() > 0.1f);
    const float peak = peak_of(buffer);
    NF_CHECK(peak > 0.2f);
    NF_CHECK(peak <= 0.95f);
    for (const float sample : buffer.samples) {
        NF_CHECK(std::isfinite(sample));
    }
}

NF_TEST(procedural_impact_is_shorter_than_the_gunshot_and_bounded) {
    const AudioBuffer shot = make_gunshot_buffer(44100u);
    const AudioBuffer hit = make_impact_buffer(44100u);
    NF_CHECK_EQ(hit.sample_rate, 44100u);
    NF_CHECK_EQ(hit.channels, 1u);
    NF_CHECK(hit.frame_count() > 0u);
    NF_CHECK(hit.duration_seconds() < shot.duration_seconds());
    const float peak = peak_of(hit);
    NF_CHECK(peak > 0.2f);
    NF_CHECK(peak <= 0.9f);
}

NF_TEST(procedural_one_shots_decay_and_are_deterministic) {
    const AudioBuffer first = make_gunshot_buffer(48000u);
    const AudioBuffer second = make_gunshot_buffer(48000u);
    NF_CHECK_EQ(first.samples.size(), second.samples.size());
    NF_CHECK(first.samples == second.samples);

    // The first tenth of the buffer carries the crack; the last tenth is the
    // decaying tail, so its energy must be lower. A constant buffer would fail
    // this and a looped one would never stop.
    const size_t head_end = first.samples.size() / 10u;
    const size_t tail_begin = first.samples.size() - first.samples.size() / 10u;
    const float head = rms(first.samples, 0u, head_end);
    const float tail = rms(first.samples, tail_begin, first.samples.size());
    NF_CHECK(head > tail * 2.0f);

    const AudioBuffer impact_a = make_impact_buffer(22050u);
    const AudioBuffer impact_b = make_impact_buffer(22050u);
    NF_CHECK(impact_a.samples == impact_b.samples);
}
