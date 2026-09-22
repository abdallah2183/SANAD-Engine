// AudioTests — reverb zones and the echo tail.
//
// A reverb zone is authored as discrete echoes rather than a convolution tail
// precisely so every number here can be pinned exactly: the impulse response
// is a pre-delay tap followed by a feedback comb, and this suite asserts the
// tap *positions*, the tap *gains*, and the 60 dB decay contract rather than
// comparing against a golden file nobody can read.

#include <NF/Test/TestFramework.hpp>
#include <NF/Audio/Reverb.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::audio;

namespace {

/// The cave the walkthrough demo uses: a 37 m round trip with a 1.5 s tail.
ReverbZone make_cave() {
    ReverbZone cave;
    cave.position = {0, 0, 0};
    cave.radius = 20.0f;
    cave.inner_radius = 5.0f;
    cave.wet_gain = 0.35f;
    cave.decay_seconds = 1.5f;
    cave.pre_delay_seconds = 0.03f;
    cave.echo_spacing_seconds = 0.11f;
    return cave;
}

/// First frame of `signal` whose magnitude exceeds `threshold`.
usize first_audible(const std::vector<f32>& signal, f32 threshold) {
    for (usize i = 0; i < signal.size(); ++i) {
        if (std::fabs(signal[i]) > threshold) {
            return i;
        }
    }
    return signal.size();
}

} // namespace

// ---------------------------------------------------------------------------
// Zone sampling
// ---------------------------------------------------------------------------

NF_TEST(reverb_zone_is_full_inside_and_dry_outside) {
    const ReverbZone cave = make_cave();

    const ReverbSample heart = compute_reverb_at(&cave, 1, {0, 0, 0});
    NF_CHECK(heart.active);
    NF_CHECK_EQ(heart.zone_index, 0u);
    NF_CHECK_NEAR(heart.wet_gain, 0.35f, 1e-6f);
    NF_CHECK_NEAR(heart.decay_seconds, 1.5f, 1e-6f);
    NF_CHECK_NEAR(heart.pre_delay_seconds, 0.03f, 1e-6f);
    NF_CHECK_NEAR(heart.echo_spacing_seconds, 0.11f, 1e-6f);

    // Anywhere inside the inner radius is the same full effect.
    NF_CHECK_NEAR(compute_reverb_at(&cave, 1, {0, 0, -5}).wet_gain, 0.35f, 1e-6f);

    // Halfway across the falloff band (5 m .. 20 m, so 12.5 m) is half wet.
    const ReverbSample mid = compute_reverb_at(&cave, 1, {12.5f, 0, 0});
    NF_CHECK(mid.active);
    NF_CHECK_NEAR(mid.wet_gain, 0.175f, 1e-5f);

    // At and beyond the outer radius the listener is in open air.
    NF_CHECK(!compute_reverb_at(&cave, 1, {20, 0, 0}).active);
    NF_CHECK(!compute_reverb_at(&cave, 1, {500, 0, 0}).active);
    NF_CHECK_NEAR(compute_reverb_at(&cave, 1, {500, 0, 0}).wet_gain, 0.0f, 1e-6f);

    // No zones authored at all.
    NF_CHECK(!compute_reverb_at(nullptr, 0, {0, 0, 0}).active);
}

NF_TEST(reverb_zone_without_a_falloff_band_only_covers_its_inner_radius) {
    ReverbZone z;
    z.position = {0, 0, 0};
    z.radius = 1.0f;
    z.inner_radius = 1.0f;
    z.wet_gain = 0.5f;

    NF_CHECK(compute_reverb_at(&z, 1, {0, 0, 0}).active);
    NF_CHECK(compute_reverb_at(&z, 1, {1, 0, 0}).active); // inside inner == 1
    NF_CHECK(!compute_reverb_at(&z, 1, {2, 0, 0}).active);

    // radius < inner_radius is the same degenerate case, not a negative band.
    z.radius = 0.5f;
    NF_CHECK(compute_reverb_at(&z, 1, {0, 0, 0}).active);
    NF_CHECK(!compute_reverb_at(&z, 1, {1.5f, 0, 0}).active);
}

