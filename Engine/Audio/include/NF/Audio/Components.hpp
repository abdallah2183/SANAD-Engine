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

} // namespace nf::audio
