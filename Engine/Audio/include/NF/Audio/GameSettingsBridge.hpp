#pragma once

// NF/Audio/GameSettingsBridge.hpp — the shipped path from a game's settings
// screen to the live mixer.
//
// The problem this exists to solve, stated exactly:
//
//   `AudioVolumeSettings` is a complete, tested settings contract. It serialises
//   `audio.volume.<bus>=<value>` lines and parses them back. `AudioScene` reads
//   those volumes every block. A game has a Settings screen with volume sliders.
//
//   ...and nothing connected the three. A game's slider moved a float in its own
//   settings struct; the mixer's volumes did not change. The player moved the
//   music slider in a packaged build and nothing happened, and the only way to
//   make it happen was to edit engine source — precisely the thing the
//   Game-Ready programme exists to rule out.
//
// This header is the missing wire. It binds a settings screen's values to the
// mixer and loads/saves them as a settings file, so a game ships working volume
// controls with no engine edit.
//
// Deliberately NOT a dependency of Engine/Audio. `AudioVolumeSettings` is a
// plain value type with no engine dependencies (see Buses.hpp), and that is
// what lets this live beside it instead of inside it.

#include <NF/Audio/Buses.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Types.hpp>

#include <cmath>
#include <string>
#include <string_view>

namespace nf::audio {

/// A settings screen's volume values, in the shape a game UI keeps them.
///
/// Two things this does that a bare struct of floats does not:
///
///  - It knows the mixer's DEFAULT volumes. `SettingsData` in Engine/UI
///    defaulted music to 1.0 while the mixer defaults it to 0.8, so a game that
///    pushed its screen's values across on startup silently turned the music up
///    on first launch. The defaults here match the mixer's exactly, so "not set"
///    and "set to the shipped default" are the same thing, and pushing values
///    across at boot is a no-op rather than a change nobody asked for.
///  - It carries the ambience and voice buses too, not just master/music/sfx.
///    A three-float struct silently drops the two buses a game would want for
///    dialogue and weather.
class GameAudioSettings {
public:
    GameAudioSettings() { reset_to_mixer_defaults(); }

    /// Adopt `AudioVolumeSettings::reset_to_defaults()` exactly. Called by the
    /// constructor, and available again so a "restore defaults" button is one
    /// call rather than five literals that can drift from the mixer's.
    void reset_to_mixer_defaults() {
        AudioVolumeSettings shipped;
        for (u32 i = 0; i < kBusCount; ++i) {
            m_volumes[i] = shipped.volume(static_cast<BusId>(i));
        }
    }

    f32 volume(BusId bus) const { return m_volumes[static_cast<u32>(bus)]; }

    /// Clamps to [0,1]; returns true when the stored value changed. A
    /// non-finite value is ignored, matching AudioVolumeSettings.
    bool set_volume(BusId bus, f32 value) {
        if (!std::isfinite(value)) return false;
        const f32 v = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        const u32 i = static_cast<u32>(bus);
        if (m_volumes[i] == v) return false;
        m_volumes[i] = v;
        return true;
    }

    /// Copy every value into `out`, which the mixer reads on its next block.
    /// The whole point of the type: one call is the entire binding.
    void apply_to(AudioVolumeSettings& out) const {
        for (u32 i = 0; i < kBusCount; ++i) {
            out.set_volume(static_cast<BusId>(i), m_volumes[i]);
        }
    }

    /// The mirror image: adopt whatever the mixer currently holds. Called after
    /// loading a file so the settings screen shows the stored values rather than
    /// the defaults, and so a file that stored one bus does not reset the other
    /// four back to defaults.
    void adopt_from(const AudioVolumeSettings& in) {
        for (u32 i = 0; i < kBusCount; ++i) {
            m_volumes[i] = in.volume(static_cast<BusId>(i));
        }
    }

    /// `audio.volume.<bus>=<value>` lines, one per bus.
    void append_settings_text(std::string& out) const {
        AudioVolumeSettings mirror;
        apply_to(mirror);
        mirror.append_settings_text(out);
    }

    /// Consume one `key=value` pair; true when it was ours. Unrecognised pairs
    /// return false so a shared settings file can carry other systems' keys
    /// without this eating them.
    bool apply_setting(std::string_view key, std::string_view value) {
        AudioVolumeSettings mirror;
        apply_to(mirror);
        if (!mirror.apply_setting(key, value)) return false;
        adopt_from(mirror);
        return true;
    }

    /// Apply every `audio.volume.*` pair in a whole settings file. Returns how
    /// many were consumed, so a caller can tell "loaded my audio settings" from
    /// "loaded an empty file" — the difference between a player hearing their
    /// saved preferences and silently getting the defaults.
    ///
    /// Lines that are not audio volumes are skipped, not rejected: a settings
    /// file is shared with other systems, and refusing to load audio because
    /// someone else's key is unfamiliar would be exactly wrong.
    ///
    /// Only the KEY is trimmed before the value is handed on. The value is
    /// passed through verbatim because `AudioVolumeSettings::apply_setting`
    /// rejects a value with surrounding characters on purpose — a typo like
    /// "0.50x" must not silently become 0.5 — and relaxing that here to accept
    /// `audio.volume.music = 0.25` would quietly undo a rule the bus settings
    /// chose deliberately. A hand-padded line is therefore ignored rather than
    /// misread; the warning below says so instead of leaving the player to
    /// wonder why their volume reset.
    u32 apply_settings_text(std::string_view text) {
        u32 applied = 0;
        usize pos = 0;
        while (pos < text.size()) {
            usize end = text.find('\n', pos);
            if (end == std::string_view::npos) end = text.size();
            std::string_view line = text.substr(pos, end - pos);
            pos = end + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty() || line.front() == '#' || line.front() == ';') continue;
            const usize eq = line.find('=');
            if (eq == std::string_view::npos) continue;
            std::string_view key = line.substr(0, eq);
            const std::string_view value = line.substr(eq + 1);
            // Trim the KEY on both sides. People hand-edit settings files, and
            // indenting or aligning keys is the most common such edit. It carries
            // no ambiguity, so forgiving it costs nothing — unlike the value,
            // which is passed through verbatim (see above).
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
                key.remove_suffix(1);
            }
            while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) {
                key.remove_prefix(1);
            }
            if (apply_setting(key, value)) {
                ++applied;
            } else if (key.rfind("audio.volume.", 0) == 0) {
                // An audio volume line that was NOT applied. Worth reporting: the
                // player is about to hear the default instead of the value they
                // saved, and the file is right there to look at.
                NF_LOG_WARN(LogCategory::Core,
                            "Audio settings: ignored unparsable line '{}={}'",
                            key, value);
            }
        }
        return applied;
    }

private:
    f32 m_volumes[kBusCount] = {1.0f, 0.8f, 1.0f, 0.7f, 1.0f};
};

/// A settings file that does not exist is a first run, not a failure: the caller
/// gets the shipped defaults and `out_found=false`. A file that exists but
/// cannot be read is different — it exists and could not be opened — so that
/// returns false with `out_found=true`.
///
/// `out` is left at the shipped defaults in both failure cases, so a game that
/// ignores the return value still plays at a sensible volume.
bool load_game_audio_settings(const std::string& path,
                              GameAudioSettings& out,
                              bool& out_found);

/// Write the settings file. Returns false rather than failing silently: a
/// settings screen that reports "saved" while nothing was written is the exact
/// bug this whole path exists to prevent.
bool save_game_audio_settings(const std::string& path, const GameAudioSettings& in);

} // namespace nf::audio
