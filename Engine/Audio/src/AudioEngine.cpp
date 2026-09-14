#include <NF/Audio/AudioEngine.hpp>
#include <algorithm>
#include <cmath>

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
                           u32 /*sample_rate*/) {
    if (!source.playing || source.buffer == nullptr) return;

    const AudioBuffer& buf = *source.buffer;
    if (buf.samples.empty() || buf.channels == 0 || buf.sample_rate == 0) return;

    // Compute pan/gain (for spatial) or use volume directly (for 2D).
    f32 left_gain = source.volume;
    f32 right_gain = source.volume;

    if (source.spatial) {
        auto pg = compute_3d_pan_gain(listener_pos, listener_forward, listener_up,
                                      source.position, source.spatial_settings);
        left_gain *= pg.left;
        right_gain *= pg.right;
    }

    usize frames_available = buf.frame_count();
    usize frames_to_mix = num_frames;

    for (usize i = 0; i < frames_to_mix; ) {
        if (source.sample_cursor >= frames_available) {
            if (source.looping) {
                source.sample_cursor = source.sample_cursor % frames_available;
            } else {
                source.playing = false;
                source.sample_cursor = 0;
                break;
            }
        }

        usize idx = source.sample_cursor * buf.channels;
        f32 sample_left = buf.samples[idx];
        f32 sample_right = (buf.channels >= 2) ? buf.samples[idx + 1] : sample_left;

        out_left[i] += sample_left * left_gain;
        out_right[i] += sample_right * right_gain;

        ++source.sample_cursor;
        ++i;
    }
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

} // namespace nf::audio
