// AudioTests — occlusion: what a wall does to a sound behind it.
//
// Two halves, both headless: the geometry (does the listener->source segment
// cross the wall?) and the acoustics (how much does one wall dim the sound?).
// The acoustic half is pinned two ways — against the textbook one-pole
// frequency response, and against the plain-language claim a game developer
// actually cares about ("6 kHz through one wall is at most half as loud").

#include <NF/Test/TestFramework.hpp>
#include <NF/Audio/Occlusion.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::audio;

namespace {

OccluderAabb wall_at_z(f32 z) {
    OccluderAabb box;
    box.min = {-5.0f, -5.0f, z - 0.1f};
    box.max = {5.0f, 5.0f, z + 0.1f};
    return box;
}

/// Textbook one-pole low-pass magnitude response:
///   |H| = a / |1 - (1-a) e^{-jw}|,  a = 1 - e^{-wc}
/// This is the definition the difference equation in LowPassFilter is supposed
/// to realise, so comparing a measured gain against it is a real check.
f32 one_pole_gain(f32 cutoff_hz, f32 frequency_hz, u32 sample_rate) {
    const f32 wc = TWO_PI * cutoff_hz / static_cast<f32>(sample_rate);
    const f32 a = 1.0f - std::exp(-wc);
    const f32 w = TWO_PI * frequency_hz / static_cast<f32>(sample_rate);
    const f32 re = 1.0f - (1.0f - a) * std::cos(w);
    const f32 im = (1.0f - a) * std::sin(w);
    return a / std::sqrt(re * re + im * im);
}

/// Measured steady-state gain for a sine at `frequency_hz`, as the ratio of
/// output to input RMS over the second half of the run (so the transient is
/// excluded and any window error cancels — both windows are the same length).
f32 measure_gain(f32 cutoff_hz, f32 frequency_hz, u32 sample_rate,
                 usize frames) {
    LowPassFilter filter;
    filter.set_cutoff(cutoff_hz, sample_rate);

    std::vector<f32> in(frames, 0.0f);
    std::vector<f32> out(frames, 0.0f);
    for (usize i = 0; i < frames; ++i) {
        in[i] = std::sin(TWO_PI * frequency_hz * static_cast<f32>(i) /
                         static_cast<f32>(sample_rate));
    }
    filter.process_buffer(in.data(), out.data(), frames);

    f64 sum_in = 0.0;
    f64 sum_out = 0.0;
    for (usize i = frames / 2; i < frames; ++i) {
        sum_in += static_cast<f64>(in[i]) * static_cast<f64>(in[i]);
        sum_out += static_cast<f64>(out[i]) * static_cast<f64>(out[i]);
    }
    if (sum_in <= 0.0) {
        return 0.0f;
    }
    return static_cast<f32>(std::sqrt(sum_out / sum_in));
}

} // namespace

// ---------------------------------------------------------------------------
// Geometry — the slab test
// ---------------------------------------------------------------------------

NF_TEST(occluder_segment_crosses_a_wall_between_the_two_points) {
    const OccluderAabb wall = wall_at_z(5.0f);
    NF_CHECK(segment_intersects_aabb({0, 0, 0}, {0, 0, 10}, wall));
    NF_CHECK(segment_intersects_aabb({0, 0, 10}, {0, 0, 0}, wall));
    // A listener standing in the doorway is still behind that wall.
    NF_CHECK(segment_intersects_aabb({0, 0, 4.9f}, {0, 0, 10}, wall));
}

NF_TEST(occluder_segment_misses_a_wall_it_does_not_pass_through) {
    const OccluderAabb wall = wall_at_z(5.0f);
    // Off to the side.
    NF_CHECK(!segment_intersects_aabb({0, 0, 0}, {0, 0, 10},
                                      {{20, -5, 4.9f}, {30, 5, 5.1f}}));
    // Behind the listener, in the opposite direction.
    NF_CHECK(!segment_intersects_aabb({0, 0, 0}, {0, 0, 10},
                                      {{-5, -5, -2}, {5, 5, -1}}));
    // Parallel to the wall plane but above it.
    NF_CHECK(!segment_intersects_aabb({0, 10, 0}, {0, 10, 10}, wall));
    // The segment stops short of the wall.
    NF_CHECK(!segment_intersects_aabb({0, 0, 0}, {0, 0, 4.0f}, wall));
}

