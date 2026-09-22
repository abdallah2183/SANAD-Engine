// AudioTests — mix buses and the volume-settings contract.
//
// `AudioVolumeSettings` is THE object a Settings window binds, so its shape is
// a published contract: five named buses, defaults a shipped game ships with,
// a clamp that refuses amplification, and a `key=value` persistence format a
// settings file can round-trip bit-exactly. `BusMixer` is the half that makes
// a slider audible: bus volume * master volume applied once per block.

#include <NF/Test/TestFramework.hpp>
#include <NF/Audio/AudioEngine.hpp>
#include <NF/Audio/Buses.hpp>

#include <string>
#include <vector>

using namespace nf;
using namespace nf::audio;

namespace {

AudioBuffer make_constant(f32 value, usize frames, u32 channels = 1) {
    AudioBuffer buf;
    buf.channels = channels;
    buf.sample_rate = 44100;
    buf.samples.assign(frames * channels, value);
    return buf;
}

/// Feed one mono constant source into `bus` for a block and return the summed
/// left channel, with the settings volumes applied exactly as a frame would.
std::vector<f32> mix_one(const AudioVolumeSettings& settings, BusId bus,
                         f32 value, usize frames) {
    AudioBuffer buf = make_constant(value, frames);
    AudioSource src;
    src.buffer = &buf;
    src.playing = true;
    src.looping = true;

    BusMixer mixer;
    mixer.set_settings(&settings);
    mixer.begin_block(frames);
    mixer.mix_source(src, {0, 0, 0}, {0, 0, -1}, {0, 1, 0}, frames, 44100, bus);

    std::vector<f32> left(frames, 0.0f);
    std::vector<f32> right(frames, 0.0f);
    mixer.finalize(left.data(), right.data(), frames);
    return left;
}

} // namespace

// ---------------------------------------------------------------------------
// Bus identity
// ---------------------------------------------------------------------------

NF_TEST(bus_names_round_trip_for_every_bus) {
    NF_CHECK_EQ(kBusCount, 5u);
    const char* expected[kBusCount] = {"master", "music", "sfx", "ambience",
                                       "voice"};
    for (u32 i = 0; i < kBusCount; ++i) {
        const BusId id = static_cast<BusId>(i);
        NF_CHECK(std::string(bus_name(id)) == expected[i]);

        BusId parsed = BusId::Sfx;
        NF_CHECK(bus_id_from_name(bus_name(id), parsed));
        NF_CHECK(parsed == id);
    }

    BusId unused = BusId::Master;
    NF_CHECK(!bus_id_from_name("reverb", unused));
    NF_CHECK(!bus_id_from_name("", unused));
    NF_CHECK(!bus_id_from_name("Master", unused)); // settings keys are lowercase
}

// ---------------------------------------------------------------------------
// Defaults, clamping, reset
// ---------------------------------------------------------------------------

NF_TEST(volume_settings_ship_with_the_documented_defaults) {
    AudioVolumeSettings s;
    NF_CHECK_NEAR(s.volume(BusId::Master), 1.0f, 1e-6f);
    NF_CHECK_NEAR(s.volume(BusId::Music), 0.8f, 1e-6f);
    NF_CHECK_NEAR(s.volume(BusId::Sfx), 1.0f, 1e-6f);
    NF_CHECK_NEAR(s.volume(BusId::Ambience), 0.7f, 1e-6f);
    NF_CHECK_NEAR(s.volume(BusId::Voice), 1.0f, 1e-6f);
}

