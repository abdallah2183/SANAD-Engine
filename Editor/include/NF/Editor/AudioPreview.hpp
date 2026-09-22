#pragma once

// NF/Editor/AudioPreview.hpp — waveform envelope for the audio inspector.
//
// The audio inspector shows the buffer's shape, not just its name: a flat line
// means silence, a symmetric envelope means a tone, a burst at the start means
// a one-shot. Drawing every sample is pointless at inspector scale (thousands
// of samples per pixel), so the buffer collapses to a min/max envelope — the
// pair that actually determines what the curve looks like on screen.
//
// Pure and device-free so EditorTests covers it; the panel draws from it. Rate,
// channel count and duration stay on the AudioBuffer the caller already holds,
// so the envelope carries only what the draw needs.

#include <NF/Core/Types.hpp>

#include <cstddef>
#include <vector>

namespace nf::editor {

struct WaveformEnvelope {
    std::vector<float> min; // lowest sample in the bucket
    std::vector<float> max; // highest sample in the bucket
    float peak = 0.0f;      // loudest |sample| overall

    [[nodiscard]] usize buckets() const { return min.size(); }
    [[nodiscard]] bool valid() const { return !min.empty() && min.size() == max.size(); }
};

/// Builds a `buckets`-wide min/max envelope over interleaved float samples,
/// reading the loudest channel (channel 0 for mono). Returns an invalid result
/// when there is nothing to show: null samples, zero channels, or a non-positive
/// bucket count. A bucket count larger than the frame count is clamped to the
/// frame count — one sample per bucket is the useful limit, and beyond that the
/// extra buckets would all hold the same single sample.
WaveformEnvelope compute_waveform(const f32* samples, usize sample_count, u32 channels,
                                  usize buckets);

} // namespace nf::editor