NF_TEST(occluder_degenerate_segments_are_handled) {
    const OccluderAabb wall = wall_at_z(5.0f);
    // Zero-length segment inside the box.
    NF_CHECK(segment_intersects_aabb({0, 0, 5}, {0, 0, 5}, wall));
    // Zero-length segment outside it.
    NF_CHECK(!segment_intersects_aabb({0, 0, 50}, {0, 0, 50}, wall));
    // Segment wholly inside a large box.
    NF_CHECK(segment_intersects_aabb({0, 0, 0}, {0, 0, 1},
                                     {{-5, -5, -5}, {5, 5, 5}}));
}

NF_TEST(occluder_count_counts_every_wall_between_listener_and_source) {
    const OccluderAabb walls[3] = {wall_at_z(5.0f), wall_at_z(7.0f),
                                   {{20, -5, 4.9f}, {30, 5, 5.1f}}};
    NF_CHECK_EQ(count_occluders_crossed({0, 0, 0}, {0, 0, 10}, walls, 3), 2u);
    NF_CHECK_EQ(count_occluders_crossed({0, 0, 0}, {0, 0, 10}, walls, 2), 2u);
    NF_CHECK_EQ(count_occluders_crossed({0, 0, 0}, {0, 0, 10}, walls, 1), 1u);
    NF_CHECK_EQ(count_occluders_crossed({0, 0, 0}, {0, 0, 10}, nullptr, 0), 0u);
    // Open air: the listener and the source are on the same side of everything.
    NF_CHECK_EQ(count_occluders_crossed({0, 0, 0}, {0, 0, 3}, walls, 3), 0u);
}

// ---------------------------------------------------------------------------
// Acoustics — openness and cutoff
// ---------------------------------------------------------------------------

NF_TEST(occlusion_amount_halves_the_openness_per_wall) {
    NF_CHECK_NEAR(occlusion_amount(0), 0.0f, 1e-6f);
    NF_CHECK_NEAR(occlusion_amount(1), 0.5f, 1e-6f);
    NF_CHECK_NEAR(occlusion_amount(2), 0.75f, 1e-6f);
    NF_CHECK_NEAR(occlusion_amount(3), 0.875f, 1e-6f);
    NF_CHECK_NEAR(occlusion_amount(4), 0.9375f, 1e-6f);
    NF_CHECK_NEAR(kPerWallMuffle, 0.5f, 1e-6f);

    // More walls are always more occluded, and never fully closed.
    NF_CHECK(occlusion_amount(10) > occlusion_amount(3));
    NF_CHECK(occlusion_amount(10) < 1.0f);
}

