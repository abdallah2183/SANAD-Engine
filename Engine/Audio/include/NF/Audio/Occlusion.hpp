#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

namespace nf::audio {

/// An axis-aligned box that blocks sound (a wall chunk, a door, a building).
struct OccluderAabb {
    Vec3 min{};
    Vec3 max{};
};

/// True when the segment `a` -> `b` passes through the box (slab test).
bool segment_intersects_aabb(const Vec3& a, const Vec3& b,
                             const OccluderAabb& box);

/// How many of `boxes` the segment from `a` to `b` passes through. This is the
/// "number of walls" between listener and source; boxes the segment merely
/// touches (enters and leaves at a single endpoint) still count — a doorway
/// exactly on the box face is still behind that wall.
u32 count_occluders_crossed(const Vec3& a, const Vec3& b,
                            const OccluderAabb* boxes, usize count);

/// How open the path is, per wall. Each wall between listener and source
/// multiplies the remaining openness by (1 - kPerWallMuffle):
///   0 walls -> 0.0, 1 -> 0.5, 2 -> 0.75, 3 -> 0.875, 4 -> 0.9375 ...
/// The returned occlusion (1 - openness) is what feeds the low-pass mapping.
inline constexpr f32 kPerWallMuffle = 0.5f;
f32 occlusion_amount(u32 walls);

/// Low-pass cutoff for a given occlusion: `kOcclusionOpenCutoffHz` in open
/// air falling to `kOcclusionClosedCutoffHz` when fully occluded, interpolated
/// exponentially (in log space) so each added wall dims about as much as the
/// last. `occlusion` is clamped to [0, 1] first.
inline constexpr f32 kOcclusionOpenCutoffHz = 20000.0f;
inline constexpr f32 kOcclusionClosedCutoffHz = 350.0f;
f32 occlusion_lowpass_cutoff(f32 occlusion);

/// One-pole low-pass: y[n] = y[n-1] + a * (x[n] - y[n-1]),
///   a = 1 - exp(-2*pi*cutoff / sample_rate).
/// DC passes at exactly 1.0; the step response is the exact closed form
///   y[n] = 1 - (1 - a)^(n+1),
/// which is what tests pin (no FFT or golden file needed).
class LowPassFilter {
public:
    /// Recompute the coefficient for a new cutoff. A non-positive cutoff or a
    /// zero sample rate passes audio unchanged (a = 1) rather than silencing.
    void set_cutoff(f32 cutoff_hz, u32 sample_rate);
    /// Clear the held sample.
    void reset();
    f32 process(f32 input);
    void process_buffer(const f32* input, f32* output, usize frames);

    f32 coefficient() const { return m_a; }
    f32 state() const { return m_state; }

private:
    f32 m_a = 1.0f;
    f32 m_state = 0.0f;
};

} // namespace nf::audio