NF_TEST(volume_setter_clamps_and_reports_a_real_change) {
    AudioVolumeSettings s;

    // Already at the default: no change, so an Apply button stays disabled.
    NF_CHECK(!s.set_volume(BusId::Music, 0.8f));

    // A slider that overshoots must never amplify: the buses feed a sum.
    NF_CHECK(s.set_volume(BusId::Music, 2.0f));
    NF_CHECK_NEAR(s.volume(BusId::Music), 1.0f, 1e-6f);
    NF_CHECK(s.set_volume(BusId::Music, -3.0f));
    NF_CHECK_NEAR(s.volume(BusId::Music), 0.0f, 1e-6f);
    NF_CHECK(!s.set_volume(BusId::Music, 0.0f));

    s.reset_to_defaults();
    NF_CHECK_NEAR(s.volume(BusId::Music), 0.8f, 1e-6f);

    // An out-of-range bus id is refused rather than writing out of bounds.
    const BusId bogus = static_cast<BusId>(99);
    NF_CHECK(!s.set_volume(bogus, 0.5f));
    NF_CHECK_NEAR(s.volume(bogus), 1.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// Persistence — the settings-file half of the Settings contract
// ---------------------------------------------------------------------------

NF_TEST(volume_settings_serialize_the_documented_format) {
    AudioVolumeSettings s;
    std::string text;
    s.append_settings_text(text);

    // 9 significant digits, one line per bus, in BusId order. f32 0.8 is
    // 0.800000012 as a double, which is what makes the re-read bit-exact.
    const std::string expected =
        "audio.volume.master=1\n"
        "audio.volume.music=0.800000012\n"
        "audio.volume.sfx=1\n"
        "audio.volume.ambience=0.699999988\n"
        "audio.volume.voice=1\n";
    NF_CHECK(text == expected);
}

NF_TEST(volume_settings_round_trip_is_bit_exact) {
    AudioVolumeSettings saved;
    saved.set_volume(BusId::Master, 0.37f);
    saved.set_volume(BusId::Music, 0.111f);
    saved.set_volume(BusId::Sfx, 0.999f);
    saved.set_volume(BusId::Ambience, 0.0f);
    saved.set_volume(BusId::Voice, 0.5f);

    std::string text;
    saved.append_settings_text(text);

    AudioVolumeSettings loaded;
    usize applied = 0;
    usize cursor = 0;
    while (cursor < text.size()) {
        const usize end = text.find('\n', cursor);
        const std::string line = text.substr(cursor, end - cursor);
        const usize eq = line.find('=');
        if (eq != std::string::npos) {
            if (loaded.apply_setting(line.substr(0, eq), line.substr(eq + 1))) {
                ++applied;
            }
        }
        cursor = (end == std::string::npos) ? text.size() : end + 1;
    }

    NF_CHECK_EQ(applied, static_cast<usize>(kBusCount));
    for (u32 i = 0; i < kBusCount; ++i) {
        const BusId id = static_cast<BusId>(i);
        NF_CHECK(loaded.volume(id) == saved.volume(id));
    }
}

NF_TEST(volume_settings_refuse_keys_they_do_not_own) {
    AudioVolumeSettings s;

    // Another system's key must come back false so the caller passes it on.
    NF_CHECK(!s.apply_setting("graphics.quality", "high"));
    NF_CHECK(!s.apply_setting("audio.volume.", "0.5"));
    NF_CHECK(!s.apply_setting("audio.volume.bogus", "0.5"));
    // A key we own with a value we cannot parse is also refused, and leaves
    // the stored value alone rather than silently zeroing it.
    NF_CHECK(!s.apply_setting("audio.volume.music", "loud"));
    NF_CHECK_NEAR(s.volume(BusId::Music), 0.8f, 1e-6f);

    // A parseable value is clamped like the setter.
    NF_CHECK(s.apply_setting("audio.volume.music", "4.0"));
    NF_CHECK_NEAR(s.volume(BusId::Music), 1.0f, 1e-6f);
    NF_CHECK(s.apply_setting("audio.volume.music", "0.25"));
    NF_CHECK_NEAR(s.volume(BusId::Music), 0.25f, 1e-6f);
}

// ---------------------------------------------------------------------------
// BusMixer — routing and the two volume stages
// ---------------------------------------------------------------------------

NF_TEST(bus_mixer_applies_bus_and_master_volume_once) {
    const usize frames = 8;

    // A source on sfx (volume 1.0) and one on music (volume 0.8 default).
    AudioBuffer sfx_buf = make_constant(1.0f, frames);
    AudioBuffer music_buf = make_constant(0.4f, frames);

    AudioVolumeSettings settings;
    BusMixer mixer;
    mixer.set_settings(&settings);
    mixer.begin_block(frames);

    AudioSource sfx;
    sfx.buffer = &sfx_buf;
    sfx.playing = true;
    sfx.looping = true;
    AudioSource music;
    music.buffer = &music_buf;
    music.playing = true;
    music.looping = true;

    mixer.mix_source(sfx, {0, 0, 0}, {0, 0, -1}, {0, 1, 0}, frames, 44100,
                     BusId::Sfx);
    mixer.mix_source(music, {0, 0, 0}, {0, 0, -1}, {0, 1, 0}, frames, 44100,
                     BusId::Music);

    NF_CHECK_NEAR(mixer.bus_peak(BusId::Sfx), 1.0f, 1e-6f);
    NF_CHECK_NEAR(mixer.bus_peak(BusId::Music), 0.4f, 1e-6f);

    // finalize ADDS, so a caller can prime the block with the device's base.
    std::vector<f32> left(frames, 0.25f);
    std::vector<f32> right(frames, 0.0f);
    mixer.finalize(left.data(), right.data(), frames);

    // left  = 0.25 + sfx(1.0 * 1.0 * 1.0) + music(0.4 * 0.8 * 1.0) = 1.57
    // right = 0.0  + the same two buses (both sources are mono)   = 1.32
    for (usize i = 0; i < frames; ++i) {
        NF_CHECK_NEAR(left[i], 1.57f, 1e-5f);
        NF_CHECK_NEAR(right[i], 1.32f, 1e-5f);
    }

    // finalize clears the accumulators, so the next block starts dry.
    NF_CHECK_NEAR(mixer.bus_peak(BusId::Sfx), 0.0f, 1e-6f);
}

NF_TEST(bus_mixer_master_slider_scales_every_bus) {
    AudioVolumeSettings settings;
    settings.set_volume(BusId::Master, 0.5f);

    const std::vector<f32> out = mix_one(settings, BusId::Sfx, 1.0f, 4);
    for (f32 v : out) {
        NF_CHECK_NEAR(v, 0.5f, 1e-6f);
    }

    // Master 0 is a real mute, not a near-zero.
    settings.set_volume(BusId::Master, 0.0f);
    const std::vector<f32> muted = mix_one(settings, BusId::Sfx, 1.0f, 4);
    for (f32 v : muted) {
        NF_CHECK_NEAR(v, 0.0f, 1e-6f);
    }
}

NF_TEST(bus_mixer_muting_one_bus_leaves_the_others_alone) {
    const usize frames = 4;
    AudioBuffer buf = make_constant(1.0f, frames);

    AudioVolumeSettings settings;
    settings.set_volume(BusId::Music, 0.0f);

    BusMixer mixer;
    mixer.set_settings(&settings);
    mixer.begin_block(frames);

    AudioSource music;
    music.buffer = &buf;
    music.playing = true;
    music.looping = true;
    AudioSource voice;
    voice.buffer = &buf;
    voice.playing = true;
    voice.looping = true;

    mixer.mix_source(music, {0, 0, 0}, {0, 0, -1}, {0, 1, 0}, frames, 44100,
                     BusId::Music);
    mixer.mix_source(voice, {0, 0, 0}, {0, 0, -1}, {0, 1, 0}, frames, 44100,
                     BusId::Voice);

    std::vector<f32> left(frames, 0.0f);
    std::vector<f32> right(frames, 0.0f);
    mixer.finalize(left.data(), right.data(), frames);

    // Only the voice bus survives: 1.0 * 1.0 * 1.0.
    for (usize i = 0; i < frames; ++i) {
        NF_CHECK_NEAR(left[i], 1.0f, 1e-6f);
    }
}

NF_TEST(bus_mixer_routes_pre_mixed_buffers_and_reads_a_bus_back) {
    const usize frames = 4;
    AudioVolumeSettings settings;
    settings.set_volume(BusId::Music, 0.5f);

    BusMixer mixer;
    mixer.set_settings(&settings);
    mixer.begin_block(frames);

    const std::vector<f32> layer_left(frames, 0.4f);
    const std::vector<f32> layer_right(frames, 0.2f);
    mixer.mix_buffer(layer_left.data(), layer_right.data(), frames,
                     BusId::Music);
    // A second layer on the same bus sums with the first.
    mixer.mix_buffer(layer_left.data(), layer_right.data(), frames,
                     BusId::Music);

    // read_bus copies without clearing, so an effects send can read the dry
    // block and still leave it intact for finalize.
    std::vector<f32> probe_left(frames, 0.0f);
    std::vector<f32> probe_right(frames, 0.0f);
    mixer.read_bus(BusId::Music, probe_left.data(), probe_right.data(), frames);
    for (usize i = 0; i < frames; ++i) {
        NF_CHECK_NEAR(probe_left[i], 0.8f, 1e-6f);
        NF_CHECK_NEAR(probe_right[i], 0.4f, 1e-6f);
    }

    std::vector<f32> left(frames, 0.0f);
    std::vector<f32> right(frames, 0.0f);
    mixer.finalize(left.data(), right.data(), frames);
    for (usize i = 0; i < frames; ++i) {
        NF_CHECK_NEAR(left[i], 0.4f, 1e-6f);
        NF_CHECK_NEAR(right[i], 0.2f, 1e-6f);
    }

    // Degenerate calls are no-ops, not crashes.
    mixer.begin_block(frames);
    mixer.mix_buffer(nullptr, layer_right.data(), frames, BusId::Music);
    mixer.mix_buffer(layer_left.data(), nullptr, frames, BusId::Music);
    mixer.mix_buffer(layer_left.data(), layer_right.data(), 0, BusId::Music);
    mixer.mix_buffer(layer_left.data(), layer_right.data(), frames,
                     static_cast<BusId>(99));
    NF_CHECK_NEAR(mixer.bus_peak(BusId::Music), 0.0f, 1e-6f);

    std::vector<f32> short_left(2, 1.0f);
    std::vector<f32> short_right(2, 1.0f);
    mixer.read_bus(BusId::Music, short_left.data(), short_right.data(), 2);
    for (usize i = 0; i < 2; ++i) {
        NF_CHECK_NEAR(short_left[i], 0.0f, 1e-6f);
    }
}
