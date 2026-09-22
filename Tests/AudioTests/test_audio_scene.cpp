// AudioTests — the AudioScene walkthrough, headless.
//
// This is the acceptance case for G5 in test form: a cave you can hear echo, a
// wall that muffles what is behind it, menu music that fades in, and sliders
// that change the mix — all through the same public API a game uses, with no
// audio hardware involved. Every number below is a real sample from the mixed
// block, so "the cave echoes" is a position in the buffer, not an adjective.

#include <NF/Test/TestFramework.hpp>
#include <NF/Audio/AudioScene.hpp>

#include <cmath>
#include <string>
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

/// The cave the walkthrough demo walks into.
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

OccluderAabb wall_at_z(f32 z) {
    OccluderAabb box;
    box.min = {-5.0f, -5.0f, z - 0.1f};
    box.max = {5.0f, 5.0f, z + 0.1f};
    return box;
}

AudioListener listener_at(const Vec3& position) {
    AudioListener listener;
    listener.position = position;
    listener.forward = {0, 0, -1};
    listener.up = {0, 1, 0};
    return listener;
}

/// Mix one emitter through the scene and return its measured gain: output RMS
/// over input RMS, both taken from the second half of the block so the filter
/// transient is excluded and the window error cancels out.
f32 measure_gain(AudioScene& scene, Emitter& emitter,
                 const std::vector<f32>& reference, usize frames) {
    emitter.playing = true;
    emitter.sample_cursor = 0;

    scene.begin_block(frames);
    scene.mix_emitter(emitter);

    std::vector<f32> left(frames, 0.0f);
    std::vector<f32> right(frames, 0.0f);
    scene.finalize(left.data(), right.data());

    f64 sum_in = 0.0;
    f64 sum_out = 0.0;
    for (usize i = frames / 2; i < frames; ++i) {
        sum_in += static_cast<f64>(reference[i]) * static_cast<f64>(reference[i]);
        sum_out += static_cast<f64>(left[i]) * static_cast<f64>(left[i]);
    }
    if (sum_in <= 0.0) {
        return 0.0f;
    }
    return static_cast<f32>(std::sqrt(sum_out / sum_in));
}

/// One block of music/ambience mixed with nothing else, returning the left
/// channel. Used to read the sliders back out of the mix.
std::vector<f32> mix_silent_block(AudioScene& scene, usize frames) {
    scene.begin_block(frames);
    std::vector<f32> left(frames, 0.0f);
    std::vector<f32> right(frames, 0.0f);
    scene.finalize(left.data(), right.data());
    return left;
}

} // namespace

// ---------------------------------------------------------------------------
// Cave echoes
// ---------------------------------------------------------------------------

NF_TEST(scene_cave_returns_echoes_at_the_zone_spacing) {
    AudioScene scene;
    scene.set_listener(listener_at({0, 0, 0}));
    NF_CHECK_EQ(scene.add_zone(make_cave()), 0u);
    NF_CHECK_EQ(scene.zone_count(), 1u);
    NF_CHECK_NEAR(scene.zone(0).wet_gain, 0.35f, 1e-6f);

    // One click, one frame long.
    AudioBuffer click;
    click.channels = 1;
    click.sample_rate = 44100;
    click.samples.assign(1, 1.0f);

    Emitter click_src;
    click_src.buffer = &click;
    click_src.playing = true;

    const usize frames = 20000;
    scene.begin_block(frames);
    NF_CHECK_EQ(scene.block_frames(), frames);
    NF_CHECK_EQ(scene.sample_rate(), 44100u);
    scene.mix_emitter(click_src);

    std::vector<f32> left(frames, 0.0f);
    std::vector<f32> right(frames, 0.0f);
    scene.finalize(left.data(), right.data());

    NF_CHECK(scene.reverb().active);
    NF_CHECK_EQ(scene.reverb().zone_index, 0u);

    // The click itself, then the cave answering at 0.03 s + n * 0.11 s.
    const f32 fb = 0.6025596f;
    NF_CHECK_NEAR(left[0], 1.0f, 1e-5f);
    NF_CHECK_NEAR(left[6173], 0.0f, 1e-7f);
    NF_CHECK_NEAR(left[6174], 0.35f, 1e-5f);
    NF_CHECK_NEAR(left[11025], 0.35f * fb, 1e-5f);
    NF_CHECK_NEAR(left[15876], 0.35f * fb * fb, 1e-5f);

    // A mono click and a mono tail reach both ears.
    NF_CHECK_NEAR(right[0], 1.0f, 1e-5f);
    NF_CHECK_NEAR(right[6174], 0.35f, 1e-5f);

    // Step outside the zone: the same click is dry, with no tail at all.
    scene.set_listener(listener_at({0, 0, 100}));
    click_src.playing = true;
    click_src.sample_cursor = 0;
    scene.begin_block(frames);
    scene.mix_emitter(click_src);
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    scene.finalize(left.data(), right.data());

    NF_CHECK(!scene.reverb().active);
    NF_CHECK_NEAR(left[0], 1.0f, 1e-5f);
    NF_CHECK_NEAR(left[6174], 0.0f, 1e-7f);
    NF_CHECK_NEAR(left[11025], 0.0f, 1e-7f);
}

