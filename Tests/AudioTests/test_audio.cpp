// AudioTests — audio engine math: attenuation, 3D pan/gain, mixing.
//
// All tests are pure math — no GPU, no audio hardware, no window. They verify
// that the audio math (attenuation curves, stereo panning, mixing) produces
// the correct values for known inputs, which is the property the runtime and
// editor depend on.

#include <NF/Test/TestFramework.hpp>
#include <NF/Audio/AudioEngine.hpp>

#include <cmath>
#include <cstring>

using namespace nf;
using namespace nf::audio;

// ---------------------------------------------------------------------------
// Attenuation
// ---------------------------------------------------------------------------

NF_TEST(attenuation_none_returns_one) {
    SpatialSettings s;
    s.model = AttenuationModel::None;
    NF_CHECK_NEAR(compute_attenuation(s, 0.0f), 1.0f, 1e-5f);
    NF_CHECK_NEAR(compute_attenuation(s, 100.0f), 1.0f, 1e-5f);
}

NF_TEST(attenuation_linear_below_min_is_one) {
    SpatialSettings s;
    s.model = AttenuationModel::Linear;
    s.min_distance = 5.0f;
    s.max_distance = 50.0f;
    NF_CHECK_NEAR(compute_attenuation(s, 0.0f), 1.0f, 1e-5f);
    NF_CHECK_NEAR(compute_attenuation(s, 3.0f), 1.0f, 1e-5f);
    NF_CHECK_NEAR(compute_attenuation(s, 5.0f), 1.0f, 1e-5f);
}

NF_TEST(attenuation_linear_above_max_is_zero) {
    SpatialSettings s;
    s.model = AttenuationModel::Linear;
    s.min_distance = 5.0f;
    s.max_distance = 50.0f;
    NF_CHECK_NEAR(compute_attenuation(s, 50.0f), 0.0f, 1e-5f);
    NF_CHECK_NEAR(compute_attenuation(s, 100.0f), 0.0f, 1e-5f);
}

NF_TEST(attenuation_linear_midpoint_is_half) {
    SpatialSettings s;
    s.model = AttenuationModel::Linear;
    s.min_distance = 10.0f;
    s.max_distance = 20.0f;
    // At distance 15 (midpoint), gain should be 0.5.
    NF_CHECK_NEAR(compute_attenuation(s, 15.0f), 0.5f, 1e-5f);
}

NF_TEST(attenuation_inverse_decreases_monotonically) {
    SpatialSettings s;
    s.model = AttenuationModel::Inverse;
    s.min_distance = 1.0f;
    s.max_distance = 100.0f;
    s.rolloff = 1.0f;

    f32 g1 = compute_attenuation(s, 1.0f);
    f32 g2 = compute_attenuation(s, 10.0f);
    f32 g3 = compute_attenuation(s, 50.0f);

    NF_CHECK(g1 > g2);
    NF_CHECK(g2 > g3);
}

NF_TEST(attenuation_exponential_decreases_monotonically) {
    SpatialSettings s;
    s.model = AttenuationModel::Exponential;
    s.min_distance = 1.0f;
    s.max_distance = 100.0f;
    s.rolloff = 1.0f;

    f32 g1 = compute_attenuation(s, 1.0f);
    f32 g2 = compute_attenuation(s, 2.0f);
    f32 g3 = compute_attenuation(s, 10.0f);

    NF_CHECK(g1 > g2);
    NF_CHECK(g2 > g3);
}

// ---------------------------------------------------------------------------
// 3D pan/gain
// ---------------------------------------------------------------------------

NF_TEST(pan_gain_source_in_front_is_centered) {
    SpatialSettings s;
    s.model = AttenuationModel::None;

    // Listener at origin, facing -Z. Source directly in front at (0,0,-5).
    Vec3 lpos = {0, 0, 0};
    Vec3 lfwd = {0, 0, -1};
    Vec3 lup = {0, 1, 0};
    Vec3 spos = {0, 0, -5};

    PanGain pg = compute_3d_pan_gain(lpos, lfwd, lup, spos, s);
    // Centered source: equal-power panning gives cos(pi/4) = sin(pi/4) ≈ 0.707 each.
    NF_CHECK_NEAR(pg.left, pg.right, 1e-4f);
    NF_CHECK_NEAR(pg.left, 0.7071f, 0.01f);
}

