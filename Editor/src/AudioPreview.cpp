#include <NF/Editor/AudioPreview.hpp>

#include <cmath>

namespace nf::editor {

WaveformEnvelope compute_waveform(const f32* samples, usize sample_count, u32 channels,
                                  usize buckets) {
    WaveformEnvelope out;
    if (samples == nullptr || channels == 0 || buckets == 0) {
        return out;
    }
    const usize frames = sample_count / channels;
    if (frames == 0) {
        return out;
    }
    // More buckets than frames would leave trailing buckets reading past the
    // buffer or reporting a single sample as a range; clamp instead.
    if (buckets > frames) {
        buckets = frames;
    }
    // The loudest channel represents the buffer: for stereo content a quiet
    // left and a loud right should show a loud envelope, not an average that
    // hides half the signal.
    u32 loud = 0;
    float loud_peak = -1.0f;
    for (u32 c = 0; c < channels; ++c) {
        float p = 0.0f;
        for (usize i = c; i < sample_count; i += channels) {
            const float a = std::fabs(samples[i]);
            if (a > p) {
                p = a;
            }
        }
        if (p > loud_peak) {
            loud_peak = p;
            loud = c;
        }
    }

    out.min.resize(buckets, 0.0f);
    out.max.resize(buckets, 0.0f);
    out.peak = 0.0f;

    // Spread `frames` samples over `buckets` as evenly as possible without
    // floats: bucket b owns [b*frames/buckets, (b+1)*frames/buckets). The end
    // bound is clamped to frames so the last bucket picks up any remainder.
    for (usize b = 0; b < buckets; ++b) {
        const usize begin = (b * frames) / buckets;
        usize end = ((b + 1) * frames) / buckets;
        if (end <= begin) {
            end = begin + 1;
        }
        if (end > frames) {
            end = frames;
        }
        float mn = samples[begin * channels + loud];
        float mx = mn;
        for (usize i = begin; i < end; ++i) {
            const float v = samples[i * channels + loud];
            if (v < mn) {
                mn = v;
            }
            if (v > mx) {
                mx = v;
            }
        }
        out.min[b] = mn;
        out.max[b] = mx;
        const float p = std::fabs(mn);
        if (p > out.peak) {
            out.peak = p;
        }
        const float pa = std::fabs(mx);
        if (pa > out.peak) {
            out.peak = pa;
        }
    }
    return out;
}

} // namespace nf::editor
