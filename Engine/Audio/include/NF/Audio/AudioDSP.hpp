#pragma once

#include <NF/Core/Types.hpp>
#include <vector>
#include <cstddef>

namespace nf::audio {

enum class FilterType {
    LowPass,
    HighPass,
    BandPass,
    Notch
};

/// Digital 2nd-order Biquad Filter (Direct Form II Transposed)
/// Based on Robert Bristow-Johnson (RBJ) Audio EQ Cookbook.
class BiquadFilter {
public:
    BiquadFilter();
    BiquadFilter(FilterType type, f32 cutoff_hz, f32 sample_rate, f32 q = 0.7071f, f32 gain_db = 0.0f);

    void configure(FilterType type, f32 cutoff_hz, f32 sample_rate, f32 q = 0.7071f, f32 gain_db = 0.0f);

    [[nodiscard]] FilterType type() const noexcept { return m_type; }
    [[nodiscard]] f32 cutoff() const noexcept { return m_cutoff_hz; }
    [[nodiscard]] f32 sample_rate() const noexcept { return m_sample_rate; }
    [[nodiscard]] f32 q() const noexcept { return m_q; }
    [[nodiscard]] f32 gain_db() const noexcept { return m_gain_db; }

    /// Process a single mono sample
    f32 process_sample(f32 input);

    /// Process an interleaved multichannel buffer (e.g. 1=mono, 2=stereo)
    /// Retains independent state for each channel.
    void process_buffer(f32* buffer, size_t total_samples, u32 channels = 1);

    /// Reset internal state registers
    void reset();

private:
    void calculate_coefficients();

    FilterType m_type = FilterType::LowPass;
    f32 m_cutoff_hz = 1000.0f;
    f32 m_sample_rate = 44100.0f;
    f32 m_q = 0.7071f;
    f32 m_gain_db = 0.0f;

    f32 m_b0 = 1.0f;
    f32 m_b1 = 0.0f;
    f32 m_b2 = 0.0f;
    f32 m_a1 = 0.0f;
    f32 m_a2 = 0.0f;

    struct ChannelState {
        f32 z1 = 0.0f;
        f32 z2 = 0.0f;
    };
    std::vector<ChannelState> m_channels;
};

/// Delay and Echo DSP Effect with Feedback & Wet/Dry mix
class DelayEffect {
public:
    explicit DelayEffect(f32 max_delay_ms = 2000.0f, f32 sample_rate = 44100.0f);

    void configure(f32 delay_time_ms, f32 feedback, f32 wet_mix, f32 dry_mix = 1.0f);
    void set_sample_rate(f32 sample_rate);

    [[nodiscard]] f32 delay_time_ms() const noexcept { return m_delay_ms; }
    [[nodiscard]] f32 feedback() const noexcept { return m_feedback; }
    [[nodiscard]] f32 wet_mix() const noexcept { return m_wet; }
    [[nodiscard]] f32 dry_mix() const noexcept { return m_dry; }

    /// Process an interleaved multichannel buffer
    void process(f32* buffer, size_t total_samples, u32 channels = 1);

    /// Clear internal delay buffer
    void reset();

private:
    f32 m_delay_ms = 300.0f;
    f32 m_feedback = 0.4f;
    f32 m_wet = 0.5f;
    f32 m_dry = 1.0f;
    f32 m_sample_rate = 44100.0f;
    size_t m_max_samples = 88200;

    std::vector<f32> m_delay_buffer;
    size_t m_write_pos = 0;
};

} // namespace nf::audio