NF_TEST(pan_gain_source_to_the_right_is_right_only) {
    SpatialSettings s;
    s.model = AttenuationModel::None;

    Vec3 lpos = {0, 0, 0};
    Vec3 lfwd = {0, 0, -1};
    Vec3 lup = {0, 1, 0};
    Vec3 spos = {5, 0, 0}; // directly to the right

    PanGain pg = compute_3d_pan_gain(lpos, lfwd, lup, spos, s);
    NF_CHECK_NEAR(pg.left, 0.0f, 1e-4f);
    NF_CHECK_NEAR(pg.right, 1.0f, 1e-4f);
}

NF_TEST(pan_gain_source_to_the_left_is_left_only) {
    SpatialSettings s;
    s.model = AttenuationModel::None;

    Vec3 lpos = {0, 0, 0};
    Vec3 lfwd = {0, 0, -1};
    Vec3 lup = {0, 1, 0};
    Vec3 spos = {-5, 0, 0}; // directly to the left

    PanGain pg = compute_3d_pan_gain(lpos, lfwd, lup, spos, s);
    NF_CHECK_NEAR(pg.left, 1.0f, 1e-4f);
    NF_CHECK_NEAR(pg.right, 0.0f, 1e-4f);
}

NF_TEST(pan_gain_source_at_listener_is_centered) {
    SpatialSettings s;
    s.model = AttenuationModel::None;

    Vec3 lpos = {0, 0, 0};
    Vec3 lfwd = {0, 0, -1};
    Vec3 lup = {0, 1, 0};
    Vec3 spos = {0, 0, 0};

    PanGain pg = compute_3d_pan_gain(lpos, lfwd, lup, spos, s);
    NF_CHECK_NEAR(pg.left, pg.right, 1e-4f);
    NF_CHECK_NEAR(pg.left, 1.0f, 1e-4f);
}

NF_TEST(pan_gain_with_linear_attenuation) {
    SpatialSettings s;
    s.model = AttenuationModel::Linear;
    s.min_distance = 1.0f;
    s.max_distance = 10.0f;

    // Source at distance 5.5 (midpoint) in front.
    Vec3 lpos = {0, 0, 0};
    Vec3 lfwd = {0, 0, -1};
    Vec3 lup = {0, 1, 0};
    Vec3 spos = {0, 0, -5.5};

    PanGain pg = compute_3d_pan_gain(lpos, lfwd, lup, spos, s);
    // Attenuation = 0.5, centered (equal-power: cos(pi/4) ≈ 0.707).
    // gain = 0.5, left = 0.5 * 0.707 ≈ 0.354.
    NF_CHECK_NEAR(pg.left, pg.right, 1e-4f);
    NF_CHECK_NEAR(pg.left, 0.3536f, 0.01f);
}

// ---------------------------------------------------------------------------
// Mixer
// ---------------------------------------------------------------------------

NF_TEST(mixer_sums_two_sources) {
    AudioBuffer buf;
    buf.channels = 1;
    buf.sample_rate = 44100;
    buf.samples.resize(4);
    buf.samples[0] = 0.5f;
    buf.samples[1] = 0.3f;
    buf.samples[2] = 0.2f;
    buf.samples[3] = 0.1f;

    AudioSource s1, s2;
    s1.buffer = &buf;
    s1.volume = 1.0f;
    s1.playing = true;
    s2.buffer = &buf;
    s2.volume = 0.5f;
    s2.playing = true;

    AudioBus bus;
    bus.volume = 1.0f;

    const usize num_frames = 4;
    f32 left[num_frames] = {0, 0, 0, 0};
    f32 right[num_frames] = {0, 0, 0, 0};

    Vec3 lpos = {0, 0, 0};
    Vec3 lfwd = {0, 0, -1};
    Vec3 lup = {0, 1, 0};

    bus.mix_source(s1, lpos, lfwd, lup, left, right, num_frames, 44100);
    bus.mix_source(s2, lpos, lfwd, lup, left, right, num_frames, 44100);

    // s1: 0.5, 0.3, 0.2, 0.1
    // s2: 0.25, 0.15, 0.1, 0.05 (0.5x volume)
    // sum: 0.75, 0.45, 0.3, 0.15
    NF_CHECK_NEAR(left[0], 0.75f, 1e-5f);
    NF_CHECK_NEAR(left[1], 0.45f, 1e-5f);
    NF_CHECK_NEAR(left[2], 0.3f, 1e-5f);
    NF_CHECK_NEAR(left[3], 0.15f, 1e-5f);
}

