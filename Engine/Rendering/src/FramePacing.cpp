#include <NF/Rendering/FramePacing.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace nf::rendering {

namespace {

bool usable_seconds(f64 seconds) {
    return std::isfinite(seconds) && seconds > 0.0;
}

} // namespace

FramePacing::FramePacing(u32 window) {
    set_window(window);
}

void FramePacing::set_window(u32 window) {
    const u32 size = std::clamp(window, kMinWindow, kMaxWindow);
    m_cpu_ms.assign(size, 0.0);
    m_gpu_ms.assign(size, 0.0);
    m_head = 0;
    m_count = 0;
}

void FramePacing::push_frame(f64 cpu_frame_s, f64 gpu_frame_s, bool gpu_valid) {
    if (!usable_seconds(cpu_frame_s)) {
        return;
    }

    const bool has_gpu = gpu_valid && usable_seconds(gpu_frame_s);
    m_cpu_ms[m_head] = cpu_frame_s * 1000.0;
    m_gpu_ms[m_head] = has_gpu ? gpu_frame_s * 1000.0 : 0.0;
    m_head = (m_head + 1) % m_cpu_ms.size();
    if (m_count < m_cpu_ms.size()) {
        ++m_count;
    }
}

FramePacingStats FramePacing::stats() const {
    FramePacingStats out{};
    if (m_count == 0) {
        return out;
    }

    const size_t n = m_count;
    const size_t ring_size = m_cpu_ms.size();
    const size_t start = (m_head + ring_size - n) % ring_size;

    std::vector<f64> sorted_ms;
    sorted_ms.reserve(n);
    f64 sum_ms = 0.0;
    f64 gpu_sum_ms = 0.0;
    u32 gpu_count = 0;
    f64 minimum_ms = std::numeric_limits<f64>::infinity();
    f64 maximum_ms = 0.0;

    for (size_t i = 0; i < n; ++i) {
        const size_t index = (start + i) % ring_size;
        const f64 frame_ms = m_cpu_ms[index];
        sum_ms += frame_ms;
        sorted_ms.push_back(frame_ms);
        minimum_ms = std::min(minimum_ms, frame_ms);
        maximum_ms = std::max(maximum_ms, frame_ms);
        if (m_gpu_ms[index] > 0.0) {
            gpu_sum_ms += m_gpu_ms[index];
            ++gpu_count;
        }
    }

    const f64 average_ms = sum_ms / static_cast<f64>(n);
    f64 squared_deviations = 0.0;
    for (const f64 frame_ms : sorted_ms) {
        const f64 delta = frame_ms - average_ms;
        squared_deviations += delta * delta;
    }
    const f64 variance_ms2 = squared_deviations / static_cast<f64>(n);

    // Ceiling of n/100 without floating-point rounding: for n=120 this is the
    // average of the two slowest frames; for n=100 it is exactly one frame.
    const size_t slow_count =
        std::min(n, std::max<size_t>(1, (n + 99) / 100));
    std::sort(sorted_ms.begin(), sorted_ms.end(),
              [](f64 a, f64 b) { return a > b; });

    f64 slow_sum_ms = 0.0;
    for (size_t i = 0; i < slow_count; ++i) {
        slow_sum_ms += sorted_ms[i];
    }
    const f64 low_ms = slow_sum_ms / static_cast<f64>(slow_count);

    out.samples = static_cast<u32>(n);
    out.gpu_samples = gpu_count;
    out.average_frame_ms = average_ms;
    out.average_fps = average_ms > 0.0 ? 1000.0 / average_ms : 0.0;
    out.low_1pct_frame_ms = low_ms;
    out.low_1pct_fps = low_ms > 0.0 ? 1000.0 / low_ms : 0.0;
    out.variance_ms2 = variance_ms2;
    out.stddev_ms = std::sqrt(variance_ms2);
    out.min_frame_ms = minimum_ms;
    out.max_frame_ms = maximum_ms;

    if (gpu_count > 0) {
        out.gpu_average_ms = gpu_sum_ms / static_cast<f64>(gpu_count);
    }
    if (gpu_count >= kMinGpuSamplesForBound && average_ms > 0.0 &&
        out.gpu_average_ms > 0.0) {
        const f64 ratio = out.gpu_average_ms / average_ms;
        if (ratio >= kGpuBoundRatio) {
            out.bound = FramePacingBound::Gpu;
        } else if (ratio <= kCpuBoundRatio) {
            out.bound = FramePacingBound::Cpu;
        } else {
            out.bound = FramePacingBound::Balanced;
        }
    }
    return out;
}

void FramePacing::reset() {
    std::fill(m_cpu_ms.begin(), m_cpu_ms.end(), 0.0);
    std::fill(m_gpu_ms.begin(), m_gpu_ms.end(), 0.0);
    m_head = 0;
    m_count = 0;
}

const char* frame_pacing_bound_name(FramePacingBound bound) {
    switch (bound) {
    case FramePacingBound::Cpu:
        return "cpu";
    case FramePacingBound::Gpu:
        return "gpu";
    case FramePacingBound::Balanced:
        return "balanced";
    case FramePacingBound::Unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace nf::rendering