NF_TEST(scene_cave_and_wall_compose_muffled_echoes) {
    AudioScene scene;
    scene.set_listener(listener_at({0, 0, 0}));
    scene.add_zone(make_cave());
    scene.add_occluder(wall_at_z(5.0f));

    AudioBuffer click;
    click.channels = 1;
    click.sample_rate = 44100;
    click.samples.assign(1, 1.0f);

    Emitter source;
    source.buffer = &click;
    source.playing = true;
    source.position = {0, 0, 10}; // behind the wall, inside the cave

    const usize frames = 20000;
    scene.begin_block(frames);
    scene.mix_emitter(source);
    std::vector<f32> left(frames, 0.0f);
    std::vector<f32> right(frames, 0.0f);
    scene.finalize(left.data(), right.data());

    NF_CHECK_EQ(source.walls_last, 1u);
    NF_CHECK_NEAR(source.occlusion_last, 0.5f, 1e-6f);
    NF_CHECK(scene.reverb().active);

    // One wall turns the click into the one-pole's impulse response
    // h[n] = a * (1 - a)^n, and the cave then echoes *that*.
    const f32 cutoff = occlusion_lowpass_cutoff(source.occlusion_last);
    const f32 a = 1.0f - std::exp(-TWO_PI * cutoff / 44100.0f);

    NF_CHECK_NEAR(left[0], a, 1e-5f);
    NF_CHECK_NEAR(left[6174], 0.35f * a, 1e-5f);
    NF_CHECK_NEAR(left[6175], 0.35f * a * (1.0f - a), 1e-5f);

    // The wall's muffling is real: a click that was full scale is now well
    // under half scale even before the echo.
    NF_CHECK(left[0] < 0.5f);
}

// ---------------------------------------------------------------------------
// Wall muffle
// ---------------------------------------------------------------------------

