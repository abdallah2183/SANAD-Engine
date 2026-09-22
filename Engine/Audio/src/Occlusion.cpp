#include <NF/Audio/Occlusion.hpp>
#include <algorithm>
#include <cmath>

namespace nf::audio {

bool segment_intersects_aabb(const Vec3& a, const Vec3& b,
                             const OccluderAabb& box) {
    // Standard slab test, parametrised on t in [0, 1] along the segment.
    f32 t_enter = 0.0f;
    f32 t_exit = 1.0f;

    const Vec3 d = b - a;
    const f32 axes_min[3] = {box.min.x, box.min.y, box.min.z};
    const f32 axes_max[3] = {box.max.x, box.max.y, box.max.z};
    const f32 axes_a[3] = {a.x, a.y, a.z};

    for (u32 axis = 0; axis < 3; ++axis) {
        const f32 delta = (axis == 0 ? d.x : (axis == 1 ? d.y : d.z));
        if (std::fabs(delta) < 1e-9f) {
            // Segment is parallel to this slab pair: it can only pass if the
            // whole segment lies between the slabs.
            if (axes_a[axis] < axes_min[axis] || axes_a[axis] > axes_max[axis]) {
                return false;
            }
            continue;
        }

        const f32 inv = 1.0f / delta;
        f32 t0 = (axes_min[axis] - axes_a[axis]) * inv;
        f32 t1 = (axes_max[axis] - axes_a[axis]) * inv;
        if (t0 > t1) {
            const f32 tmp = t0;
            t0 = t1;
            t1 = tmp;
        }

        t_enter = std::max(t_enter, t0);
        t_exit = std::min(t_exit, t1);
        if (t_enter > t_exit) {
            return false;
        }
    }

    return t_enter <= t_exit;
}

u32 count_occluders_crossed(const Vec3& a, const Vec3& b,
                            const OccluderAabb* boxes, usize count) {
    u32 walls = 0;
    for (usize i = 0; i < count; ++i) {
        if (segment_intersects_aabb(a, b, boxes[i])) {
            ++walls;
        }
    }
    return walls;
}

f32 occlusion_amount(u32 walls) {
    // openness = (1 - k)^walls, occlusion = 1 - openness. Exact for the pin
    // values 1 wall -> 0.5, 2 -> 0.75, ...
    const f32 openness = std::pow(1.0f - kPerWallMuffle,
                                  static_cast<f32>(walls));
    return 1.0f - openness;
}

f32 occlusion_lowpass_cutoff(f32 occlusion) {
    const f32 t = std::clamp(occlusion, 0.0f, 1.0f);
    const f32 ratio = kOcclusionClosedCutoffHz / kOcclusionOpenCutoffHz;
    return kOcclusionOpenCutoffHz * std::pow(ratio, t);
}

void LowPassFilter::set_cutoff(f32 cutoff_hz, u32 sample_rate) {
    if (sample_rate == 0 || cutoff_hz <= 0.0f) {
        // Degenerate input: pass audio through unchanged rather than silence.
        m_a = 1.0f;
        return;
    }
    const f32 omega = 6.2831853f * cutoff_hz / static_cast<f32>(sample_rate);
    // Clamp the exponent so a huge cutoff can't push exp() to exactly 0 and
    // then past it into denormal territory on some libmsvc builds.
    m_a = 1.0f - std::exp(-std::min(omega, 20.0f));
}

void LowPassFilter::reset() {
    m_state = 0.0f;
}

f32 LowPassFilter::process(f32 input) {
    m_state += m_a * (input - m_state);
    return m_state;
}

void LowPassFilter::process_buffer(const f32* input, f32* output,
                                   usize frames) {
    for (usize i = 0; i < frames; ++i) {
        output[i] = process(input[i]);
    }
}

} // namespace nf::audio