NF_TEST(mixer_stereo_buffer_fills_both_channels) {
    AudioBuffer buf;
    buf.channels = 2;
    buf.sample_rate = 44100;
    buf.samples.resize(4); // 2 frames, 2 channels each
    buf.samples[0] = 0.5f; // frame 0 left
    buf.samples[1] = 0.7f; // frame 0 right
    buf.samples[2] = 0.3f; // frame 1 left
    buf.samples[3] = 0.9f; // frame 1 right

    AudioSource src;
    src.buffer = &buf;
    src.volume = 1.0f;
    src.playing = true;

    AudioBus bus;
    bus.volume = 1.0f;

    const usize num_frames = 2;
    f32 left[num_frames] = {0, 0};
    f32 right[num_frames] = {0, 0};

    Vec3 lpos = {0, 0, 0};
    Vec3 lfwd = {0, 0, -1};
    Vec3 lup = {0, 1, 0};

    bus.mix_source(src, lpos, lfwd, lup, left, right, num_frames, 44100);

    NF_CHECK_NEAR(left[0], 0.5f, 1e-5f);
    NF_CHECK_NEAR(right[0], 0.7f, 1e-5f);
    NF_CHECK_NEAR(left[1], 0.3f, 1e-5f);
    NF_CHECK_NEAR(right[1], 0.9f, 1e-5f);
}

NF_TEST(mixer_stops_at_end_of_non_looping_buffer) {
    AudioBuffer buf;
    buf.channels = 1;
    buf.sample_rate = 44100;
    buf.samples.resize(2);
    buf.samples[0] = 0.5f;
    buf.samples[1] = 0.3f;

    AudioSource src;
    src.buffer = &buf;
    src.volume = 1.0f;
    src.playing = true;
    src.looping = false;

    AudioBus bus;
    bus.volume = 1.0f;

    f32 left[4] = {0, 0, 0, 0};
    f32 right[4] = {0, 0, 0, 0};

    Vec3 lpos = {0, 0, 0};
    Vec3 lfwd = {0, 0, -1};
    Vec3 lup = {0, 1, 0};

    // Request 4 frames but buffer only has 2.
    bus.mix_source(src, lpos, lfwd, lup, left, right, 4, 44100);

    NF_CHECK_NEAR(left[0], 0.5f, 1e-5f);
    NF_CHECK_NEAR(left[1], 0.3f, 1e-5f);
    NF_CHECK_NEAR(left[2], 0.0f, 1e-5f);
    NF_CHECK_NEAR(left[3], 0.0f, 1e-5f);
    NF_CHECK(!src.playing);
}

NF_TEST(mixer_loops_buffer) {
    AudioBuffer buf;
    buf.channels = 1;
    buf.sample_rate = 44100;
    buf.samples.resize(2);
    buf.samples[0] = 0.5f;
    buf.samples[1] = 0.3f;

    AudioSource src;
    src.buffer = &buf;
    src.volume = 1.0f;
    src.playing = true;
    src.looping = true;

    AudioBus bus;
    bus.volume = 1.0f;

    f32 left[4] = {0, 0, 0, 0};
    f32 right[4] = {0, 0, 0, 0};

    Vec3 lpos = {0, 0, 0};
    Vec3 lfwd = {0, 0, -1};
    Vec3 lup = {0, 1, 0};

    // Request 4 frames, buffer has 2, loops.
    bus.mix_source(src, lpos, lfwd, lup, left, right, 4, 44100);

    // Frames: 0.5, 0.3, 0.5, 0.3 (wrapped).
    NF_CHECK_NEAR(left[0], 0.5f, 1e-5f);
    NF_CHECK_NEAR(left[1], 0.3f, 1e-5f);
    NF_CHECK_NEAR(left[2], 0.5f, 1e-5f);
    NF_CHECK_NEAR(left[3], 0.3f, 1e-5f);
    NF_CHECK(src.playing);
}