NF_TEST(occlusion_cutoff_falls_exponentially_with_occlusion) {
    NF_CHECK_NEAR(occlusion_lowpass_cutoff(0.0f), kOcclusionOpenCutoffHz, 1e-3f);
    NF_CHECK_NEAR(occlusion_lowpass_cutoff(1.0f), kOcclusionClosedCutoffHz,
                  1e-3f);
    NF_CHECK_NEAR(occlusion_lowpass_cutoff(0.5f), 2645.7513f, 1e-2f);
    NF_CHECK_NEAR(occlusion_lowpass_cutoff(0.25f), 20000.0f * std::sqrt(
                      std::sqrt(kOcclusionClosedCutoffHz / kOcclusionOpenCutoffHz)),
                  1e-2f);

    // Out-of-range occlusion is clamped, not extrapolated into a boost.
    NF_CHECK_NEAR(occlusion_lowpass_cutoff(-1.0f), kOcclusionOpenCutoffHz, 1e-3f);
    NF_CHECK_NEAR(occlusion_lowpass_cutoff(2.0f), kOcclusionClosedCutoffHz, 1e-3f);

    NF_CHECK_NEAR(kOcclusionOpenCutoffHz, 20000.0f, 1e-6f);
    NF_CHECK_NEAR(kOcclusionClosedCutoffHz, 350.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// The filter itself
// ---------------------------------------------------------------------------

NF_TEST(lowpass_passes_dc_at_unity_and_the_cutoff_at_minus_three_db) {
    // DC is untouched: the one-pole's gain at 0 Hz is exactly 1.
    LowPassFilter filter;
    filter.set_cutoff(1000.0f, 44100);
    f32 last = 0.0f;
    for (u32 i = 0; i < 200; ++i) {
        last = filter.process(1.0f);
    }
    NF_CHECK_NEAR(last, 1.0f, 1e-6f);

    // At the cutoff the response is the textbook -3 dB.
    const f32 at_cutoff = measure_gain(1000.0f, 1000.0f, 44100, 44100);
    NF_CHECK_NEAR(at_cutoff, 0.70710678f, 0.02f);
}

NF_TEST(lowpass_step_response_follows_the_closed_form) {
    // y[n] = 1 - (1 - a)^(n + 1) for a unit step, which is what the header
    // promises and what makes the filter's behaviour predictable.
    LowPassFilter filter;
    filter.set_cutoff(1000.0f, 44100);
    const f32 a = filter.coefficient();
    NF_CHECK(a > 0.0f);
    NF_CHECK(a < 1.0f);

    for (u32 n = 0; n < 64; ++n) {
        const f32 y = filter.process(1.0f);
        const f32 expected =
            1.0f - std::pow(1.0f - a, static_cast<f32>(n + 1));
        NF_CHECK_NEAR(y, expected, 1e-6f);
    }
}

NF_TEST(lowpass_matches_the_textbook_response_and_muffles_highs) {
    // One wall between the listener and the source.
    const f32 cutoff = occlusion_lowpass_cutoff(occlusion_amount(1));
    const usize frames = 44100;

    const f32 low = measure_gain(cutoff, 200.0f, 44100, frames);
    const f32 high = measure_gain(cutoff, 6000.0f, 44100, frames);

    // The implementation realises the textbook response.
    NF_CHECK_NEAR(low, one_pole_gain(cutoff, 200.0f, 44100), 0.01f);
    NF_CHECK_NEAR(high, one_pole_gain(cutoff, 6000.0f, 44100), 0.01f);

    // ... and that response is a muffle: a 200 Hz rumble comes through, a
    // 6 kHz hiss does not.
    NF_CHECK(low > 0.99f);
    NF_CHECK(high < 0.45f);
    NF_CHECK(high < low * 0.45f);

    // A second wall cuts further still.
    const f32 cutoff2 = occlusion_lowpass_cutoff(occlusion_amount(2));
    const f32 high2 = measure_gain(cutoff2, 6000.0f, 44100, frames);
    NF_CHECK(high2 < high);
    NF_CHECK_NEAR(high2, one_pole_gain(cutoff2, 6000.0f, 44100), 0.01f);
}

NF_TEST(lowpass_keeps_its_state_across_blocks) {
    // The scene filters one block at a time; if the state reset per block, a
    // steady tone would re-swing from zero every frame and click.
    LowPassFilter split;
    split.set_cutoff(1000.0f, 44100);
    f32 split_last = 0.0f;
    for (u32 block = 0; block < 4; ++block) {
        for (u32 i = 0; i < 25; ++i) {
            split_last = split.process(1.0f);
        }
    }

    LowPassFilter whole;
    whole.set_cutoff(1000.0f, 44100);
    f32 whole_last = 0.0f;
    for (u32 i = 0; i < 100; ++i) {
        whole_last = whole.process(1.0f);
    }

    NF_CHECK_NEAR(split_last, whole_last, 1e-6f);
    NF_CHECK_NEAR(split.state(), whole_last, 1e-6f);
}

NF_TEST(lowpass_degenerate_input_passes_audio_through) {
    // A non-positive cutoff or a zero sample rate must not silence the source:
    // a bad scene field should be audible-but-wrong, never a dead game.
    LowPassFilter no_cutoff;
    no_cutoff.set_cutoff(0.0f, 44100);
    NF_CHECK_NEAR(no_cutoff.coefficient(), 1.0f, 1e-6f);
    NF_CHECK_NEAR(no_cutoff.process(0.25f), 0.25f, 1e-6f);

    LowPassFilter no_rate;
    no_rate.set_cutoff(1000.0f, 0);
    NF_CHECK_NEAR(no_rate.coefficient(), 1.0f, 1e-6f);

    LowPassFilter negative;
    negative.set_cutoff(-500.0f, 44100);
    NF_CHECK_NEAR(negative.coefficient(), 1.0f, 1e-6f);

    // A very high cutoff is still a filter, not an overflow.
    LowPassFilter wide_open;
    wide_open.set_cutoff(1e9f, 44100);
    NF_CHECK(wide_open.coefficient() > 0.99f);
    NF_CHECK(wide_open.coefficient() <= 1.0f);
}

NF_TEST(lowpass_reset_clears_the_held_sample) {
    LowPassFilter filter;
    filter.set_cutoff(1000.0f, 44100);
    filter.process(1.0f);
    NF_CHECK(filter.state() > 0.0f);
    filter.reset();
    NF_CHECK_NEAR(filter.state(), 0.0f, 1e-9f);
    // The coefficient survives a reset — only the signal state is dropped.
    NF_CHECK(filter.coefficient() > 0.0f);
    NF_CHECK(filter.coefficient() < 1.0f);
}
