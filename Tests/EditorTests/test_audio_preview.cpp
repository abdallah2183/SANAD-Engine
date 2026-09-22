// Editor audio waveform: the CPU half of the P3 audio preview.
//
// The inspector draws min/max bars, so the envelope is the shape the panel
// actually shows. What matters: silence stays flat, a tone is symmetric about
// zero, the loudest channel represents stereo content, and a bucket count the
// buffer cannot fill is clamped rather than read past the end.

#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/AudioPreview.hpp>

#include <cmath>
#include <vector>

using namespace nf;

namespace {

std::vector<f32> make_tone(u32 frames, u32 channels, f32 freq, f32 amplitude) {
    std::vector<f32> out(static_cast<usize>(frames) * channels, 0.0f);
    for (u32 i = 0; i < frames; ++i) {
        const f32 v = amplitude * std::sin(2.0f * 3.14159265f * freq *
                                           static_cast<f32>(i) / 1000.0f);
        for (u32 c = 0; c < channels; ++c) {
            out[static_cast<usize>(i) * channels + c] = v;
        }
    }
    return out;
}

} // namespace

NF_TEST(waveform_rejects_bad_input) {
    std::vector<f32> samples{0.5f, -0.5f, 0.25f, -0.25f};
    NF_CHECK(!editor::compute_waveform(nullptr, samples.size(), 1, 4).valid());
    NF_CHECK(!editor::compute_waveform(samples.data(), samples.size(), 0, 4).valid());
    NF_CHECK(!editor::compute_waveform(samples.data(), samples.size(), 1, 0).valid());
    NF_CHECK(!editor::compute_waveform(samples.data(), 0, 1, 4).valid());
    // Fewer samples than one frame per channel: nothing to show.
    NF_CHECK(!editor::compute_waveform(samples.data(), 1, 4, 4).valid());
}

NF_TEST(waveform_silence_is_flat) {
    const std::vector<f32> samples(200, 0.0f);
    const editor::WaveformEnvelope env =
        editor::compute_waveform(samples.data(), samples.size(), 1, 10);
    NF_CHECK(env.valid());
    NF_CHECK_EQ(env.buckets(), 10u);
    NF_CHECK_EQ(env.peak, 0.0f);
    for (usize i = 0; i < env.buckets(); ++i) {
        NF_CHECK_EQ(env.min[i], 0.0f);
        NF_CHECK_EQ(env.max[i], 0.0f);
    }
}

NF_TEST(waveform_tone_is_symmetric) {
    // 10 cycles over 1000 samples with 10 buckets puts exactly one full cycle
    // in every bucket, so each bucket spans both peaks: min ~ -1, max ~ +1 and
    // the curve is symmetric about 0.
    const u32 frames = 1000;
    const std::vector<f32> samples = make_tone(frames, 1, 10.0f, 1.0f);
    const editor::WaveformEnvelope env =
        editor::compute_waveform(samples.data(), samples.size(), 1, 10);
    NF_CHECK(env.valid());
    NF_CHECK_EQ(env.buckets(), 10u);
    NF_CHECK(env.peak > 0.99f);
    for (usize i = 0; i < env.buckets(); ++i) {
        NF_CHECK(env.min[i] < -0.99f);
        NF_CHECK(env.max[i] > 0.99f);
        // Symmetry: the trough is as deep as the peak is tall.
        NF_CHECK(std::fabs(-env.min[i] - env.max[i]) < 0.02f);
    }
}

NF_TEST(waveform_half_amplitude_halves_the_peak) {
    const u32 frames = 1000;
    const std::vector<f32> loud = make_tone(frames, 1, 1.0f, 1.0f);
    const std::vector<f32> quiet = make_tone(frames, 1, 1.0f, 0.5f);
    const editor::WaveformEnvelope e_loud =
        editor::compute_waveform(loud.data(), loud.size(), 1, 10);
    const editor::WaveformEnvelope e_quiet =
        editor::compute_waveform(quiet.data(), quiet.size(), 1, 10);
    NF_CHECK(e_loud.valid());
    NF_CHECK(e_quiet.valid());
    NF_CHECK(e_loud.peak > e_quiet.peak);
    NF_CHECK(std::fabs(e_quiet.peak - 0.5f) < 0.02f);
}

NF_TEST(waveform_picks_the_loudest_channel) {
    // Quiet left, loud right: the envelope must follow the right channel, not
    // the average of the two (an average would hide half the signal).
    const u32 frames = 1000;
    const std::vector<f32> tone = make_tone(frames, 2, 1.0f, 1.0f);
    std::vector<f32> samples(tone.size(), 0.0f);
    for (u32 i = 0; i < frames; ++i) {
        samples[i * 2 + 0] = tone[i * 2 + 0] * 0.1f; // left: 10%
        samples[i * 2 + 1] = tone[i * 2 + 1];        // right: 100%
    }
    const editor::WaveformEnvelope env =
        editor::compute_waveform(samples.data(), samples.size(), 2, 10);
    NF_CHECK(env.valid());
    NF_CHECK(env.peak > 0.99f);
}

NF_TEST(waveform_clamps_buckets_to_frames) {
    // 4 frames asked for 10 buckets: the envelope cannot have more buckets than
    // samples, and each of the 4 must be reported.
    const std::vector<f32> samples{0.0f, 1.0f, 0.0f, -1.0f};
    const editor::WaveformEnvelope env =
        editor::compute_waveform(samples.data(), samples.size(), 1, 10);
    NF_CHECK(env.valid());
    NF_CHECK_EQ(env.buckets(), 4u);
    NF_CHECK_EQ(env.peak, 1.0f);
    NF_CHECK_EQ(env.min[0], 0.0f);
    NF_CHECK_EQ(env.max[0], 0.0f);
    NF_CHECK_EQ(env.min[1], 1.0f);
    NF_CHECK_EQ(env.max[1], 1.0f);
    NF_CHECK_EQ(env.min[3], -1.0f);
    NF_CHECK_EQ(env.max[3], -1.0f);
}

NF_TEST(waveform_one_bucket_covers_the_whole_buffer) {
    const std::vector<f32> samples{-0.5f, 0.25f, 1.0f, -1.0f, 0.0f, 0.75f};
    const editor::WaveformEnvelope env =
        editor::compute_waveform(samples.data(), samples.size(), 1, 1);
    NF_CHECK(env.valid());
    NF_CHECK_EQ(env.buckets(), 1u);
    NF_CHECK_EQ(env.min[0], -1.0f);
    NF_CHECK_EQ(env.max[0], 1.0f);
    NF_CHECK_EQ(env.peak, 1.0f);
}

NF_TEST(waveform_covers_every_frame) {
    // The bucket partitions must be contiguous and total the frame count, so a
    // spike in the last frame is visible in the last bucket and nowhere else.
    const u32 frames = 100;
    std::vector<f32> samples(frames, 0.0f);
    samples[frames - 1] = 1.0f;
    const editor::WaveformEnvelope env =
        editor::compute_waveform(samples.data(), samples.size(), 1, 10);
    NF_CHECK(env.valid());
    NF_CHECK_EQ(env.buckets(), 10u);
    NF_CHECK_EQ(env.peak, 1.0f);
    NF_CHECK_EQ(env.max[9], 1.0f);
    for (usize i = 0; i < 9u; ++i) {
        NF_CHECK_EQ(env.max[i], 0.0f);
    }
}