NF_TEST(mixer_volume_scales_output) {
    AudioBuffer buf;
    buf.channels = 1;
    buf.sample_rate = 44100;
    buf.samples.resize(1);
    buf.samples[0] = 1.0f;

    AudioSource src;
    src.buffer = &buf;
    src.volume = 0.5f;
    src.playing = true;

    AudioBus bus;
    bus.volume = 1.0f;

    f32 left[1] = {0};
    f32 right[1] = {0};

    Vec3 lpos = {0, 0, 0};
    Vec3 lfwd = {0, 0, -1};
    Vec3 lup = {0, 1, 0};

    bus.mix_source(src, lpos, lfwd, lup, left, right, 1, 44100);
    NF_CHECK_NEAR(left[0], 0.5f, 1e-5f);
}

// ---------------------------------------------------------------------------
// NullAudioDevice
// ---------------------------------------------------------------------------

NF_TEST(null_device_initialize_and_shutdown) {
    NullAudioDevice dev;
    NF_CHECK(!dev.is_initialized());
    NF_CHECK(dev.initialize(44100, 256));
    NF_CHECK(dev.is_initialized());
    NF_CHECK_EQ(dev.sample_rate(), 44100u);
    dev.shutdown();
    NF_CHECK(!dev.is_initialized());
}

NF_TEST(null_device_produces_silence) {
    NullAudioDevice dev;
    dev.initialize(44100, 256);

    const usize n = 16;
    f32 left[n];
    f32 right[n];

    // Fill with non-zero so we can tell if the device overwrites.
    for (usize i = 0; i < n; ++i) {
        left[i] = 1.0f;
        right[i] = 1.0f;
    }

    dev.request_buffer(left, right, n);

    for (usize i = 0; i < n; ++i) {
        NF_CHECK_NEAR(left[i], 0.0f, 1e-5f);
        NF_CHECK_NEAR(right[i], 0.0f, 1e-5f);
    }
}

NF_TEST(audio_buffer_duration) {
    AudioBuffer buf;
    buf.channels = 1;
    buf.sample_rate = 44100;
    buf.samples.resize(44100); // 1 second of mono audio
    NF_CHECK_NEAR(buf.duration_seconds(), 1.0f, 1e-5f);
}

NF_TEST(audio_buffer_stereo_frame_count) {
    AudioBuffer buf;
    buf.channels = 2;
    buf.sample_rate = 44100;
    buf.samples.resize(88200); // 44100 stereo frames
    NF_CHECK_EQ(buf.frame_count(), 44100u);
    NF_CHECK_NEAR(buf.duration_seconds(), 1.0f, 1e-5f);
}

// ---------------------------------------------------------------------------
// make_tone_buffer — procedural PCM
//
// The scene format can name a buffer but the audio import pipeline is a Phase 9
// non-goal, so the only way an `Audio:` line produces real samples is this
// generator. These tests pin its contract: exact frame counts, a sine that
// starts at zero, headroom below full scale, and silence (not a huge
// allocation) for bad input.
// ---------------------------------------------------------------------------

NF_TEST(tone_buffer_has_expected_frame_count_and_rate) {
    const AudioBuffer buf = make_tone_buffer(440.0f, 1.0f, 44100, 1);

    NF_CHECK_EQ(buf.sample_rate, 44100u);
    NF_CHECK_EQ(buf.channels, 1u);
    NF_CHECK_EQ(buf.frame_count(), 44100u);
    NF_CHECK_NEAR(buf.duration_seconds(), 1.0f, 1e-5f);

    const AudioBuffer half = make_tone_buffer(440.0f, 0.5f, 44100, 1);
    NF_CHECK_EQ(half.frame_count(), 22050u);
    NF_CHECK_NEAR(half.duration_seconds(), 0.5f, 1e-5f);
}

