// AudioTests — the music and ambience layer.
//
// Envelopes here are deliberately linear ramps at 1/fade_seconds per second,
// not exponential curves: every gain in this file is a number a test can pin
// exactly, and a fade the designer asked to take two seconds takes two seconds.

#include <NF/Test/TestFramework.hpp>
#include <NF/Audio/AudioEngine.hpp>
#include <NF/Audio/MusicSystem.hpp>

#include <vector>

using namespace nf;
using namespace nf::audio;

namespace {

AudioBuffer make_constant(f32 value, usize frames) {
    AudioBuffer buf;
    buf.channels = 1;
    buf.sample_rate = 44100;
    buf.samples.assign(frames, value);
    return buf;
}

MusicTrack track_of(const AudioBuffer& buffer, f32 base_volume) {
    MusicTrack track;
    track.buffer = &buffer;
    track.base_volume = base_volume;
    return track;
}

} // namespace

// ---------------------------------------------------------------------------
// Music envelopes
// ---------------------------------------------------------------------------

NF_TEST(music_play_fades_in_linearly_then_reports_playing) {
    const AudioBuffer buffer = make_constant(1.0f, 100);
    const MusicTrack track = track_of(buffer, 1.0f);

    MusicSystem music;
    NF_CHECK(music.state() == MusicState::Stopped);
    NF_CHECK(!music.has_outgoing());

    music.play(track, 1.0f);
    NF_CHECK(music.state() == MusicState::FadingIn);
    NF_CHECK_NEAR(music.current_gain(), 0.0f, 1e-6f);

    music.update(0.25f);
    NF_CHECK_NEAR(music.current_gain(), 0.25f, 1e-5f);
    NF_CHECK(music.state() == MusicState::FadingIn);

    music.update(0.5f);
    NF_CHECK_NEAR(music.current_gain(), 0.75f, 1e-5f);

    music.update(0.25f);
    NF_CHECK_NEAR(music.current_gain(), 1.0f, 1e-6f);
    NF_CHECK(music.state() == MusicState::Playing);

    // Holding past the ramp does not overshoot the ceiling.
    music.update(5.0f);
    NF_CHECK_NEAR(music.current_gain(), 1.0f, 1e-6f);
    NF_CHECK(music.state() == MusicState::Playing);
}

NF_TEST(music_play_with_a_zero_fade_starts_at_full_level) {
    const AudioBuffer buffer = make_constant(1.0f, 100);
    MusicSystem music;
    music.play(track_of(buffer, 1.0f), 0.0f);

    NF_CHECK(music.state() == MusicState::Playing);
    NF_CHECK_NEAR(music.current_gain(), 1.0f, 1e-6f);
    NF_CHECK(!music.has_outgoing());
}

NF_TEST(music_play_without_a_buffer_is_a_no_op) {
    MusicSystem music;
    music.play(MusicTrack{}, 1.0f);

    NF_CHECK(music.state() == MusicState::Stopped);
    NF_CHECK_NEAR(music.current_gain(), 0.0f, 1e-6f);

    std::vector<f32> left(4, 0.0f);
    std::vector<f32> right(4, 0.0f);
    music.mix_music(left.data(), right.data(), 4);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.0f, 1e-9f);
    }
}

NF_TEST(music_mixes_base_volume_times_envelope_and_loops) {
    // A three-frame ramp, so a wrap is visible sample by sample.
    AudioBuffer buffer;
    buffer.channels = 1;
    buffer.sample_rate = 44100;
    buffer.samples = {1.0f, 0.5f, 0.25f};

    MusicSystem music;
    music.play(track_of(buffer, 0.5f), 0.0f); // gain snaps to 1.0
    NF_CHECK(music.state() == MusicState::Playing);

    const usize frames = 7;
    std::vector<f32> left(frames, 0.0f);
    std::vector<f32> right(frames, 0.0f);
    music.mix_music(left.data(), right.data(), frames);

    // base_volume 0.5 applied to a looping 1, 0.5, 0.25 pattern.
    const f32 expected[7] = {0.5f, 0.25f, 0.125f, 0.5f,
                             0.25f, 0.125f, 0.5f};
    for (usize i = 0; i < frames; ++i) {
        NF_CHECK_NEAR(left[i], expected[i], 1e-6f);
        NF_CHECK_NEAR(right[i], expected[i], 1e-6f);
    }
}

