#pragma once

#include <NF/Core/Types.hpp>
#include <atomic>
#include <vector>

namespace nf::audio {

/// SPSC float-stereo ring, capacity in frames (rounded up to a power of two).
/// push() converts planar in to interleaved storage and drops what does not
/// fit (counting overruns — dropping input keeps latency bounded instead of
/// growing it). pop() zero-fills what is not there (counting underruns —
/// a gap renders as silence, never as garbage or a stall).
class AudioRingBuffer {
public:
    explicit AudioRingBuffer(u32 capacity_frames = 65536);

    /// Stores up to `frames` from the planar inputs. Returns frames stored.
    u32 push(const f32* left, const f32* right, u32 frames);
    /// Reads up to `frames` interleaved (LRLR...) into `out`. Returns frames
    /// read; the remainder of `frames` in `out` is silence.
    u32 pop(f32* out_interleaved, u32 frames);

    u32 capacity_frames() const { return m_capacity; }
    /// Frames currently stored (approximate under concurrency; exact when
    /// one side is idle, which is all the tests need).
    u32 stored_frames() const;
    u64 overruns() const { return m_overruns.load(std::memory_order_relaxed); }
    u64 underruns() const { return m_underruns.load(std::memory_order_relaxed); }

private:
    u32 m_capacity = 0;
    u32 m_mask = 0;
    std::vector<f32> m_data; // interleaved, capacity * 2
    std::atomic<u32> m_write = 0; // frames written (producer)
    std::atomic<u32> m_read = 0;  // frames read (consumer)
    std::atomic<u64> m_overruns{0};
    std::atomic<u64> m_underruns{0};
};

/// Linear sample-rate converter (planar stereo in and out). `io_pos` is the
/// fractional input position of the next output frame, carried across calls
/// (blocks are contiguous in time); past-the-end reads hold the last input
/// frame. Returns output frames written. Downsampling aliases slightly (no
/// AA filter); for game audio that is inaudible next to the alternative,
/// which is silence.
u32 resample_linear(const f32* in_l, const f32* in_r, u32 in_frames, u32 in_rate, f32* out_l,
                    f32* out_r, u32 out_capacity, u32 out_rate, double& io_pos);

} // namespace nf::audio