NF_TEST(reverb_strongest_zone_wins_and_ties_keep_the_first) {
    ReverbZone zones[2];
    zones[0].position = {100, 0, 0};
    zones[0].radius = 10.0f;
    zones[0].inner_radius = 1.0f;
    zones[0].wet_gain = 0.9f;
    zones[0].decay_seconds = 3.0f;
    zones[1].position = {0, 0, 0};
    zones[1].radius = 10.0f;
    zones[1].inner_radius = 1.0f;
    zones[1].wet_gain = 0.3f;
    zones[1].decay_seconds = 1.0f;

    const ReverbSample s = compute_reverb_at(zones, 2, {0, 0, 0});
    NF_CHECK(s.active);
    NF_CHECK_EQ(s.zone_index, 1u);
    NF_CHECK_NEAR(s.wet_gain, 0.3f, 1e-6f);
    NF_CHECK_NEAR(s.decay_seconds, 1.0f, 1e-6f);

    // Two zones with equal weight: the first-listed one supplies the tail.
    ReverbZone tie[2];
    tie[0] = zones[1];
    tie[1] = zones[1];
    tie[1].wet_gain = 0.8f;
    tie[1].decay_seconds = 4.0f;
    const ReverbSample t = compute_reverb_at(tie, 2, {0, 0, 0});
    NF_CHECK_EQ(t.zone_index, 0u);
    NF_CHECK_NEAR(t.wet_gain, 0.3f, 1e-6f);
    NF_CHECK_NEAR(t.decay_seconds, 1.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// EchoProcessor — the tail itself
// ---------------------------------------------------------------------------

NF_TEST(echo_processor_places_taps_at_pre_delay_plus_n_times_spacing) {
    const ReverbZone cave = make_cave();
    const ReverbSample rv = compute_reverb_at(&cave, 1, {0, 0, 0});
    NF_CHECK(rv.active);

    EchoProcessor echo;
    echo.configure(44100, rv);

    // 0.03 s and 0.11 s at 44100 Hz, to the nearest frame.
    NF_CHECK_EQ(echo.pre_delay_frames(), 1323u);
    NF_CHECK_EQ(echo.spacing_frames(), 4851u);
    NF_CHECK_NEAR(echo.wet_gain(), 0.35f, 1e-6f);

    // feedback = 0.001 ^ (spacing / decay)
    NF_CHECK_NEAR(echo.feedback(), 0.6025596f, 1e-6f);
    // ... which is exactly "60 dB down after decay_seconds of echoes".
    NF_CHECK_NEAR(std::pow(echo.feedback(), 1.5f / 0.11f), 0.001f, 1e-4f);

    // Long enough for the fourth tap: the last assertion reads
    // wet[pre_delay + 3 * spacing] == wet[20727], so 20000 frames (which stops
    // at the third tap) walked off the end of the buffer.
    const usize frames = 21000;
    std::vector<f32> dry(frames, 0.0f);
    std::vector<f32> wet(frames, 0.0f);
    dry[0] = 1.0f; // a single click
    echo.process(dry.data(), wet.data(), frames);

    // The dry signal is NOT copied through: the first thing the listener hears
    // is the echo returning, not the click.
    NF_CHECK_EQ(first_audible(wet, 1e-7f), 6174u);

    // Tap n lands at pre_delay + n * spacing with gain wet * feedback^(n-1).
    const f32 fb = 0.6025596f;
    NF_CHECK_NEAR(wet[6174], 0.35f, 1e-5f);
    NF_CHECK_NEAR(wet[6173], 0.0f, 1e-7f);
    NF_CHECK_NEAR(wet[6175], 0.0f, 1e-7f);
    NF_CHECK_NEAR(wet[6174 + 4851], 0.35f * fb, 1e-5f);
    NF_CHECK_NEAR(wet[6174 + 2 * 4851], 0.35f * fb * fb, 1e-5f);
    NF_CHECK_NEAR(wet[6174 + 3 * 4851], 0.35f * fb * fb * fb, 1e-5f);
}

NF_TEST(echo_processor_reconfigure_moves_the_taps_to_the_new_room) {
    // A small tiled room: much shorter round trip, much shorter tail.
    ReverbZone room;
    room.position = {0, 0, 0};
    room.radius = 4.0f;
    room.inner_radius = 1.0f;
    room.wet_gain = 0.5f;
    room.decay_seconds = 0.6f;
    room.pre_delay_seconds = 0.01f;
    room.echo_spacing_seconds = 0.05f;

    EchoProcessor echo;
    echo.configure(44100, compute_reverb_at(&room, 1, {0, 0, 0}));

    NF_CHECK_EQ(echo.pre_delay_frames(), 441u);
    NF_CHECK_EQ(echo.spacing_frames(), 2205u);
    // 0.001 ^ (0.05 / 0.6) == 10 ^ (-1/4)
    NF_CHECK_NEAR(echo.feedback(), 0.5623413f, 1e-6f);

    const usize frames = 4000;
    std::vector<f32> dry(frames, 0.0f);
    std::vector<f32> wet(frames, 0.0f);
    dry[0] = 1.0f;
    echo.process(dry.data(), wet.data(), frames);

    NF_CHECK_EQ(first_audible(wet, 1e-7f), 2646u); // 441 + 2205
    NF_CHECK_NEAR(wet[2646], 0.5f, 1e-5f);
}

NF_TEST(echo_processor_zero_pre_delay_still_returns_an_echo) {
    ReverbZone z;
    z.position = {0, 0, 0};
    z.radius = 4.0f;
    z.inner_radius = 1.0f;
    z.wet_gain = 0.5f;
    z.decay_seconds = 1.0f;
    z.pre_delay_seconds = 0.0f;
    z.echo_spacing_seconds = 0.1f;

    EchoProcessor echo;
    echo.configure(44100, compute_reverb_at(&z, 1, {0, 0, 0}));

    // A zero pre-delay is one frame, not zero: a length-0 ring would read its
    // own input. The accessor reports what the processor actually does, so
    // "echo n lands at pre_delay + n * spacing" holds for every zone.
    NF_CHECK_EQ(echo.pre_delay_frames(), 1u);
    NF_CHECK_EQ(echo.spacing_frames(), 4410u);
    NF_CHECK_NEAR(echo.feedback(), 0.5011872f, 1e-6f); // 0.001 ^ (0.1 / 1.0) == 10 ^ -0.3

    const usize frames = 5000;
    std::vector<f32> dry(frames, 0.0f);
    std::vector<f32> wet(frames, 0.0f);
    dry[0] = 1.0f;
    echo.process(dry.data(), wet.data(), frames);

    NF_CHECK_EQ(first_audible(wet, 1e-7f), 4411u);
    NF_CHECK_NEAR(wet[4411], 0.5f, 1e-5f);
}

NF_TEST(echo_processor_reset_drops_the_tail_without_touching_the_config) {
    const ReverbZone cave = make_cave();
    EchoProcessor echo;
    echo.configure(44100, compute_reverb_at(&cave, 1, {0, 0, 0}));

    std::vector<f32> dry(7000, 0.0f);
    std::vector<f32> wet(7000, 0.0f);
    dry[0] = 1.0f;
    echo.process(dry.data(), wet.data(), 7000);
    NF_CHECK_NEAR(wet[6174], 0.35f, 1e-5f);

    echo.reset();
    NF_CHECK_NEAR(echo.feedback(), 0.6025596f, 1e-6f);
    NF_CHECK_EQ(echo.spacing_frames(), 4851u);

    std::vector<f32> silent(7000, 0.0f);
    std::vector<f32> after(7000, 0.0f);
    echo.process(silent.data(), after.data(), 7000);
    NF_CHECK_EQ(first_audible(after, 1e-7f), after.size());
}

NF_TEST(echo_processor_with_a_zero_sample_rate_is_silent_not_a_crash) {
    EchoProcessor echo;
    ReverbSample rv;
    rv.active = true;
    rv.wet_gain = 0.5f;
    rv.decay_seconds = 1.0f;
    rv.pre_delay_seconds = 0.03f;
    rv.echo_spacing_seconds = 0.1f;
    echo.configure(0, rv);

    NF_CHECK_EQ(echo.spacing_frames(), 0u);
    NF_CHECK_EQ(echo.pre_delay_frames(), 0u);

    std::vector<f32> dry(8, 1.0f);
    std::vector<f32> wet(8, 9.0f);
    echo.process(dry.data(), wet.data(), 8);
    for (f32 v : wet) {
        NF_CHECK_NEAR(v, 0.0f, 1e-9f);
    }
}

NF_TEST(echo_processor_zero_decay_has_no_feedback) {
    EchoProcessor echo;
    ReverbSample rv;
    rv.active = true;
    rv.wet_gain = 0.5f;
    rv.decay_seconds = 0.0f;
    rv.pre_delay_seconds = 0.02f;
    rv.echo_spacing_seconds = 0.1f;
    echo.configure(44100, rv);

    // No decay means no repeat: the first echo is the only one.
    NF_CHECK_NEAR(echo.feedback(), 0.0f, 1e-9f);
    NF_CHECK_EQ(echo.pre_delay_frames(), 882u);

    const usize frames = 12000;
    std::vector<f32> dry(frames, 0.0f);
    std::vector<f32> wet(frames, 0.0f);
    dry[0] = 1.0f;
    echo.process(dry.data(), wet.data(), frames);

    NF_CHECK_NEAR(wet[882 + 4410], 0.5f, 1e-5f);
    NF_CHECK_NEAR(wet[882 + 2 * 4410], 0.0f, 1e-7f);
}