NF_TEST(music_envelope_scales_what_is_mixed) {
    const AudioBuffer buffer = make_constant(1.0f, 64);
    MusicSystem music;
    music.play(track_of(buffer, 1.0f), 1.0f);
    music.update(0.5f); // half way up the ramp

    std::vector<f32> left(4, 0.0f);
    std::vector<f32> right(4, 0.0f);
    music.mix_music(left.data(), right.data(), 4);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.5f, 1e-5f);
    }
}

NF_TEST(music_play_over_live_music_fades_the_old_track_out) {
    const AudioBuffer a = make_constant(1.0f, 64);
    const AudioBuffer b = make_constant(1.0f, 64);

    MusicSystem music;
    music.play(track_of(a, 1.0f), 0.0f);
    NF_CHECK(music.state() == MusicState::Playing);

    // play() over live music is a crossfade: the new track fades in and the
    // old one keeps sounding while it fades out. The state reads FadingIn
    // because the new track is what is fading in; has_outgoing() says the rest.
    music.play(track_of(b, 1.0f), 2.0f);
    NF_CHECK(music.state() == MusicState::FadingIn);
    NF_CHECK(music.has_outgoing());
    NF_CHECK_NEAR(music.outgoing_gain(), 1.0f, 1e-6f);

    music.update(1.0f);
    NF_CHECK_NEAR(music.current_gain(), 0.5f, 1e-5f);
    NF_CHECK_NEAR(music.outgoing_gain(), 0.5f, 1e-5f);

    music.update(1.0f);
    NF_CHECK_NEAR(music.current_gain(), 1.0f, 1e-6f);
    NF_CHECK(music.state() == MusicState::Playing);
    NF_CHECK(!music.has_outgoing());
}

NF_TEST(music_crossfade_reports_crossfading_until_the_old_track_is_gone) {
    const AudioBuffer a = make_constant(1.0f, 64);
    const AudioBuffer b = make_constant(1.0f, 64);

    MusicSystem music;
    music.play(track_of(a, 1.0f), 0.0f);
    music.crossfade(track_of(b, 1.0f), 1.0f, 1.0f);

    NF_CHECK(music.state() == MusicState::Crossfading);
    NF_CHECK(music.has_outgoing());
    NF_CHECK_NEAR(music.current_gain(), 0.0f, 1e-6f);
    NF_CHECK_NEAR(music.outgoing_gain(), 1.0f, 1e-6f);

    music.update(0.5f);
    NF_CHECK_NEAR(music.current_gain(), 0.5f, 1e-5f);
    NF_CHECK_NEAR(music.outgoing_gain(), 0.5f, 1e-5f);
    NF_CHECK(music.state() == MusicState::Crossfading);

    music.update(0.5f);
    NF_CHECK_NEAR(music.current_gain(), 1.0f, 1e-6f);
    NF_CHECK(!music.has_outgoing());
    NF_CHECK(music.state() == MusicState::Playing);

    // Both halves of the crossfade are summed while it runs.
    music.play(track_of(a, 1.0f), 0.0f);
    music.crossfade(track_of(b, 1.0f), 1.0f, 1.0f);
    music.update(0.5f);
    std::vector<f32> left(4, 0.0f);
    std::vector<f32> right(4, 0.0f);
    music.mix_music(left.data(), right.data(), 4);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 1.0f, 1e-5f); // 0.5 + 0.5
    }
}

