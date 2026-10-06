#pragma once

#include <NF/Audio/AudioEngine.hpp>
#include <NF/Core/UUID.hpp>
#include <string>

namespace nf::audio {

/// ECS component for audio. Stores the audio source state (volume, pitch,
/// looping, spatial) plus a reference to the audio buffer asset (by UUID).
/// The runtime steps audio each frame: updates the source position from the
/// entity transform, and requests mixed output from the audio device.
struct AudioComponent {
    // Asset reference
    UUID buffer_uuid{};
    std::string buffer_name;

    // Playback state
    f32 volume = 1.0f;
    f32 pitch = 1.0f;
    bool looping = false;
    bool playing = false;
    bool autoplay = false;

    /// Playback position, in frames. Owned by the component rather than a live
    /// AudioSource so it survives the frame: the runtime builds a fresh source
    /// per mix, and a cursor living only there would restart the sound every
    /// frame.
    usize sample_cursor = 0;

    /// Exact fractional playback position, the resampling cursor. Both this and
    /// `sample_cursor` must survive the frame: `sample_cursor` alone is the
    /// integral part, so a buffer whose rate differs from the mix rate would be
    /// re-seeked to a truncated position every block and drift. -1 means "adopt
    /// sample_cursor on the next mix", which is the correct initial value.
    f64 sample_position = -1.0;

    // Routing (AudioScene, Buses.hpp). Every source feeds exactly one bus, and
    // each bus is a Settings slider. Defaults match the shipped world mix.
    audio::BusId bus = audio::BusId::Sfx;
    /// Run the wall test between the listener and this source. False for sounds
    /// meant to ignore geometry (a UI blip, narration).
    bool occluded = true;

    // 3D spatial
    bool spatial = false;
    SpatialSettings spatial_settings;

    // Internal: the runtime fills this on load.
    const AudioBuffer* buffer = nullptr;

    /// Procedural tone spec. The buffer below is *generated*, not loaded, so
    /// the spec is the only record of what it was — a save/load round trip
    /// needs it to rebuild the samples, otherwise the reloaded scene is silent.
    /// Zero means "no procedural tone".
    f32 tone_hz = 0.0f;
    f32 tone_duration = 0.0f;

    /// Buffer generated from `tone_*`, held by value.
    ///
    /// `buffer` is deliberately NOT pointed at this in the loader: the
    /// component is moved into the ECS world, which would leave a pointer to
    /// the moved-from object's member. `Runtime::step_audio` binds the address
    /// at mix time instead, which is always current.
    AudioBuffer owned_buffer;

    /// The buffer to mix: an asset-backed one when the runtime resolved
    /// `buffer_name`, otherwise the generated one. Null when there is neither.
    const AudioBuffer* resolved_buffer() const {
        if (buffer != nullptr) {
            return buffer;
        }
        return owned_buffer.samples.empty() ? nullptr : &owned_buffer;
    }
};

/// A reverb zone authored on an entity. The zone's POSITION is the entity's
/// world transform, so moving the entity moves the echo with it.
///
/// Why an entity component and not a scene-level record: the `.nfscene` format
/// has no place for a global list — scene v1 expects `---` immediately after
/// `entity_count`, so a scene-wide block would be a format migration. Placing
/// the echo by placing an object is also what an author wants: "this cave
/// echoes" is a fact about a place.
///
/// `inner_radius <= radius` is the contract `audio::ReverbZone` documents:
/// full effect inside the inner radius, fading to nothing at the outer one.
struct ReverbZoneComponent {
    f32 radius = 10.0f;
    f32 inner_radius = 2.0f;
    /// Wet level (0..1) of the echo at full effect.
    f32 wet_gain = 0.35f;
    /// Seconds for the tail to fall 60 dB.
    f32 decay_seconds = 1.5f;
    /// Seconds of silence before the first echo returns.
    f32 pre_delay_seconds = 0.03f;
    /// Seconds between successive echoes.
    f32 echo_spacing_seconds = 0.11f;
    bool enabled = true;
};

/// Level music — the track a scene starts on load, faded in.
///
/// `buffer_name` is a logical path (e.g. `content://Audio/theme.ogg`) resolved
/// through the VFS by `resolve_scene_audio`, exactly like AudioComponent's; the
/// decoded samples land in `owned_buffer` and are what the runtime hands the
/// mixer.
struct MusicComponent {
    std::string buffer_name;
    /// Passed to the mixer as the track's base volume.
    f32 volume = 1.0f;
    f32 fade_in_seconds = 0.0f;
    bool enabled = true;

    /// Decoded at load. Not serialized — the path is.
    AudioBuffer owned_buffer;

    const AudioBuffer* resolved_buffer() const {
        return owned_buffer.samples.empty() ? nullptr : &owned_buffer;
    }
};

/// The ambience bed (wind, crowd, room tone) a scene starts on load.
///
/// Deliberately has no volume field: `MusicSystem::set_ambience` takes a buffer
/// and a fade and nothing else, so a volume here would be parsed, saved and
/// round-tripped while never reaching the mixer — the exact "field with no
/// reader" defect this engine has shipped before. Ambience level is the
/// `ambience` bus slider's job.
struct AmbienceComponent {
    std::string buffer_name;
    f32 fade_in_seconds = 0.0f;
    bool enabled = true;

    AudioBuffer owned_buffer;

    const AudioBuffer* resolved_buffer() const {
        return owned_buffer.samples.empty() ? nullptr : &owned_buffer;
    }
};

} // namespace nf::audio
