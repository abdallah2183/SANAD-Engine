#pragma once

// NF/Scene2D/Particles2D.hpp — deterministic 2D particle emitter and simulator.
// Design doc Section 57 (Particles): GPU/CPU particles, and Section 114
// (Determinism).
//
// Determinism is the whole point of this implementation, not a property it
// happens to have: there is no RNG, no `rand()`, no `std::mt19937`, no seeded
// state. Every per-particle initial value (direction, speed, lifetime, size,
// colour, spin) is `hash_to_unit(emit_counter + salt)`, where `emit_counter` is
// a monotonically increasing integer and `salt` is a per-burst offset. The same
// emitter configuration at the same world position, fed the same time steps,
// reproduces a particle cloud bit for bit — which is what a replay, a
// network-validated hit check, and a recorded demo all need.
//
// Simulation is CPU-side into a fixed-capacity ring. Section 57 calls for the
// particle budget to be honoured exactly: `max_particles` is an allocation, not
// a target, and when the ring is full the oldest particle is evicted rather
// than the burst being dropped or the buffer grown.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include "Math2D.hpp"
#include "SpriteBatcher.hpp"

#include <vector>

namespace nf::scene2d {

/// One particle's simulation state. Kept to 48 bytes so a pool of 8192 fits in
/// ~400 KB — the whole system is written to stay in L2 while it integrates.
struct Particle2D {
    Vec2 position{0.0f, 0.0f};
    Vec2 velocity{0.0f, 0.0f};
    /// Remaining life, seconds. When it hits zero the slot is free.
    f32 life = 0.0f;
    f32 age = 0.0f;          ///< Elapsed, for curve lookups.
    f32 initial_life = 0.0f;  ///< life at spawn, normalised into curves.
    f32 size = 1.0f;          ///< Current half-extent in world units.
    f32 initial_size = 1.0f;
    /// Degrees per second, clockwise in y-down space.
    f32 spin = 0.0f;
    f32 rotation_deg = 0.0f;
    u32 color = 0xFFFFFFFFu;  ///< Packed RGBA8 (see pack_color).
    f32 drag = 0.0f;          ///< Velocity damping per second.
    f32 gravity_scale = 1.0f;
};

/// Emitter configuration — pure data, so a gameplay script or a saved scene can
/// hold one by value and the same struct feeds both the editor's preview and
/// the runtime simulation.
struct ParticleEmitter2D {
    /// Particles per second. Fractional accumulation means 7.5/s emits 15 in
    /// two seconds, not 14, without a frame-rate-dependent rounding bias.
    f32 rate = 10.0f;
    /// One-shot burst added on `burst()`, or zero for a continuous stream.
    u32 burst_count = 0;

    /// Cone the initial direction is scattered into, degrees. 0 = all one way,
    /// 360 = full circle. Scatter is `hash_to_unit`-based, so the cone shape is
    /// reproducible.
    f32 spread_deg = 360.0f;
    /// Centre of the cone, degrees clockwise from +x in y-down space. 90 means
    /// "downward" — the natural default for gravity-driven effects.
    f32 direction_deg = 90.0f;
    f32 speed_min = 1.0f;
    f32 speed_max = 3.0f;
    f32 life_min = 0.5f;
    f32 life_max = 1.5f;
    f32 size_start = 0.5f;
    f32 size_end = 0.1f;
    f32 spin_min = -90.0f;
    f32 spin_max = 90.0f;
    /// Colour over life: start and end packed RGBA8, lerped per frame.
    u32 color_start = 0xFFFFFFFFu;
    u32 color_end = 0x00000000u;
    /// Air resistance, applied as velocity *= exp(-drag*dt): the
    /// frame-rate-independent form (a naive `1 - drag*dt` can invert the
    /// velocity on a long step).
    f32 drag = 0.5f;
    f32 gravity_scale = 1.0f;

    /// Local-space emit area shape. A point emitter has both radii zero.
    f32 emit_radius_x = 0.0f;
    f32 emit_radius_y = 0.0f;

    /// Atlas page and UVs for every emitted particle. Kept on the emitter
    /// because a burst shares one look; per-particle variation comes from the
    /// colour and size curves, not from texture swapping mid-burst.
    u32 page = 0;
    f32 u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    /// Rendered behind (negative) or in front of (positive) gameplay sprites.
    f32 depth = 0.0f;
    f32 parallax = 1.0f;
};

class Particles2D {
public:
    Particles2D() = default;

    /// Hard capacity. Allocates once; `emit` never reallocates mid-frame.
    void resize(u32 max_particles) {
        m_pool.resize(static_cast<usize>(max_particles));
        clear();
    }

    usize capacity() const { return m_pool.size(); }
    usize alive() const { return static_cast<usize>(m_alive); }

    /// Removes every live particle without freeing the pool.
    void clear() {
        m_alive = 0u;
        m_written = 0u;
        m_head = 0u;
        m_emit_acc = 0.0f;
    }

    void set_emitter(const ParticleEmitter2D& e) { m_emitter = e; }
    const ParticleEmitter2D& emitter() const { return m_emitter; }
    ParticleEmitter2D& emitter() { return m_emitter; }

    /// Emits exactly `count` particles at `origin` immediately. Fractional
    /// carry from `update` is bypassed, which is what an explosion wants: it
    /// fires its full count on the frame the event happens, not spread over the
    /// next second according to the frame rate.
    void burst(Vec2 origin, u32 count);

    /// Advances the simulation by `dt` and emits according to `rate`. Call from
    /// the fixed-timestep loop, not the render loop: particles integrating at
    /// frame rate would drift between machines of different speed.
    void update(Vec2 origin, f32 dt, Vec2 gravity = Vec2{0.0f, 0.0f});

    /// Appends one `SpriteDraw` per live particle into `out`, for the batcher.
    /// Keeping collection separate from simulation means the same particle pool
    /// can be drawn into several views (main camera, minimap, shadow pass)
    /// without re-simulating.
    void collect_draws(std::vector<SpriteDraw>& out) const;

    /// Direct pool access for debug draw and tests.
    const std::vector<Particle2D>& pool() const { return m_pool; }

private:
    /// Spawns one particle, hashing its initial state from the emit counter.
    /// Determinism holds across `resize` and across runs because the counter is
    /// monotonic for the emitter's lifetime, never reset by a pool change.
    void emit_one(Vec2 origin);

    std::vector<Particle2D> m_pool;
    ParticleEmitter2D m_emitter{};
    /// Ring write cursor: the next never-written slot, or when the pool is full
    /// the oldest particle (the ring wraps and evicts).
    u32 m_head = 0u;
    /// Slots that have been written at least once. Particles in [0, m_written)
    /// may be dead, but no slot beyond it is ever read.
    u32 m_written = 0u;
    u32 m_alive = 0u;
    /// Fractional emission accumulator for `rate`.
    f32 m_emit_acc = 0.0f;
    /// Monotonic spawn index — the hash key that makes the whole system
    /// reproducible. Wraps at 2^32, which restarts the hash pattern but only
    /// after ~4 billion particles.
    u32 m_emit_counter = 0u;
};

} // namespace nf::scene2d
