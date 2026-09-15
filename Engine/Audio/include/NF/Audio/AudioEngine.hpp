#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nf::audio {

/// Sample rate the runtime opens the device at, and the rate procedural tones
/// are generated at. 44100 is the conventional choice and is what the null
/// backend reports, so headless and windowed runs agree.
inline constexpr u32 kDefaultSampleRate = 44100;

/// Frames per device buffer. The runtime mixes in blocks of this size, which
/// keeps a frame's mixing cost independent of how long the frame took.
inline constexpr u32 kDefaultBufferFrames = 1024;

/// Attenuation model for 3D positional audio.
enum class AttenuationModel {
    None,           // No attenuation (2D)
    Linear,         // linear from [min_dist, max_dist]
    Inverse,        // 1 / (1 + r * dist) — classic OpenAL
    Exponential,    // pow(dist / min_dist, -rolloff)
};

/// 3D audio source settings.
struct SpatialSettings {
    AttenuationModel model = AttenuationModel::Linear;
    f32 min_distance = 1.0f;  // No attenuation below this
    f32 max_distance = 50.0f; // Full attenuation at/beyond this
    f32 rolloff = 1.0f;       // Model-dependent rate
};

/// Compute the gain (0..1) for a source at `distance` from the listener.
/// Pure function — no hardware dependency.
f32 compute_attenuation(const SpatialSettings& settings, f32 distance);

/// Compute stereo pan and gain for a 3D source.
/// `listener_pos` and `source_pos` are world-space.
/// `listener_forward` and `listener_up` define the listener orientation.
/// Returns left gain and right gain (0..1 each).
struct PanGain {
    f32 left = 0.0f;
    f32 right = 0.0f;
};

PanGain compute_3d_pan_gain(const Vec3& listener_pos,
                            const Vec3& listener_forward,
                            const Vec3& listener_up,
                            const Vec3& source_pos,
                            const SpatialSettings& spatial);

/// A PCM audio buffer. Supports mono and stereo, 16-bit or 32-bit float.
/// Simple and self-contained — a trivial format for testing and basic
/// playback. A future import pipeline (WAV/OGG) would decode into this.
struct AudioBuffer {
    std::vector<f32> samples;  // Interleaved for stereo
    u32 channels = 0;
    u32 sample_rate = 0;
    usize frame_count() const { return channels > 0 ? samples.size() / channels : 0; }
    f32 duration_seconds() const { return sample_rate > 0 ? static_cast<f32>(frame_count()) / static_cast<f32>(sample_rate) : 0.0f; }
};

/// Build a deterministic PCM sine tone.
///
/// Exists for the same reason the animation side has a procedural clip builder:
/// the audio import pipeline (WAV/OGG/FLAC) is an explicit Phase 9 non-goal
/// (§7), but a scene still needs real samples. Without this an `Audio:` line
/// can only ever be silence, so "the runtime steps audio" is untestable.
///
/// `channels` is treated as mono unless it is exactly 2. Returns an empty
/// buffer (no samples, zero rate) when `sample_rate` or `duration` is
/// non-positive, so a bad scene field yields silence rather than a huge
/// allocation.
AudioBuffer make_tone_buffer(f32 frequency_hz, f32 duration_seconds,
                             u32 sample_rate, u32 channels = 1);

/// Audio source: a buffer + playback state.
struct AudioSource {
    const AudioBuffer* buffer = nullptr;
    f32 volume = 1.0f;
    f32 pitch = 1.0f;
    bool looping = false;
    bool playing = false;
    usize sample_cursor = 0;

    // 3D
    bool spatial = false;
    Vec3 position = {0, 0, 0};
    SpatialSettings spatial_settings;

    void play() { playing = true; }
    void pause() { playing = false; }
    void stop() { playing = false; sample_cursor = 0; }
};

/// Audio bus: a mixer that sums sources, applies a master volume, and
/// produces an output buffer.
struct AudioBus {
    f32 volume = 1.0f;
    std::string name = "master";

    /// Mix `source` into `out_left` and `out_right` for `num_frames` frames.
    /// `out_left` and `out_right` are pre-sized to `num_frames`.
    /// `listener_*` are used only if the source is spatial.
    void mix_source(AudioSource& source,
                    const Vec3& listener_pos,
                    const Vec3& listener_forward,
                    const Vec3& listener_up,
                    f32* out_left,
                    f32* out_right,
                    usize num_frames,
                    u32 sample_rate);
};

/// AudioListener: position and orientation of the audio listener.
struct AudioListener {
    Vec3 position = {0, 0, 0};
    Vec3 forward = {0, 0, -1};
    Vec3 up = {0, 1, 0};
};

/// Abstract audio device. The headless/null backend returns silence and never
/// touches hardware. The WASAPI backend (Windows) pushes the final mix to the
/// OS mixer on its own thread.
class AudioDevice {
public:
    virtual ~AudioDevice() = default;
    virtual bool initialize(u32 sample_rate, u32 buffer_frames) = 0;
    virtual void shutdown() = 0;
    virtual bool is_initialized() const = 0;
    virtual u32 sample_rate() const = 0;
    /// Called by the runtime to request a buffer of mixed audio. The device
    /// fills `left` and `right` (each `num_frames` long). The null backend
    /// fills with silence.
    virtual void request_buffer(f32* left, f32* right, usize num_frames) = 0;
    /// Human-readable backend id for logs and tests ("null", "wasapi-shared").
    virtual const char* backend_name() const { return "null"; }
    /// True when the device wants the final mix pushed (real output).
    /// Default false keeps every existing backend compiling unchanged.
    virtual bool accepts_push() const { return false; }
    /// Hands the fully-mixed block to the device. Only called when
    /// accepts_push() is true.
    virtual void submit_mix(const f32* left, const f32* right, usize num_frames) {
        (void)left;
        (void)right;
        (void)num_frames;
    }
};

/// Best output device for this machine: real hardware when a backend can
/// open it, NullAudioDevice otherwise (headless/CI, or no endpoint). Never
/// null, never throws — silence is always preferable to no runtime.
std::unique_ptr<AudioDevice> create_output_device();

/// Null/headless audio device — produces silence, no hardware. This is what
/// CI and headless tests use. It never fails and never touches the audio API.
class NullAudioDevice : public AudioDevice {
public:
    bool initialize(u32 sample_rate, u32 buffer_frames) override;
    void shutdown() override;
    bool is_initialized() const override;
    u32 sample_rate() const override;
    void request_buffer(f32* left, f32* right, usize num_frames) override;

private:
    bool m_initialized = false;
    u32 m_sample_rate = 44100;
};

} // namespace nf::audio
