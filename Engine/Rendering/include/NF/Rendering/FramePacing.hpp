#pragma once

// NF/Rendering/FramePacing.hpp — rolling frame-time telemetry (Phase 41).
//
// This is a MEASUREMENT, not a pacer. It never sleeps, never waits for a
// presentation fence, and never changes the render scale. A host loop pushes the
// wall time it actually observed plus an optional GPU duration, then reads back
// a fixed-size view of the recent frames.
//
// Why a window: one frame cannot distinguish a steady 60 fps from a run that is
// usually 60 fps but hitches every second. The default window is 120 frames —
// about two seconds at 60 fps — and every statistic below describes exactly that
// window, not the whole session.
//
// Raw wall time is deliberate. `Runtime::update` receives a clamped dt so a
// debugger pause or a scene load cannot poison the simulation; telemetry must
// see that pause, because it is exactly the kind of frame the player felt. The
// host loop is therefore the only correct source of the CPU sample. Runtime's
// simulation clock and this window are intentionally separate inputs.
//
// The 1% low is the mean of the slowest `max(1, ceil(N / 100))` frames in the
// window. It is not the single worst frame (one debugger break should not define
// the number forever) and it is not an interpolated percentile.
//
// Bound classification compares the GPU average with the CPU wall-clock
// average. A GPU frame near the wall clock means the GPU is the constraint; a
// much smaller GPU frame means the frame finished on the CPU side. "CPU" here
// includes waiting on presentation/vsync: the wall clock contains that wait, and
// that is why the divisor is frame time rather than recorded CPU work. At least
// `kMinGpuSamplesForBound` GPU samples must be present, otherwise one late
// timestamp could label the whole window.
//
// GPU samples are optional and may be supplied on only some frames. They are
// stored in the same ring as the CPU sample, so a device that stops reporting
// timestamps ages out of the window instead of pinning an old verdict.

#include <NF/Core/Types.hpp>

#include <vector>

namespace nf::rendering {

/// Operational bottleneck verdict for one window. `Unknown` is the honest state
/// before enough paired GPU samples exist; it is not an error.
enum class FramePacingBound : u8 {
    Unknown = 0,
    Cpu = 1,
    Gpu = 2,
    Balanced = 3,
};

/// Stable technical name for logs. UI copy goes through Localization instead.
const char* frame_pacing_bound_name(FramePacingBound bound);

struct FramePacingStats {
    u32 samples = 0;          // CPU frames currently in the window
    u32 gpu_samples = 0;      // frames in that window with a GPU timestamp
    f64 average_frame_ms = 0.0;
    f64 average_fps = 0.0;
    f64 low_1pct_frame_ms = 0.0;
    f64 low_1pct_fps = 0.0;
    /// Population variance/stddev of the same frame durations, in ms and ms².
    /// Displaying standard deviation is usually more readable than variance.
    f64 variance_ms2 = 0.0;
    f64 stddev_ms = 0.0;
    f64 min_frame_ms = 0.0;
    f64 max_frame_ms = 0.0;
    f64 gpu_average_ms = 0.0;
    FramePacingBound bound = FramePacingBound::Unknown;
};

class FramePacing {
public:
    static constexpr u32 kDefaultWindow = 120;
    static constexpr u32 kMinWindow = 8;
    static constexpr u32 kMaxWindow = 1200;
    /// Below this many paired GPU samples a ratio is noise, not evidence.
    static constexpr u32 kMinGpuSamplesForBound = 8;
    /// GPU average / CPU wall-clock average thresholds. At or above 0.90 the GPU
    /// fills the frame; at or below 0.75 there is substantial non-GPU headroom.
    static constexpr f64 kGpuBoundRatio = 0.90;
    static constexpr f64 kCpuBoundRatio = 0.75;

    explicit FramePacing(u32 window = kDefaultWindow);

    /// Resizes the ring and CLEARS all samples. A window must never mix
    /// differently-sized histories, so a reconfigure is also a reset. The size
    /// is clamped to [kMinWindow, kMaxWindow].
    void set_window(u32 window);
    u32 window() const { return static_cast<u32>(m_cpu_ms.size()); }

    /// Pushes one host-loop frame. `cpu_frame_s` is required and must be finite
    /// and positive; an invalid CPU sample is ignored, and its optional GPU
    /// sample is ignored with it because a GPU duration without a frame duration
    /// cannot be placed on the timeline. `gpu_frame_s` is stored only when
    /// `gpu_valid` is true, is finite, and is positive.
    void push_frame(f64 cpu_frame_s, f64 gpu_frame_s = 0.0, bool gpu_valid = false);

    /// Computes statistics over the current window. Calling this does not
    /// consume or advance samples, so the editor can read it every frame.
    FramePacingStats stats() const;

    /// Forgets every sample. Hosts call this after a scene load or a play-mode
    /// switch so the previous content's hitch does not describe the new one.
    void reset();

private:
    std::vector<f64> m_cpu_ms;
    std::vector<f64> m_gpu_ms; // 0 = no GPU sample paired with that CPU frame
    size_t m_head = 0;         // next slot to overwrite
    size_t m_count = 0;
};

} // namespace nf::rendering