NF_TEST(scene_wall_muffles_a_source_behind_it) {
    AudioScene scene;
    scene.set_listener(listener_at({0, 0, 0}));

    const AudioBuffer hiss = make_tone_buffer(6000.0f, 1.0f, 44100, 1);
    const AudioBuffer rumble = make_tone_buffer(200.0f, 1.0f, 44100, 1);
    const usize frames = 44100;

    Emitter source;
    source.buffer = &hiss;
    source.volume = 1.0f;
    source.position = {0, 0, 10};

    // Open air: the sound arrives unmodified, and there is no filter in the
    // path at all (a 20 kHz one-pole would still colour it).
    const f32 open_gain = measure_gain(scene, source, hiss.samples, frames);
    NF_CHECK_NEAR(open_gain, 1.0f, 1e-4f);
    NF_CHECK_EQ(source.walls_last, 0u);
    NF_CHECK_NEAR(source.occlusion_last, 0.0f, 1e-6f);

    // One wall.
    scene.add_occluder(wall_at_z(5.0f));
    const f32 one_wall = measure_gain(scene, source, hiss.samples, frames);
    NF_CHECK_EQ(source.walls_last, 1u);
    NF_CHECK_NEAR(source.occlusion_last, 0.5f, 1e-6f);
    NF_CHECK(one_wall < 0.45f);

    // A low rumble behind the same wall still comes through.
    source.buffer = &rumble;
    const f32 rumble_gain = measure_gain(scene, source, rumble.samples, frames);
    NF_CHECK(rumble_gain > 0.95f);
    NF_CHECK(rumble_gain > one_wall * 2.0f);

    // Two walls cut deeper still.
    scene.add_occluder(wall_at_z(7.0f));
    source.buffer = &hiss;
    const f32 two_walls = measure_gain(scene, source, hiss.samples, frames);
    NF_CHECK_EQ(source.walls_last, 2u);
    NF_CHECK_NEAR(source.occlusion_last, 0.75f, 1e-6f);
    NF_CHECK(two_walls < one_wall);
    NF_CHECK(two_walls < one_wall * 0.5f);

    // A sound authored to ignore geometry is not filtered, walls or not.
    source.occluded = false;
    const f32 bypass = measure_gain(scene, source, hiss.samples, frames);
    NF_CHECK_NEAR(bypass, 1.0f, 1e-4f);
    NF_CHECK_EQ(source.walls_last, 0u);
    NF_CHECK_NEAR(source.occlusion_last, 0.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// Menu music and the sliders
// ---------------------------------------------------------------------------

NF_TEST(scene_menu_music_fades_in_and_the_sliders_move_the_mix) {
    AudioScene scene;
    scene.set_listener(listener_at({0, 0, 0}));

    const AudioBuffer menu = make_constant(1.0f, 44100);
    MusicTrack track;
    track.buffer = &menu;
    track.base_volume = 0.5f;
    scene.music().play(track, 0.5f); // a half-second fade-in

    const usize block = 4410; // 0.1 s at 44100

    // Block 1: the envelope is 0.2 of the way up.
    // 1.0 (bed) * 0.5 (base) * 0.2 (envelope) * 0.8 (music slider) = 0.08
    std::vector<f32> left = mix_silent_block(scene, block);
    NF_CHECK_NEAR(scene.music().current_gain(), 0.2f, 1e-5f);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.08f, 1e-5f);
    }

    // Block 2: 0.4 up the ramp.
    left = mix_silent_block(scene, block);
    NF_CHECK_NEAR(scene.music().current_gain(), 0.4f, 1e-5f);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.16f, 1e-5f);
    }

    // The Settings window drags the music slider down to 0.4.
    NF_CHECK(scene.settings().set_volume(BusId::Music, 0.4f));
    left = mix_silent_block(scene, block);
    NF_CHECK_NEAR(scene.music().current_gain(), 0.6f, 1e-5f);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.12f, 1e-5f);
    }

    // Muting the bus silences the music without pausing it: the envelope
    // keeps running, so unmuting does not restart the fade.
    NF_CHECK(scene.settings().set_volume(BusId::Music, 0.0f));
    left = mix_silent_block(scene, block);
    NF_CHECK_NEAR(scene.music().current_gain(), 0.8f, 1e-5f);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.0f, 1e-9f);
    }

    // The master slider is a second, independent stage.
    NF_CHECK(scene.settings().set_volume(BusId::Music, 1.0f));
    NF_CHECK(scene.settings().set_volume(BusId::Master, 0.0f));
    left = mix_silent_block(scene, block);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.0f, 1e-9f);
    }

    NF_CHECK(scene.settings().set_volume(BusId::Master, 1.0f));
    left = mix_silent_block(scene, block); // the ramp has topped out
    NF_CHECK_NEAR(scene.music().current_gain(), 1.0f, 1e-6f);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.5f, 1e-5f);
    }
}

NF_TEST(scene_ambience_rides_its_own_bus_and_slider) {
    AudioScene scene;
    scene.set_listener(listener_at({0, 0, 0}));

    const AudioBuffer menu = make_constant(1.0f, 44100);
    const AudioBuffer drips = make_constant(1.0f, 44100);
    MusicTrack track;
    track.buffer = &menu;
    track.base_volume = 0.5f;
    scene.music().play(track, 0.0f); // full level immediately
    scene.music().set_ambience(&drips, 0.0f);

    const usize block = 4410;

    // Music 0.5 * 0.8 (music bus) + ambience 1.0 * 0.7 (ambience bus) = 1.1.
    std::vector<f32> left = mix_silent_block(scene, block);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.4f + 0.7f, 1e-5f);
    }

    // Fading the ambience bus leaves the music untouched.
    NF_CHECK(scene.settings().set_volume(BusId::Ambience, 0.0f));
    left = mix_silent_block(scene, block);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.4f, 1e-5f);
    }

    // Clearing the bed fades it out; the music still plays.
    scene.music().clear_ambience(0.0f);
    left = mix_silent_block(scene, block);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.4f, 1e-5f);
    }
    NF_CHECK(!scene.music().has_ambience());
}

