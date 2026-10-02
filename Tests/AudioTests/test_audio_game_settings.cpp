// Tests/AudioTests/test_audio_game_settings.cpp — the game settings-screen path
// to the live mixer (G5 -> Runtime, closed 2026-10-01).
//
// The gap this covers: `AudioVolumeSettings` could serialise and parse
// `audio.volume.<bus>=<value>`, and `AudioScene` read those values every block,
// and a game had a Settings screen with volume sliders — and nothing connected
// them. A shipped game moved a slider and the music did not change, and the fix
// was "edit engine source", which is the one thing the Game-Ready programme
// exists to rule out.
//
// So these assert the whole chain, not just the file format: settings -> the
// mixer's own settings object -> the mix the player actually hears.

#include <NF/Test/TestFramework.hpp>

#include <NF/Audio/AudioScene.hpp>
#include <NF/Audio/GameSettingsBridge.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

using namespace nf;
using namespace nf::audio;

namespace {

std::filesystem::path settings_path(const char* tag) {
    return std::filesystem::temp_directory_path() /
           (std::string("nf_game_audio_") + tag + ".settings");
}

/// Peak of one 1024-frame block of a 440 Hz tone routed to `bus`, with the
/// mixer configured by `settings`. The helper exists so the tests below assert
/// on what a player would HEAR rather than on a struct field — a file-format
/// test can pass while the wire to the mixer is still cut.
float peak_of_tone(const GameAudioSettings& settings, BusId bus) {
    AudioScene scene;
    settings.apply_to(scene.settings());

    AudioBuffer tone = make_tone_buffer(440.0f, 0.5f, kDefaultSampleRate, 1);
    scene.begin_block(1024, kDefaultSampleRate);
    Emitter emitter;
    emitter.buffer = &tone;
    emitter.playing = true;
    emitter.bus = bus;
    scene.mix_emitter(emitter);

    std::vector<f32> left(1024, 0.0f), right(1024, 0.0f);
    scene.finalize(left.data(), right.data());

    float peak = 0.0f;
    for (usize i = 0; i < 1024; ++i) {
        peak = std::max(peak, std::abs(left[i]));
        peak = std::max(peak, std::abs(right[i]));
    }
    return peak;
}

} // namespace

NF_TEST(game_audio_settings_defaults_match_the_mixer_exactly) {
    // The bug this type was written to prevent: a settings struct whose defaults
    // differ from the mixer's. `SettingsData` in Engine/UI defaults music to 1.0
    // while the mixer ships 0.8, so a game that pushed its screen's values across
    // at startup turned the music UP on a player's first launch, and nothing in
    // the game had asked for that.
    GameAudioSettings settings;
    AudioVolumeSettings mixer_defaults;
    for (u32 i = 0; i < kBusCount; ++i) {
        const auto bus = static_cast<BusId>(i);
        NF_CHECK_NEAR(settings.volume(bus), mixer_defaults.volume(bus), 1e-6f);
    }
    // The specific pair that differed, pinned so a future default change on
    // either side is a visible decision rather than a silent mismatch.
    NF_CHECK_NEAR(settings.volume(BusId::Music), 0.8f, 1e-6f);
    NF_CHECK_NEAR(settings.volume(BusId::Ambience), 0.7f, 1e-6f);
}

NF_TEST(pushing_settings_into_the_mixer_changes_what_is_heard) {
    // The claim, end to end through the mixer's own arithmetic: a settings
    // screen value reaches the output buffer.
    GameAudioSettings settings;
    settings.set_volume(BusId::Sfx, 0.0f);
    const float muted = peak_of_tone(settings, BusId::Sfx);

    settings.set_volume(BusId::Sfx, 1.0f);
    const float audible = peak_of_tone(settings, BusId::Sfx);

    NF_CHECK(muted < 1e-6f);
    NF_CHECK(audible > 0.01f);
}

NF_TEST(game_audio_settings_round_trip_through_a_file) {
    const auto path = settings_path("roundtrip");
    std::filesystem::remove(path);

    GameAudioSettings written;
    written.set_volume(BusId::Master, 0.5f);
    written.set_volume(BusId::Music, 0.25f);
    written.set_volume(BusId::Voice, 0.125f);
    NF_CHECK(save_game_audio_settings(path.string(), written));

    GameAudioSettings loaded;
    bool found = false;
    NF_CHECK(load_game_audio_settings(path.string(), loaded, found));
    NF_CHECK(found);
    for (u32 i = 0; i < kBusCount; ++i) {
        const auto bus = static_cast<BusId>(i);
        NF_CHECK_NEAR(loaded.volume(bus), written.volume(bus), 1e-6f);
    }
    std::filesystem::remove(path);
}

NF_TEST(a_missing_settings_file_is_a_first_run_not_a_failure) {
    // First launch has no file. That must succeed with the shipped defaults, or
    // a game that treats a false return as fatal cannot start on a new machine.
    const auto path = settings_path("absent");
    std::filesystem::remove(path);

    GameAudioSettings loaded;
    loaded.set_volume(BusId::Music, 0.1f);  // must be reset by the load
    bool found = true;
    NF_CHECK(load_game_audio_settings(path.string(), loaded, found));
    NF_CHECK(!found);
    // Crucially, the previous session's value did not survive: a caller that
    // reuses the object across reloads must not leak state between them.
    NF_CHECK_NEAR(loaded.volume(BusId::Music), 0.8f, 1e-6f);
}