NF_TEST(tone_buffer_traces_a_sine_from_zero) {
    // 11025 Hz at 44100 Hz advances exactly a quarter period per sample, so the
    // first four samples are 0, +1, 0, -1 scaled by the generator's amplitude.
    const AudioBuffer buf = make_tone_buffer(11025.0f, 1.0f, 44100, 1);
    NF_CHECK_EQ(buf.frame_count(), 44100u);

    NF_CHECK_NEAR(buf.samples[0], 0.0f, 1e-4f);
    NF_CHECK_NEAR(buf.samples[1], 0.5f, 1e-4f);
    NF_CHECK_NEAR(buf.samples[2], 0.0f, 1e-4f);
    NF_CHECK_NEAR(buf.samples[3], -0.5f, 1e-4f);
}

NF_TEST(tone_buffer_keeps_headroom_below_full_scale) {
    const AudioBuffer buf = make_tone_buffer(220.0f, 1.0f, 44100, 1);

    f32 peak = 0.0f;
    for (f32 s : buf.samples) {
        const f32 mag = std::fabs(s);
        if (mag > peak) peak = mag;
    }

    // Reaches its amplitude (so this is a real tone, not silence) but never
    // hits full scale, which matters because AudioBus sums sources together.
    NF_CHECK(peak > 0.49f);
    NF_CHECK(peak <= 0.5f + 1e-4f);
}

NF_TEST(tone_buffer_is_mono_unless_asked_for_stereo) {
    const AudioBuffer mono = make_tone_buffer(440.0f, 0.1f, 44100, 3);
    NF_CHECK_EQ(mono.channels, 1u);

    const AudioBuffer stereo = make_tone_buffer(440.0f, 0.1f, 44100, 2);
    NF_CHECK_EQ(stereo.channels, 2u);
    NF_CHECK_EQ(stereo.frame_count(), mono.frame_count());
    NF_CHECK_EQ(stereo.samples.size(), mono.samples.size() * 2);

    // Both interleaved lanes carry the same sample.
    for (usize f = 0; f < mono.frame_count(); ++f) {
        NF_CHECK_NEAR(stereo.samples[f * 2 + 0], mono.samples[f], 1e-6f);
        NF_CHECK_NEAR(stereo.samples[f * 2 + 1], mono.samples[f], 1e-6f);
    }
}

NF_TEST(tone_buffer_is_deterministic) {
    const AudioBuffer a = make_tone_buffer(440.0f, 0.25f, 44100, 1);
    const AudioBuffer b = make_tone_buffer(440.0f, 0.25f, 44100, 1);

    NF_CHECK_EQ(a.samples.size(), b.samples.size());
    NF_CHECK(std::memcmp(a.samples.data(), b.samples.data(),
                         a.samples.size() * sizeof(f32)) == 0);
}

NF_TEST(tone_buffer_rejects_non_positive_input) {
    NF_CHECK(make_tone_buffer(440.0f, 0.0f, 44100, 1).samples.empty());
    NF_CHECK(make_tone_buffer(440.0f, -1.0f, 44100, 1).samples.empty());
    NF_CHECK(make_tone_buffer(440.0f, 1.0f, 0, 1).samples.empty());

    // A duration too short to fill one frame yields silence, not a partial frame.
    NF_CHECK(make_tone_buffer(440.0f, 1e-9f, 44100, 1).samples.empty());

    // Rejected buffers report zero rate so frame_count()/duration_seconds() are
    // both safe and zero rather than garbage.
    const AudioBuffer bad = make_tone_buffer(440.0f, 0.0f, 44100, 1);
    NF_CHECK_EQ(bad.sample_rate, 0u);
    NF_CHECK_EQ(bad.frame_count(), 0u);
    NF_CHECK_NEAR(bad.duration_seconds(), 0.0f, 1e-6f);
}

NF_TEST(tone_buffer_defaults_match_engine_constants) {
    NF_CHECK_EQ(kDefaultSampleRate, 44100u);
    NF_CHECK_EQ(kDefaultBufferFrames, 1024u);
}