NF_TEST(music_stop_fades_out_and_then_reports_stopped) {
    const AudioBuffer a = make_constant(1.0f, 64);

    MusicSystem music;
    music.play(track_of(a, 1.0f), 0.0f);
    music.stop(0.5f);

    NF_CHECK(music.state() == MusicState::FadingOut);
    NF_CHECK(music.has_outgoing());
    NF_CHECK_NEAR(music.outgoing_gain(), 1.0f, 1e-6f);

    music.update(0.25f);
    NF_CHECK_NEAR(music.outgoing_gain(), 0.5f, 1e-5f);
    NF_CHECK(music.state() == MusicState::FadingOut);

    music.update(0.25f);
    NF_CHECK(!music.has_outgoing());
    NF_CHECK(music.state() == MusicState::Stopped);

    // A stopped system mixes silence and stays stopped.
    std::vector<f32> left(4, 0.0f);
    std::vector<f32> right(4, 0.0f);
    music.mix_music(left.data(), right.data(), 4);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.0f, 1e-9f);
    }
    music.update(1.0f);
    NF_CHECK(music.state() == MusicState::Stopped);
}

NF_TEST(music_stop_with_a_zero_fade_is_immediate) {
    const AudioBuffer a = make_constant(1.0f, 64);
    MusicSystem music;
    music.play(track_of(a, 1.0f), 0.0f);
    music.stop(0.0f);

    NF_CHECK(!music.has_outgoing());
    NF_CHECK(music.state() == MusicState::Stopped);
    NF_CHECK_NEAR(music.current_gain(), 0.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// Ambience bed
// ---------------------------------------------------------------------------

NF_TEST(ambience_fades_in_to_full_and_mixes_independently_of_music) {
    const AudioBuffer bed = make_constant(0.5f, 64);

    MusicSystem music;
    NF_CHECK(!music.has_ambience());
    NF_CHECK_NEAR(music.ambience_gain(), 0.0f, 1e-6f);

    music.set_ambience(&bed, 0.5f);
    NF_CHECK(music.has_ambience());
    NF_CHECK_NEAR(music.ambience_gain(), 0.0f, 1e-6f);

    music.update(0.25f);
    NF_CHECK_NEAR(music.ambience_gain(), 0.5f, 1e-5f);
    music.update(0.25f);
    NF_CHECK_NEAR(music.ambience_gain(), 1.0f, 1e-6f);

    std::vector<f32> left(4, 0.0f);
    std::vector<f32> right(4, 0.0f);
    music.mix_ambience(left.data(), right.data(), 4);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.5f, 1e-6f);
    }
    // The ambience bed is not part of the music mix.
    std::vector<f32> music_left(4, 0.0f);
    std::vector<f32> music_right(4, 0.0f);
    music.mix_music(music_left.data(), music_right.data(), 4);
    for (f32 v : music_left) {
        NF_CHECK_NEAR(v, 0.0f, 1e-9f);
    }
}

NF_TEST(ambience_clear_fades_out_over_the_requested_time) {
    const AudioBuffer bed = make_constant(0.5f, 64);

    MusicSystem music;
    music.set_ambience(&bed, 0.0f); // instant: gain snaps to 1
    NF_CHECK_NEAR(music.ambience_gain(), 1.0f, 1e-6f);

    music.clear_ambience(1.0f);
    NF_CHECK(music.has_ambience()); // still sounding, on its way down

    music.update(0.25f);
    NF_CHECK_NEAR(music.ambience_gain(), 0.75f, 1e-5f);
    NF_CHECK(music.has_ambience());

    music.update(0.5f);
    NF_CHECK_NEAR(music.ambience_gain(), 0.25f, 1e-5f);
    NF_CHECK(music.has_ambience());

    music.update(0.25f);
    NF_CHECK(!music.has_ambience());
    NF_CHECK_NEAR(music.ambience_gain(), 0.0f, 1e-6f);

    // Clearing an already-clear bed is a no-op, not a re-fade.
    music.clear_ambience(0.5f);
    NF_CHECK(!music.has_ambience());
}

NF_TEST(ambience_without_a_buffer_is_a_no_op) {
    MusicSystem music;
    music.set_ambience(nullptr, 1.0f);
    NF_CHECK(!music.has_ambience());

    std::vector<f32> left(4, 0.0f);
    std::vector<f32> right(4, 0.0f);
    music.mix_ambience(left.data(), right.data(), 4);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.0f, 1e-9f);
    }
}