NF_TEST(scene_routes_emitters_onto_the_bus_they_name) {
    AudioScene scene;
    scene.set_listener(listener_at({0, 0, 0}));

    const AudioBuffer buf = make_constant(1.0f, 64);
    Emitter sfx;
    sfx.buffer = &buf;
    sfx.playing = true;
    sfx.bus = BusId::Sfx;
    Emitter voice;
    voice.buffer = &buf;
    voice.playing = true;
    voice.bus = BusId::Voice;

    scene.settings().set_volume(BusId::Voice, 0.25f);

    const usize frames = 8;
    scene.begin_block(frames);
    scene.mix_emitter(sfx);
    scene.mix_emitter(voice);
    std::vector<f32> left(frames, 0.0f);
    std::vector<f32> right(frames, 0.0f);
    scene.finalize(left.data(), right.data());

    // 1.0 on the sfx bus + 0.25 on the voice bus.
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 1.25f, 1e-5f);
    }

    // Muting the voice bus leaves the sfx source alone.
    NF_CHECK(scene.settings().set_volume(BusId::Voice, 0.0f));
    scene.begin_block(frames);
    sfx.playing = true;
    voice.playing = true;
    scene.mix_emitter(sfx);
    scene.mix_emitter(voice);
    std::fill(left.begin(), left.end(), 0.0f);
    scene.finalize(left.data(), right.data());
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 1.0f, 1e-5f);
    }
}

// ---------------------------------------------------------------------------
// The settings file contract, end to end
// ---------------------------------------------------------------------------

NF_TEST(scene_volume_settings_survive_a_save_and_reload) {
    AudioScene scene;
    NF_CHECK(scene.settings().set_volume(BusId::Master, 0.9f));
    NF_CHECK(scene.settings().set_volume(BusId::Music, 0.35f));
    NF_CHECK(scene.settings().set_volume(BusId::Sfx, 0.6f));
    NF_CHECK(scene.settings().set_volume(BusId::Ambience, 0.15f));
    NF_CHECK(scene.settings().set_volume(BusId::Voice, 0.75f));

    std::string text;
    scene.settings().append_settings_text(text);

    AudioScene reopened;
    usize applied = 0;
    usize cursor = 0;
    while (cursor < text.size()) {
        const usize end = text.find('\n', cursor);
        const std::string line = text.substr(cursor, end - cursor);
        const usize eq = line.find('=');
        if (eq != std::string::npos &&
            reopened.settings().apply_setting(line.substr(0, eq),
                                              line.substr(eq + 1))) {
            ++applied;
        }
        cursor = (end == std::string::npos) ? text.size() : end + 1;
    }

    NF_CHECK_EQ(applied, static_cast<usize>(kBusCount));
    for (u32 i = 0; i < kBusCount; ++i) {
        const BusId id = static_cast<BusId>(i);
        NF_CHECK(reopened.settings().volume(id) == scene.settings().volume(id));
        // Re-applying a reloaded value is not a change: the file round-tripped
        // bit-exactly, so a Settings window shows no unsaved edit.
        NF_CHECK(!reopened.settings().set_volume(
            id, scene.settings().volume(id)));
    }
}

// ---------------------------------------------------------------------------
// The walkthrough, as a walk: open air -> cave edge -> cave heart -> out again
// ---------------------------------------------------------------------------

