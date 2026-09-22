#include <NF/Audio/Reverb.hpp>
#include <algorithm>
#include <cmath>

namespace nf::audio {

namespace {

/// Weight of a zone at distance `d`: full inside the inner radius, linear to
/// zero at the outer radius. Zones with a degenerate (zero or negative) band
/// contribute only when the listener is inside the inner radius itself.
f32 zone_weight(const ReverbZone& zone, f32 distance) {
    if (distance <= zone.inner_radius) {
        return 1.0f;
    }
    if (distance >= zone.radius) {
        return 0.0f;
    }
    const f32 band = zone.radius - zone.inner_radius;
    if (band <= 1e-8f) {
        return 0.0f;
    }
    return 1.0f - (distance - zone.inner_radius) / band;
}

} // namespace

ReverbSample compute_reverb_at(const ReverbZone* zones, usize count,
                               const Vec3& listener) {
    ReverbSample result;
    f32 best_weight = 0.0f;

    for (usize i = 0; i < count; ++i) {
        const ReverbZone& zone = zones[i];
        const f32 distance = (zone.position - listener).length();
        const f32 weight = zone_weight(zone, distance);
        if (weight > best_weight) {
            best_weight = weight;
            result.wet_gain = zone.wet_gain * weight;
            result.decay_seconds = zone.decay_seconds;
            result.pre_delay_seconds = zone.pre_delay_seconds;
            result.echo_spacing_seconds = zone.echo_spacing_seconds;
            result.zone_index = static_cast<u32>(i);
            result.active = true;
        }
    }

    if (!result.active) {
        result.wet_gain = 0.0f;
    }
    return result;
}

void EchoProcessor::configure(u32 sample_rate, const ReverbSample& reverb) {
    m_wet = reverb.wet_gain;
    m_feedback = 0.0f;
    m_pre_delay = 0;
    m_spacing = 0;

    if (sample_rate == 0) {
        return;
    }

    m_pre_delay = static_cast<usize>(
        reverb.pre_delay_seconds * static_cast<f32>(sample_rate) + 0.5f);
    // A ring of zero frames would read its own input, so the shortest real
    // pre-delay is one frame. Clamping here (rather than only sizing the ring
    // below) keeps pre_delay_frames() equal to the delay the processor
    // actually applies, which is what makes the documented
    // "echo n lands at pre_delay + n * spacing" true for a zero-delay zone.
    m_pre_delay = std::max<usize>(m_pre_delay, 1);
    m_spacing = static_cast<usize>(
        reverb.echo_spacing_seconds * static_cast<f32>(sample_rate) + 0.5f);
    // A comb of fewer than one frame would read its own input; clamp to 1.
    m_spacing = std::max<usize>(m_spacing, 1);

    if (reverb.decay_seconds > 0.0f && reverb.echo_spacing_seconds > 0.0f) {
        // 0.001 ^ (spacing / decay): the spacing-delayed echo has fallen
        // 60 dB when `decay_seconds` of spacing-spaced echoes have passed.
        m_feedback = std::pow(0.001f,
                              reverb.echo_spacing_seconds / reverb.decay_seconds);
    }

    m_comb.assign(m_spacing, 0.0f);
    m_pre.assign(std::max<usize>(m_pre_delay, 1), 0.0f);
    m_comb_cursor = 0;
    m_pre_cursor = 0;
}

void EchoProcessor::reset() {
    if (!m_comb.empty()) {
        std::fill(m_comb.begin(), m_comb.end(), 0.0f);
    }
    if (!m_pre.empty()) {
        std::fill(m_pre.begin(), m_pre.end(), 0.0f);
    }
    m_comb_cursor = 0;
    m_pre_cursor = 0;
}

void EchoProcessor::process(const f32* dry, f32* wet, usize frames) {
    if (m_comb.empty()) {
        for (usize i = 0; i < frames; ++i) {
            wet[i] = 0.0f;
        }
        return;
    }

    const usize comb_len = m_comb.size();
    const usize pre_len = m_pre.size();
    for (usize i = 0; i < frames; ++i) {
        // Feedback comb: y[n] = x[n] + f * y[n - spacing]; we emit the delayed
        // y (the echo), not the sum, so the dry path stays with the caller.
        const f32 delayed = m_comb[m_comb_cursor];
        m_comb[m_comb_cursor] = dry[i] + m_feedback * delayed;
        m_comb_cursor = (m_comb_cursor + 1) % comb_len;

        // Pre-delay ring: push the comb output, emit what was pushed
        // pre_delay frames ago.
        const f32 shifted = m_pre[m_pre_cursor];
        m_pre[m_pre_cursor] = delayed;
        m_pre_cursor = (m_pre_cursor + 1) % pre_len;

        wet[i] = shifted * m_wet;
    }
}

} // namespace nf::audio
