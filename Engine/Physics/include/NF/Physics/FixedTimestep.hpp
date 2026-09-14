#pragma once

#include <NF/Core/Types.hpp>

namespace nf::physics {

/// Fixed-timestep accumulator.
///
/// The point is not smoothness, it is that the same inputs produce the same
/// state regardless of frame rate. A frame that takes 200 ms does not produce a
/// 200 ms physics step — it produces N fixed steps, and the remainder is carried
/// into the next frame. That is what makes the simulation reproducible, and
/// therefore testable and (eventually) replayable and networkable.
class FixedTimestep {
public:
    explicit FixedTimestep(f32 step = 1.0f / 60.0f, u32 max_substeps = 8)
        : m_step(step > 1e-5f ? step : 1e-5f), m_max_substeps(max_substeps ? max_substeps : 1) {}

    /// Add elapsed time; returns how many fixed steps to run this frame.
    ///
    /// Never returns more than max_substeps. A long stall (a breakpoint, a
    /// window drag) would otherwise queue up hundreds of steps and the
    /// simulation would appear to fast-forward; the surplus is dropped and
    /// counted instead.
    u32 advance(f32 frame_delta) {
        // `!(x > 0)` rather than `x <= 0` so a NaN delta is rejected too.
        if (!(frame_delta > 0.0f)) {
            return 0;
        }
        const f32 budget = m_step * static_cast<f32>(m_max_substeps);
        if (frame_delta > budget) {
            m_dropped += static_cast<u32>((frame_delta - budget) / m_step);
            frame_delta = budget;
        }

        m_accumulator += frame_delta;
        u32 steps = 0;
        while (m_accumulator >= m_step) {
            m_accumulator -= m_step;
            ++steps;
        }
        return steps;
    }

    /// Where the current time sits between the last two fixed states. For
    /// rendering only; the simulation never reads it.
    f32 alpha() const { return m_accumulator / m_step; }

    f32 step() const { return m_step; }

    void set_step(f32 step) {
        m_step = (step > 1e-5f) ? step : 1e-5f;
    }
    void set_max_substeps(u32 n) { m_max_substeps = (n != 0) ? n : 1; }

    /// Steps discarded because a frame exceeded the substep budget.
    u32 dropped_steps() const { return m_dropped; }

    void reset() {
        m_accumulator = 0.0f;
        m_dropped = 0;
    }

private:
    f32 m_step;
    f32 m_accumulator = 0.0f;
    u32 m_max_substeps;
    u32 m_dropped = 0;
};

} // namespace nf::physics