NF_TEST(scene_walking_into_the_cave_grows_the_echo_and_the_tail_survives) {
    AudioScene scene;
    scene.add_zone(make_cave());

    AudioBuffer click;
    click.channels = 1;
    click.sample_rate = 44100;
    click.samples.assign(1, 1.0f);

    Emitter src;
    src.buffer = &click;

    const usize frames = 8000; // long enough to hold the first tap at 6174
    std::vector<f32> left(frames, 0.0f);
    std::vector<f32> right(frames, 0.0f);

    // --- 1. Open air: outside the cave radius the click is dry, no tail. ----
    scene.set_listener(listener_at({0, 0, 30}));
    src.playing = true;
    src.sample_cursor = 0;
    scene.begin_block(frames);
    scene.mix_emitter(src);
    scene.finalize(left.data(), right.data());

    NF_CHECK(!scene.reverb().active);
    NF_CHECK_NEAR(left[0], 1.0f, 1e-5f);
    NF_CHECK_NEAR(left[6174], 0.0f, 1e-7f);

    // --- 2. Half way across the falloff band: the cave answers at half wet. -
    scene.set_listener(listener_at({0, 0, 12.5f}));
    src.playing = true;
    src.sample_cursor = 0;
    scene.begin_block(frames);
    scene.mix_emitter(src);
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    scene.finalize(left.data(), right.data());

    NF_CHECK(scene.reverb().active);
    NF_CHECK_NEAR(scene.reverb().wet_gain, 0.175f, 1e-5f);
    NF_CHECK_NEAR(left[0], 1.0f, 1e-5f);
    NF_CHECK_NEAR(left[6174], 0.175f, 1e-5f);

    // --- 3. Step to the heart of the cave. Nothing is played this block, so
    //     everything audible is the step-2 click's tail still ringing — the
    //     walk must not restart the echo. The tail is scaled by the deeper
    //     zone weight (0.35, not 0.175): the contract is that the weight rides
    //     on wet_gain per block, so walking does not disturb the comb.
    scene.set_listener(listener_at({0, 0, 0}));
    scene.begin_block(frames);
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    scene.finalize(left.data(), right.data());

    NF_CHECK(scene.reverb().active);
    NF_CHECK_NEAR(scene.reverb().wet_gain, 0.35f, 1e-5f);
    const f32 fb = 0.6025596f;
    NF_CHECK_NEAR(left[0], 0.0f, 1e-9f);
    NF_CHECK_NEAR(left[3025], 0.35f * fb, 1e-5f);       // 2nd tap, carried over
    NF_CHECK_NEAR(left[7876], 0.35f * fb * fb, 1e-5f);  // 3rd tap

    // --- 4. Walk back out: the tail is dropped, not left ringing into open
    //     air where there is no room to answer.
    scene.set_listener(listener_at({0, 0, 30}));
    scene.begin_block(frames);
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    scene.finalize(left.data(), right.data());

    NF_CHECK(!scene.reverb().active);
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.0f, 1e-9f);
    }
}

// ---------------------------------------------------------------------------
// Degenerate input
// ---------------------------------------------------------------------------

NF_TEST(scene_degenerate_calls_are_no_ops_not_crashes) {
    AudioScene scene;

    // finalize() before any begin_block() leaves the output untouched.
    std::vector<f32> left(4, 7.0f);
    std::vector<f32> right(4, 7.0f);
    scene.finalize(left.data(), right.data());
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 7.0f, 1e-9f);
    }

    // A zero-frame block is a no-op too.
    scene.begin_block(0);
    NF_CHECK_EQ(scene.block_frames(), 0u);
    scene.finalize(left.data(), right.data());
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 7.0f, 1e-9f);
    }

    // An emitter with no buffer contributes silence, and its diagnostics are
    // cleared rather than left over from the previous block.
    scene.begin_block(4);
    Emitter empty;
    empty.walls_last = 9;
    empty.occlusion_last = 9.0f;
    scene.mix_emitter(empty);
    NF_CHECK_EQ(empty.walls_last, 0u);
    NF_CHECK_NEAR(empty.occlusion_last, 0.0f, 1e-6f);

    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    scene.finalize(left.data(), right.data());
    for (f32 v : left) {
        NF_CHECK_NEAR(v, 0.0f, 1e-9f);
    }
    NF_CHECK(!scene.reverb().active);

    // A zero sample rate falls back to the engine default instead of dividing
    // by zero in the music envelope.
    scene.begin_block(4, 0);
    NF_CHECK_EQ(scene.sample_rate(), kDefaultSampleRate);

    // Authoring state can be torn down.
    scene.add_zone(make_cave());
    scene.add_occluder(wall_at_z(5.0f));
    NF_CHECK_EQ(scene.zone_count(), 1u);
    NF_CHECK_EQ(scene.occluder_count(), 1u);
    scene.clear_zones();
    scene.clear_occluders();
    NF_CHECK_EQ(scene.zone_count(), 0u);
    NF_CHECK_EQ(scene.occluder_count(), 0u);
}
