#pragma once

// NF/Runtime/PlayerSettings.hpp — the settings that belong to the PLAYER, not
// to the level.
//
// The camera's sensitivity, its inverted-Y preference and its field of view
// are authored in the scene, and that is right for a demo: a level shows its
// intended framing. It is wrong for a game, because two players at one machine
// disagree about all three, and neither should have to edit a scene file (or
// ask the author to) to change how the mouse feels.
//
// So the scene keeps the AUTHORED value and this file keeps a PLAYER value on
// top of it. The rule is the one the rest of the engine already follows:
//
//   * a scale of 1.0 and an FOV of 0 mean "the scene's value" — a player who
//     never writes a settings file plays exactly the scene the author shipped;
//   * a player value OVERRIDES, it never edits the scene, so deleting the file
//     returns the scene's intent intact;
//   * the file is optional and its absence is not an error.
//
// Format (`content://Settings/player.nfsettings`), one key per line, `#`
// comments, unknown keys ignored so an older engine can read a newer file:
//
//     # SANAD player settings
//     version: 1
//     look_scale: 1.4
//     stick_scale: 1.0
//     invert_y: on        # on | off | auto (auto = whatever the scene says)
//     fov: 0              # 0 = the scene's FOV
//     ads_scale: 0.9
//     shake_scale: 0.6
//     volume.master: 1.0
//     audio.dynamics.compressor: off
//     audio.dynamics.limiter: off
//
// The two dynamics stages are OFF by default. A player who wants a quieter
// night mix opts in; everyone else hears the author's master bit for bit.

#include <NF/Audio/Buses.hpp>
#include <NF/Audio/Dynamics.hpp>
#include <NF/Core/Types.hpp>

#include <string>
#include <string_view>

namespace nf::runtime {

/// What the player wants done with the vertical look axis.
enum class InvertY : u8 {
    Scene = 0, ///< Whatever the scene authored (the default).
    On = 1,    ///< Force inverted.
    Off = 2,   ///< Force normal.
};

struct PlayerSettings {
    /// Multipliers on the scene's authored sensitivity. 1.0 = unchanged;
    /// 0.5 = half as fast; 2.0 = twice. Clamped to [0.05, 10] on load,
    /// because 0 would freeze the camera and the player would call it a bug.
    float look_scale = 1.0f;
    float stick_scale = 1.0f;

    /// Inverted-Y preference. `Scene` keeps the authored value, which is why
    /// this is a three-state value and not a bool: "the player has no
    /// opinion" is a real answer and must not read as "the player wants it
    /// off".
    InvertY invert_y = InvertY::Scene;

    /// Vertical field of view in degrees, or 0 for the scene's. Bounded to
    /// [20, 140] when authored: outside that range the projection is a
    /// special effect, not a preference.
    float fov_y = 0.0f;

    /// Multiplier on the scene's ADS sensitivity scale, so a player can slow
    /// the sight picture further without the scene knowing.
    float ads_scale = 1.0f;

    /// Multiplier on camera shake. 0 turns shake off for a player who finds
    /// it nauseating, without any scene or script changing.
    float shake_scale = 1.0f;

    /// Mixer volumes, one per bus, on the 0..1 scale `AudioVolumeSettings`
    /// defines. Stored as that type rather than five floats so the shipped
    /// defaults (master 1.0, music 0.8, sfx 1.0, ambience 0.7, voice 1.0)
    /// live in ONE place — a second copy here would drift the day one of them
    /// is retuned.
    audio::AudioVolumeSettings volumes{};

    /// Master-bus compressor and limiter. This is a player preference, not
    /// scene data: a night-time headphone listener needs a different dynamic
    /// range from a player on a home theatre, and the level's author cannot
    /// ship one master that fits both. Both stages default to off.
    audio::MasterDynamicsSettings dynamics{};
};

/// Parses the settings text. Unknown keys are ignored (forward compatible);
/// a KNOWN key with an unreadable or out-of-range value is an error naming the
/// line, and the caller keeps the previous settings — half-applied settings
/// are worse than none, because the player cannot tell which half took.
[[nodiscard]] bool parse_player_settings(std::string_view text, PlayerSettings& out,
                                         std::string& out_error);

/// The inverse, in a stable key order so a saved file diffs cleanly.
[[nodiscard]] std::string serialize_player_settings(const PlayerSettings& settings);

// --- profiles ----------------------------------------------------------------
//
// One machine, more than one player. A profile is just a settings file under a
// name (`content://Settings/players/<name>.nfsettings`), and the ACTIVE one is
// still `player.nfsettings` — the runtime never has to know profiles exist.
// The editor copies a profile over the active file to switch, which keeps the
// runtime's contract ("read one file") exactly as it was.
//
// These three helpers are pure so the rules can be tested without a filesystem:
// a name that would escape the directory, a name with a newline, or an index
// with a duplicate would each be a real bug that no UI test would catch.

/// True for names that are safe as a file stem: 1..32 characters of letters,
/// digits, space, `_` and `-`, and not starting or ending with a space.
/// Refused rather than sanitized — silently renaming "a/b" to "ab" would make
/// two different profiles collide.
[[nodiscard]] bool valid_profile_name(std::string_view name);

/// The logical path a profile lives at. Caller must have validated the name;
/// an invalid name returns an empty string rather than a path that escapes.
[[nodiscard]] std::string profile_path(std::string_view name);

/// The profile index: one name per line, `#` comments, blanks ignored. This is
/// how the editor knows which profiles exist — the VFS has no directory
/// listing, and inventing one for a five-line file would be the wrong trade.
[[nodiscard]] std::vector<std::string> parse_profile_index(std::string_view text);

/// The inverse. Writes one name per line; invalid names are dropped, which is
/// the only sane answer for an index that was hand-edited into nonsense.
[[nodiscard]] std::string serialize_profile_index(const std::vector<std::string>& names);

} // namespace nf::runtime
