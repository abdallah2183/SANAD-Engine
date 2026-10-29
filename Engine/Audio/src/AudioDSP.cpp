#include <NF/Audio/AudioDSP.hpp>
#include <cmath>
#include <algorithm>

namespace nf::audio {

constexpr f32 PI = 3.14159265358979323846f;

BiquadFilter::BiquadFilter() {
    calculate_coefficients();
}

BiquadFilter::BiquadFilter(FilterType type, f32 cutoff_hz, f32 sample_rate, f32 q, f32 gain_db) {
    configure(type, cutoff_hz, sample_rate, q, gain_db);
}

void BiquadFilter::configure(FilterType type, f32 cutoff_hz, f32 sample_rate, f32 q, f32 gain_db) {
    m_type = type;
    m_cutoff_hz = std::max(10.0f, std::min(cutoff_hz, sample_rate * 0.49f));
    m_sample_rate = std::max(8000.0f, sample_rate);
    m_q = std::max(0.01f, q);
    m_gain_db = gain_db;
    calculate_coefficients();
}

void BiquadFilter::calculate_coefficients() {
    const f32 omega = 2.0f * PI * (m_cutoff_hz / m_sample_rate);
    const f32 sin_w = std::sin(omega);
    const f32 cos_w = std::cos(omega);
    const f32 alpha = sin_w / (2.0f * m_q);

    f32 b0 = 1.0f;
    f32 b1 = 0.0f;
    f32 b2 = 0.0f;
    f32 a0 = 1.0f;
    f32 a1 = 0.0f;
    f32 a2 = 0.0f;

    switch (m_type) {
    case FilterType::LowPass:
        b0 = (1.0f - cos_w) * 0.5f;
        b1 = 1.0f - cos_w;
        b2 = (1.0f - cos_w) * 0.5f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_w;
        a2 = 1.0f - alpha;
        break;
    case FilterType::HighPass:
        b0 = (1.0f + cos_w) * 0.5f;
        b1 = -(1.0f + cos_w);
        b2 = (1.0f + cos_w) * 0.5f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_w;
        a2 = 1.0f - alpha;
        break;
    case FilterType::BandPass:
        b0 = alpha;
        b1 = 0.0f;
        b2 = -alpha;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_w;
        a2 = 1.0f - alpha;
        break;
    case FilterType::Notch:
        b0 = 1.0f;
        b1 = -2.0f * cos_w;
        b2 = 1.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_w;
        a2 = 1.0f - alpha;
        break;
    }

    const f32 inv_a0 = 1.0f / a0;
    m_b0 = b0 * inv_a0;
    m_b1 = b1 * inv_a0;
    m_b2 = b2 * inv_a0;
    m_a1 = a1 * inv_a0;
    m_a2 = a2 * inv_a0;
}

f32 BiquadFilter::process_sample(f32 input) {
    if (m_channels.empty()) {
        m_channels.resize(1);
    }
    ChannelState& state = m_channels[0];
    const f32 output = m_b0 * input + state.z1;
    state.z1 = m_b1 * input - m_a1 * output + state.z2;
    state.z2 = m_b2 * input - m_a2 * output;
    return output;
}

void BiquadFilter::process_buffer(f32* buffer, size_t total_samples, u32 channels) {
    if (!buffer || total_samples == 0 || channels == 0) {
        return;
    }
    if (m_channels.size() < channels) {
        m_channels.resize(channels);
    }

    const size_t frames = total_samples / channels;
    for (size_t f = 0; f < frames; ++f) {
        for (u32 c = 0; c < channels; ++c) {
            const size_t idx = f * channels + c;
            const f32 x = buffer[idx];
            ChannelState& st = m_channels[c];
            const f32 y = m_b0 * x + st.z1;
            st.z1 = m_b1 * x - m_a1 * y + st.z2;
            st.z2 = m_b2 * x - m_a2 * y;
            buffer[idx] = y;
        }
    }
}

void BiquadFilter::reset() {
    for (auto& ch : m_channels) {
        ch.z1 = 0.0f;
        ch.z2 = 0.0f;
    }
}

// ----------------------------------------------------------------------------
// DelayEffect
// ----------------------------------------------------------------------------

DelayEffect::DelayEffect(f32 max_delay_ms, f32 sample_rate)
    : m_sample_rate(std::max(8000.0f, sample_rate)) {
    const f32 max_ms = std::max(50.0f, max_delay_ms);
    m_max_samples = static_cast<size_t>(std::ceil((max_ms * 0.001f) * m_sample_rate)) + 16;
    m_delay_buffer.assign(m_max_samples, 0.0f);
    m_write_pos = 0;
}

void DelayEffect::configure(f32 delay_time_ms, f32 feedback, f32 wet_mix, f32 dry_mix) {
    m_delay_ms = std::max(0.0f, delay_time_ms);
    m_feedback = std::clamp(feedback, 0.0f, 0.98f);
    m_wet = std::clamp(wet_mix, 0.0f, 1.0f);
    m_dry = std::clamp(dry_mix, 0.0f, 1.0f);
}

void DelayEffect::set_sample_rate(f32 sample_rate) {
    m_sample_rate = std::max(8000.0f, sample_rate);
    const f32 max_ms = std::max(50.0f, (static_cast<f32>(m_max_samples) / m_sample_rate) * 1000.0f);
    m_max_samples = static_cast<size_t>(std::ceil((max_ms * 0.001f) * m_sample_rate)) + 16;
    m_delay_buffer.assign(m_max_samples, 0.0f);
    m_write_pos = 0;
}

void DelayEffect::process(f32* buffer, size_t total_samples, u32 channels) {
    if (!buffer || total_samples == 0 || channels == 0 || m_delay_buffer.empty()) {
        return;
    }

    const f32 delay_samples = std::clamp((m_delay_ms * 0.001f) * m_sample_rate, 0.0f, static_cast<f32>(m_max_samples - 2));
    const size_t d_int = static_cast<size_t>(delay_samples);
    const f32 d_frac = delay_samples - static_cast<f32>(d_int);
    const size_t buf_size = m_delay_buffer.size();

    const size_t frames = total_samples / channels;
    for (size_t f = 0; f < frames; ++f) {
        // Average channels for delay line input
        f32 input_sum = 0.0f;
        for (u32 c = 0; c < channels; ++c) {
            input_sum += buffer[f * channels + c];
        }
        const f32 mono_in = input_sum / static_cast<f32>(channels);

        // Read interpolated delay
        const size_t r0 = (m_write_pos + buf_size - d_int) % buf_size;
        const size_t r1 = (r0 + buf_size - 1) % buf_size;
        const f32 delayed = (1.0f - d_frac) * m_delay_buffer[r0] + d_frac * m_delay_buffer[r1];

        // Write to delay buffer with feedback
        m_delay_buffer[m_write_pos] = std::clamp(mono_in + m_feedback * delayed, -2.0f, 2.0f);
        m_write_pos = (m_write_pos + 1) % buf_size;

        // Apply mix to channels
        for (u32 c = 0; c < channels; ++c) {
            const size_t idx = f * channels + c;
            buffer[idx] = m_dry * buffer[idx] + m_wet * delayed;
        }
    }
}

void DelayEffect::reset() {
    std::fill(m_delay_buffer.begin(), m_delay_buffer.end(), 0.0f);
    m_write_pos = 0;
}

} // namespace nf::audio
