#include <NF/Audio/AudioEngine.hpp>
#include <NF/Core/Logger.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#ifdef _WIN32
#include <NF/Audio/WasapiAudioDevice.hpp>
#endif

namespace nf::audio {

// ---------------------------------------------------------------------------
// Attenuation
// ---------------------------------------------------------------------------

f32 compute_attenuation(const SpatialSettings& s, f32 distance) {
    if (s.model == AttenuationModel::None) {
        return 1.0f;
    }

    if (distance <= s.min_distance) {
        return 1.0f;
    }
    if (distance >= s.max_distance) {
        return 0.0f;
    }

    f32 d = distance - s.min_distance;
    f32 range = s.max_distance - s.min_distance;
    if (range <= 1e-8f) return 0.0f;

    switch (s.model) {
        case AttenuationModel::Linear:
            return 1.0f - d / range;
        case AttenuationModel::Inverse:
            return 1.0f / (1.0f + s.rolloff * d);
        case AttenuationModel::Exponential: {
            f32 ratio = distance / s.min_distance;
            if (ratio < 1e-6f) return 1.0f;
            return std::pow(ratio, -s.rolloff);
        }
        default:
            return 1.0f;
    }
}

// ---------------------------------------------------------------------------
// 3D pan/gain
// ---------------------------------------------------------------------------

PanGain compute_3d_pan_gain(const Vec3& listener_pos,
                             const Vec3& listener_forward,
                             const Vec3& listener_up,
                             const Vec3& source_pos,
                             const SpatialSettings& spatial) {
    Vec3 to_source = source_pos - listener_pos;
    f32 distance = to_source.length();

    f32 gain = compute_attenuation(spatial, distance);

    if (distance < 1e-6f) {
        // Source at listener position — center.
        return {gain, gain};
    }

    // Compute the right vector: forward x up (left-handed) or up x forward.
    // We want listener_right = cross(forward, up) for a right-handed system.
    // Our cross product: (a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x).
    Vec3 right = listener_forward.cross(listener_up);
    f32 right_len = right.length();
    if (right_len < 1e-6f) {
        return {gain, gain};
    }
    right = right * (1.0f / right_len);

    // Project the direction to source onto the right vector.
    Vec3 dir = to_source * (1.0f / distance);
    f32 pan = dir.dot(right); // -1 (full left) to +1 (full right)

    // Equal-power panning: left = cos((pan+1)/2 * pi/2), right = sin(...)
    f32 angle = (pan + 1.0f) * 0.5f * 3.14159265f * 0.5f;
    f32 left_gain = std::cos(angle) * gain;
    f32 right_gain = std::sin(angle) * gain;

    return {left_gain, right_gain};
}

// ---------------------------------------------------------------------------
// AudioBus mixing
// ---------------------------------------------------------------------------

void AudioBus::mix_source(AudioSource& source,
                           const Vec3& listener_pos,
                           const Vec3& listener_forward,
                           const Vec3& listener_up,
                           f32* out_left,
                           f32* out_right,
                           usize num_frames,
                           u32 mix_sample_rate) {
    if (!source.playing || source.buffer == nullptr) return;

    const AudioBuffer& buf = *source.buffer;
    if (buf.samples.empty() || buf.channels == 0 || buf.sample_rate == 0 ||
        num_frames == 0) {
        return;
    }
    if (mix_sample_rate == 0) {
        mix_sample_rate = kDefaultSampleRate;
    }

    // Compute pan/gain (for spatial) or use volume directly (for 2D).
    f32 left_gain = source.volume;
    f32 right_gain = source.volume;

    if (source.spatial) {
        auto pg = compute_3d_pan_gain(listener_pos, listener_forward, listener_up,
                                      source.position, source.spatial_settings);
        left_gain *= pg.left;
        right_gain *= pg.right;
    }

    const usize frames_available = buf.frame_count();
    if (frames_available == 0) return;

    // `sample_position` is authoritative after the first block. Preserve the
    // established integer seek behaviour as well: callers that assign a new
    // `sample_cursor` get that exact position even if the fractional cursor
    // still points elsewhere. Setting sample_position to -1 is the explicit
    // form of the same operation.
    const bool has_fractional_position =
        std::isfinite(source.sample_position) && source.sample_position >= 0.0;
    f64 position = has_fractional_position
                       ? source.sample_position
                       : static_cast<f64>(source.sample_cursor);
    if (has_fractional_position &&
        position < static_cast<f64>((std::numeric_limits<usize>::max)())) {
        const usize position_floor =
            static_cast<usize>(std::floor(position));
        if (source.sample_cursor != position_floor) {
            position = static_cast<f64>(source.sample_cursor);
        }
    }

    if (position >= static_cast<f64>(frames_available)) {
        if (source.looping) {
            position = std::fmod(position,
                                 static_cast<f64>(frames_available));
        } else {
            source.playing = false;
            source.sample_cursor = 0;
            source.sample_position = 0.0;
            return;
        }
    }

    // Pitch multiplies the resampling step, so it composes with a buffer whose
    // own rate already differs from the mix rate. A non-finite or non-positive
    // pitch falls back to 1.0 instead of stalling (0) or running backwards (a
    // negative step would walk off the front of the buffer forever): an authored
    // value that cannot be honoured plays at the authored rate rather than
    // going silent, which is the smaller surprise.
    f32 pitch = source.pitch;
    if (!std::isfinite(pitch) || pitch <= 0.0f) {
        pitch = 1.0f;
    }
    const f64 source_step =
        static_cast<f64>(buf.sample_rate) / static_cast<f64>(mix_sample_rate) *
        static_cast<f64>(pitch);
    for (usize i = 0; i < num_frames; ++i) {
        if (position >= static_cast<f64>(frames_available)) {
            if (!source.looping) {
                source.playing = false;
                source.sample_cursor = 0;
                source.sample_position = 0.0;
                break;
            }
            position = std::fmod(position,
                                 static_cast<f64>(frames_available));
        }

        const usize frame0 = static_cast<usize>(position);
        const f64 fraction = position - static_cast<f64>(frame0);
        usize frame1 = frame0 + 1;
        if (frame1 >= frames_available) {
            frame1 = source.looping ? 0u : frame0;
        }

        const usize index0 = frame0 * buf.channels;
        const usize index1 = frame1 * buf.channels;
        const f32 left0 = buf.samples[index0];
        const f32 left1 = buf.samples[index1];
        const f32 right0 = buf.channels >= 2 ? buf.samples[index0 + 1] : left0;
        const f32 right1 = buf.channels >= 2 ? buf.samples[index1 + 1] : left1;
        const f32 sample_left = static_cast<f32>(
            static_cast<f64>(left0) +
            (static_cast<f64>(left1) - static_cast<f64>(left0)) * fraction);
        const f32 sample_right = static_cast<f32>(
            static_cast<f64>(right0) +
            (static_cast<f64>(right1) - static_cast<f64>(right0)) * fraction);

        out_left[i] += sample_left * left_gain;
        out_right[i] += sample_right * right_gain;
        position += source_step;
    }

    source.sample_cursor =
        static_cast<usize>(std::floor(position));
    source.sample_position = position;
}

// ---------------------------------------------------------------------------
// Procedural buffer
// ---------------------------------------------------------------------------

AudioBuffer make_tone_buffer(f32 frequency_hz, f32 duration_seconds,
                             u32 sample_rate, u32 channels) {
    AudioBuffer buf;
    if (sample_rate == 0 || duration_seconds <= 0.0f) {
        return buf;
    }

    const usize frames = static_cast<usize>(duration_seconds * static_cast<f32>(sample_rate));
    if (frames == 0) {
        return buf;
    }

    buf.sample_rate = sample_rate;
    buf.channels = (channels == 2) ? 2u : 1u;
    buf.samples.resize(frames * buf.channels);

    // Half scale leaves headroom: the bus sums sources into one buffer, so a
    // full-scale tone clips as soon as a second source overlaps it.
    constexpr f32 kAmplitude = 0.5f;
    const f32 step = TWO_PI * frequency_hz / static_cast<f32>(sample_rate);

    for (usize f = 0; f < frames; ++f) {
        const f32 v = std::sin(step * static_cast<f32>(f)) * kAmplitude;
        for (u32 c = 0; c < buf.channels; ++c) {
            buf.samples[f * buf.channels + c] = v;
        }
    }
    return buf;
}

// ---------------------------------------------------------------------------
// NullAudioDevice
// ---------------------------------------------------------------------------

bool NullAudioDevice::initialize(u32 sample_rate, u32 /*buffer_frames*/) {
    m_sample_rate = sample_rate;
    m_initialized = true;
    return true;
}

void NullAudioDevice::shutdown() {
    m_initialized = false;
}

bool NullAudioDevice::is_initialized() const {
    return m_initialized;
}

u32 NullAudioDevice::sample_rate() const {
    return m_sample_rate;
}

void NullAudioDevice::request_buffer(f32* left, f32* right, usize num_frames) {
    if (left) std::memset(left, 0, num_frames * sizeof(f32));
    if (right) std::memset(right, 0, num_frames * sizeof(f32));
}

// ---------------------------------------------------------------------------
// Output device factory
// ---------------------------------------------------------------------------

std::unique_ptr<AudioDevice> create_output_device() {
#ifdef _WIN32
    // Real hardware first: shared-mode float stereo at the endpoint's own
    // mix rate (resampled from the engine rate when they differ — some
    // drivers reject anything but the mix rate at open time).
    auto wasapi = std::make_unique<WasapiAudioDevice>();
    if (wasapi->initialize(kDefaultSampleRate, kDefaultBufferFrames)) {
        NF_LOG_INFO(LogCategory::Audio, "Audio output: {}", wasapi->backend_name());
        return wasapi;
    }
    NF_LOG_INFO(LogCategory::Audio, "Audio output: no usable endpoint, silent null device");
#endif
    auto null_dev = std::make_unique<NullAudioDevice>();
    null_dev->initialize(kDefaultSampleRate, kDefaultBufferFrames);
    return null_dev;
}

} // namespace nf::audio
