#include <NF/Audio/Dynamics.hpp>

#include <algorithm>
#include <cmath>

namespace nf::audio {

namespace {

constexpr u32 kFallbackSampleRate = 48000;
constexpr f32 kMinLinearPeak = 1.0e-12f;
constexpr f32 kMaxLookaheadMs = 1000.0f;

f32 db_to_gain(f32 db) {
    return std::pow(10.0f, db * 0.05f);
}

f32 gain_to_db(f32 gain) {
    return -20.0f * std::log10(std::max(gain, kMinLinearPeak));
}

/// One-pole time constant: the fraction of the remaining distance left after
/// `ms` milliseconds. exp(-1 / samples) reaches ~63% in `ms` milliseconds,
/// which is the convention every compressor's attack/release control uses.
/// Zero means "arrive immediately"; a non-finite value is treated the same way.
f32 time_coefficient(f32 ms, u32 sample_rate) {
    if (!(ms > 0.0f)) {
        return 0.0f;
    }
    const f32 samples = ms * 0.001f * static_cast<f32>(sample_rate);
    if (!std::isfinite(samples) || samples <= 0.0f) {
        return 0.0f;
    }
    return std::exp(-1.0f / samples);
}

/// A non-finite value cannot be allowed into the envelopes: tanh(NaN) is NaN,
/// and one NaN sample poisons the device buffer for the rest of the session.
/// Each field falls back to its own neutral value, not to the shipped default:
/// a broken ratio becomes 1 (no compression), a broken makeup becomes 0 dB,
/// and a broken ceiling becomes 0 dBFS. A settings file with one bad number
/// therefore degrades to "that control does nothing", not to "the mix is
/// inexplicably crushed".
CompressorSettings sanitize(const CompressorSettings& in) {
    CompressorSettings out = in;
    out.threshold_db =
        std::isfinite(in.threshold_db) ? std::clamp(in.threshold_db, -120.0f, 24.0f) : 0.0f;
    out.ratio = std::isfinite(in.ratio) ? std::clamp(in.ratio, 1.0f, 100.0f) : 1.0f;
    // Capped rather than clamped: a one-hour release is legitimate technically
    // but indistinguishable from a bug when a player drags a slider.
    out.attack_ms = std::isfinite(in.attack_ms) ? std::clamp(in.attack_ms, 0.0f, 5000.0f) : 0.0f;
    out.release_ms =
        std::isfinite(in.release_ms) ? std::clamp(in.release_ms, 0.0f, 10000.0f) : 0.0f;
    out.knee_db = std::isfinite(in.knee_db) ? std::clamp(in.knee_db, 0.0f, 48.0f) : 0.0f;
    out.makeup_db = std::isfinite(in.makeup_db) ? std::clamp(in.makeup_db, -60.0f, 60.0f) : 0.0f;
    return out;
}

LimiterSettings sanitize(const LimiterSettings& in) {
    LimiterSettings out = in;
    out.ceiling_db = std::isfinite(in.ceiling_db) ? std::clamp(in.ceiling_db, -120.0f, 0.0f) : 0.0f;
    out.release_ms =
        std::isfinite(in.release_ms) ? std::clamp(in.release_ms, 0.0f, 10000.0f) : 0.0f;
    out.lookahead_ms = std::isfinite(in.lookahead_ms)
                           ? std::clamp(in.lookahead_ms, 0.0f, kMaxLookaheadMs)
                           : 0.0f;
    return out;
}

} // namespace

void MasterDynamics::configure(u32 sample_rate, u32 lookahead) {
    m_sample_rate = sample_rate;
    m_lookahead = lookahead;

    // The detector window is `lookahead + 1` samples: the delayed sample plus
    // every sample that will reach the output before it. Ring capacity is the
    // window size, so the deque never has to allocate while audio is running.
    m_max_ring.assign(static_cast<usize>(lookahead) + 1, MaxEntry{});
    m_max_head = 0;
    m_max_size = 0;

    m_delay_left.assign(lookahead, 0.0f);
    m_delay_right.assign(lookahead, 0.0f);
    m_delay_pos = 0;
    m_sequence = 0;

    // A new delay length is a new latency contract; starting from unity gain
    // is the only choice that does not apply a stale reduction to samples that
    // never went through the old detector.
    m_limiter_gain = 1.0f;
    m_limiter_gr_db = 0.0f;
}

void MasterDynamics::reset() {
    m_compressor_gr_db = 0.0f;
    m_limiter_gain = 1.0f;
    m_limiter_gr_db = 0.0f;
    m_sequence = 0;
    m_delay_pos = 0;
    std::fill(m_max_ring.begin(), m_max_ring.end(), MaxEntry{});
    m_max_head = 0;
    m_max_size = 0;
    // A reset deliberately keeps the CURRENT rate and lookahead: the delay
    // length is configuration, while the samples and envelopes are state.
    std::fill(m_delay_left.begin(), m_delay_left.end(), 0.0f);
    std::fill(m_delay_right.begin(), m_delay_right.end(), 0.0f);
}

void MasterDynamics::process(f32* left, f32* right, usize frames, u32 sample_rate) {
    if (left == nullptr || right == nullptr || frames == 0) {
        return;
    }
    if (sample_rate == 0) {
        sample_rate = kFallbackSampleRate;
    }

    const CompressorSettings compressor = sanitize(m_settings.compressor);
    const LimiterSettings limiter = sanitize(m_settings.limiter);

    // Even a disabled limiter reports its configured latency: a caller that
    // aligns tracks before enabling the stage should not have to query it
    // twice. Configuration only rebuilds the delay lines; it touches no audio.
    const long long lookahead_long =
        std::llround(static_cast<double>(limiter.lookahead_ms) * 0.001 *
                     static_cast<double>(sample_rate));
    const u32 lookahead = static_cast<u32>(
        std::clamp<long long>(lookahead_long, 0, 192000));
    if (sample_rate != m_sample_rate || lookahead != m_lookahead) {
        configure(sample_rate, lookahead);
    }

    if (!compressor.enabled && !limiter.enabled) {
        m_compressor_gr_db = 0.0f;
        m_limiter_gr_db = 0.0f;
        return; // bit-exact: not one sample is read or written
    }

    // --- compressor ---------------------------------------------------------
    if (compressor.enabled) {
        const f32 attack = time_coefficient(compressor.attack_ms, sample_rate);
        const f32 release = time_coefficient(compressor.release_ms, sample_rate);
        const f32 slope = 1.0f - 1.0f / compressor.ratio;
        const f32 half_knee = compressor.knee_db * 0.5f;
        const bool can_compress =
            slope > 0.0f || compressor.makeup_db != 0.0f;

        if (can_compress) {
            for (usize i = 0; i < frames; ++i) {
                const f32 peak =
                    std::max(std::fabs(left[i]), std::fabs(right[i]));
                f32 reduction = 0.0f;

                if (slope > 0.0f && peak > kMinLinearPeak) {
                    const f32 level_db = 20.0f * std::log10(peak);
                    const f32 over = level_db - compressor.threshold_db;
                    if (compressor.knee_db > 0.0f && over > -half_knee &&
                        over < half_knee) {
                        // Quadratic interpolation across the knee: the gain
                        // curve is continuous in value AND slope at both
                        // edges, which is what makes it inaudible.
                        const f32 into_knee = over + half_knee;
                        reduction = slope * into_knee * into_knee /
                                    (2.0f * compressor.knee_db);
                    } else if (over >= half_knee) {
                        reduction = slope * over;
                    }
                }

                if (!std::isfinite(reduction) || reduction < 0.0f) {
                    reduction = 0.0f;
                }
                const f32 coefficient = reduction > m_compressor_gr_db ? attack : release;
                m_compressor_gr_db =
                    reduction + coefficient * (m_compressor_gr_db - reduction);
                if (!std::isfinite(m_compressor_gr_db)) {
                    m_compressor_gr_db = 0.0f;
                }

                const f32 gain_db = compressor.makeup_db - m_compressor_gr_db;
                if (gain_db != 0.0f) {
                    const f32 gain = db_to_gain(gain_db);
                    left[i] *= gain;
                    right[i] *= gain;
                }
            }
        } else {
            // ratio 1 and 0 dB makeup: a disabled compressor by another name.
            // Still run the detector so the meter reads zero rather than stale.
            m_compressor_gr_db = 0.0f;
        }
    } else {
        m_compressor_gr_db = 0.0f;
    }

    // The compressor's gain is the last thing that may add gain. If the
    // limiter is off, the block is done and the samples are untouched from
    // here on.
    if (!limiter.enabled) {
        m_limiter_gr_db = 0.0f;
        return;
    }

    // --- limiter ------------------------------------------------------------
    const f32 ceiling = db_to_gain(limiter.ceiling_db);
    const f32 ceiling_gain = std::isfinite(ceiling) ? ceiling : 1.0f;
    const f32 release = time_coefficient(limiter.release_ms, sample_rate);
    const usize capacity = m_max_ring.size();
    const usize delay_length = m_delay_left.size();

    for (usize i = 0; i < frames; ++i) {
        const f32 in_left = left[i];
        const f32 in_right = right[i];
        const f32 peak = std::max(std::fabs(in_left), std::fabs(in_right));

        // Drop detector entries that have fallen out of the lookahead window
        // BEFORE inserting this sample. The window is [n - lookahead, n] and
        // therefore holds at most lookahead + 1 detector values.
        const u64 sequence = m_sequence;
        while (m_max_size > 0 &&
               m_max_ring[m_max_head].sequence + static_cast<u64>(m_lookahead) <
                   sequence) {
            m_max_head = (m_max_head + 1) % capacity;
            --m_max_size;
        }

        // Monotonic deque: everything older and quieter than this sample can
        // never be the maximum again, so drop it from the back. Newer values
        // dominate older equal ones for the same reason.
        while (m_max_size > 0) {
            const usize back =
                (m_max_head + m_max_size - 1) % capacity;
            if (m_max_ring[back].value <= peak) {
                --m_max_size;
            } else {
                break;
            }
        }
        const usize tail = (m_max_head + m_max_size) % capacity;
        m_max_ring[tail] = MaxEntry{sequence, peak};
        ++m_max_size;

        const f32 window_peak = m_max_ring[m_max_head].value;
        f32 target = 1.0f;
        if (std::isfinite(window_peak) && window_peak > ceiling_gain &&
            window_peak > kMinLinearPeak) {
            target = ceiling_gain / window_peak;
        }
        target = std::clamp(target, 0.0f, 1.0f);

        // Instant attack is the point of a lookahead limiter: the gain is
        // already down when the delayed peak arrives. Release is the only
        // smoothed direction.
        if (target < m_limiter_gain) {
            m_limiter_gain = target;
        } else {
            m_limiter_gain = target + release * (m_limiter_gain - target);
        }
        if (!std::isfinite(m_limiter_gain)) {
            m_limiter_gain = 1.0f;
        }

        f32 out_left = in_left;
        f32 out_right = in_right;
        if (delay_length > 0) {
            // Reads the sample that entered `lookahead` frames ago, then
            // stores today's sample for the future detector to protect.
            out_left = m_delay_left[m_delay_pos];
            out_right = m_delay_right[m_delay_pos];
            m_delay_left[m_delay_pos] = in_left;
            m_delay_right[m_delay_pos] = in_right;
            m_delay_pos = (m_delay_pos + 1) % delay_length;
        }

        if (m_limiter_gain < 1.0f) {
            out_left *= m_limiter_gain;
            out_right *= m_limiter_gain;
            // Floating-point multiplication can land one ulp above the
            // ceiling; a brickwall means brickwall, so clamp the final pair.
            out_left = std::clamp(out_left, -ceiling_gain, ceiling_gain);
            out_right = std::clamp(out_right, -ceiling_gain, ceiling_gain);
        }

        left[i] = out_left;
        right[i] = out_right;
        ++m_sequence;
    }

    m_limiter_gr_db = std::clamp(gain_to_db(m_limiter_gain), 0.0f, 200.0f);
}

} // namespace nf::audio