NF_TEST(a_settings_file_with_no_audio_keys_is_reported_but_not_fatal) {
    // A shared settings file holding other systems' keys is normal. Loading
    // audio from it must consume the audio lines and leave the rest alone.
    const auto path = settings_path("shared");
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "# a settings file\n";
        f << "graphics.resolution=1920x1080\n";  // not ours: must be left alone
        f << "audio.volume.music = 0.25\n";      // hand-edited space after '='
        f << "audio.volume.voice=0.4\n";        // the form a tool writes
    }

    GameAudioSettings loaded;
    bool found = false;
    NF_CHECK(load_game_audio_settings(path.string(), loaded, found));
    NF_CHECK(found);
    // The tool-written form applied...
    NF_CHECK_NEAR(loaded.volume(BusId::Voice), 0.4f, 1e-6f);
    // ...and the hand-padded one did NOT. That is deliberate, and the point of
    // pinning it: `AudioVolumeSettings::apply_setting` refuses a value carrying
    // surrounding characters so a typo like "0.50x" cannot become 0.5. Making a
    // padded line work here would quietly undo that rule to accommodate a
    // formatting nicety, so the line is ignored (and warned about) instead. The
    // buses it did not mention keep the mixer's defaults.
    NF_CHECK_NEAR(loaded.volume(BusId::Music), 0.8f, 1e-6f);
    NF_CHECK_NEAR(loaded.volume(BusId::Sfx), 1.0f, 1e-6f);
    NF_CHECK_NEAR(loaded.volume(BusId::Ambience), 0.7f, 1e-6f);

    std::filesystem::remove(path);
}

NF_TEST(a_padded_key_still_loads_because_people_hand_edit_settings_files) {
    // The KEY side of the same rule. Indenting or aligning keys is the most
    // common human edit and carries no ambiguity, so it is forgiven. This is the
    // contrast with the test above: forgiving the key is safe, forgiving the
    // value is not.
    const auto path = settings_path("paddedkey");
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "   audio.volume.ambience  =0.9\n";
    }
    GameAudioSettings loaded;
    bool found = false;
    NF_CHECK(load_game_audio_settings(path.string(), loaded, found));
    NF_CHECK(found);
    NF_CHECK_NEAR(loaded.volume(BusId::Ambience), 0.9f, 1e-6f);
    std::filesystem::remove(path);
}

NF_TEST(game_audio_settings_clamps_and_rejects_the_impossible) {
    GameAudioSettings s;
    NF_CHECK(s.set_volume(BusId::Music, 5.0f));
    NF_CHECK_NEAR(s.volume(BusId::Music), 1.0f, 1e-6f);
    NF_CHECK(s.set_volume(BusId::Music, -3.0f));
    NF_CHECK_NEAR(s.volume(BusId::Music), 0.0f, 1e-6f);
    // A NaN would poison the value permanently and silently silence the bus
    // forever, so it is refused rather than clamped.
    NF_CHECK(!s.set_volume(BusId::Music, std::numeric_limits<f32>::quiet_NaN()));
    NF_CHECK_NEAR(s.volume(BusId::Music), 0.0f, 1e-6f);
    // "Unchanged" is reported, so a settings screen can skip a redundant save.
    NF_CHECK(!s.set_volume(BusId::Music, 0.0f));
    NF_CHECK(s.set_volume(BusId::Music, 0.1f));
}

NF_TEST(saving_into_a_directory_that_does_not_exist_still_works) {
    // First-run path: the game has never written a settings file, so the folder
    // is not there either. Refusing the first write would be the worst possible
    // moment to fail.
    const auto dir = std::filesystem::temp_directory_path() / "nf_game_audio_nested";
    std::filesystem::remove_all(dir);
    const auto path = dir / "deeper" / "audio.settings";

    GameAudioSettings s;
    s.set_volume(BusId::Master, 0.75f);
    NF_CHECK(save_game_audio_settings(path.string(), s));
    NF_CHECK(std::filesystem::exists(path));

    GameAudioSettings back;
    bool found = false;
    NF_CHECK(load_game_audio_settings(path.string(), back, found));
    NF_CHECK(found);
    NF_CHECK_NEAR(back.volume(BusId::Master), 0.75f, 1e-6f);

    std::filesystem::remove_all(dir);
}

NF_TEST(a_saved_settings_file_reloads_into_a_live_mixer) {
    // The full journey a player actually takes: change a slider, quit, relaunch,
    // and hear the volume they chose. Each hop is proven above; this proves they
    // compose, because a bridge that works in isolation and not in sequence is
    // the usual outcome.
    const auto path = settings_path("journey");
    std::filesystem::remove(path);

    // Session one: the player turns the music down.
    {
        GameAudioSettings settings;
        settings.set_volume(BusId::Music, 0.3f);
        NF_CHECK(save_game_audio_settings(path.string(), settings));
    }

    // Session two: a fresh process, a fresh mixer, the same file.
    {
        GameAudioSettings settings;
        bool found = false;
        NF_CHECK(load_game_audio_settings(path.string(), settings, found));
        NF_CHECK(found);

        const float peak = peak_of_tone(settings, BusId::Music);
        // The tone generator peaks at 0.5, so an unattenuated mix is ~0.5 and
        // 0.3 of it is ~0.15. The point is that it is neither silence nor full
        // volume: the stored value is in the audio, not just in the struct.
        NF_CHECK(peak > 0.02f);
        NF_CHECK(peak < 0.45f);
    }

    std::filesystem::remove(path);
}