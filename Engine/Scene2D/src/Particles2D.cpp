// NF/Scene2D/Particles2D.cpp — deterministic 2D particle simulation.
// Design doc Section 57 (Particles), Section 114 (Determinism).
//
// Every per-particle value below is a hash of a monotonic counter (Section
// 114). There is no RNG to seed, so there is nothing to get out of sync: the
// same emitter at the same position, stepped with the same dt sequence, emits
// the same cloud on every machine and in every replay.

#include <NF/Scene2D/Particles2D.hpp>

#include <cmath>

namespace nf::scene2d {

namespace {

/// Hash key for field `field` of particle `index`. The multiply by an odd
/// Knuth-ish constant separates consecutive counters far more than an add
/// would, so two particles emitted back to back do not land in the same corner
/// of parameter space.
inline f32 field(u32 index, u32 field_id) {
    return hash_u32_to_unit(index * 2654435761u + field_id * 40503u);
}

/// Unpacks one RGBA8 channel. `pack_color` stores R in the low byte.
inline u8 channel(u32 c, u32 shift) {
    return static_cast<u8>((c >> shift) & 0xFFu);
}

/// Lerps two packed RGBA8 colours by `t` in [0,1]. Channels are interpolated in
/// 8-bit space rather than float to keep the result identical on every target —
/// a float round-trip can differ by one ULP per platform.
u32 lerp_color(u32 a, u32 b, f32 t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    const u32 ta = static_cast<u32>(t * 256.0f); // [1,255], biased toward b
    const u32 ia = 256u - ta;
    u32 result = 0u;
    for (u32 shift = 0u; shift < 32u; shift += 8u) {
        const u32 ca = channel(a, shift);
        const u32 cb = channel(b, shift);
        result |= static_cast<u32>((ca * ia + cb * ta) >> 8) << shift;
    }
    return result;
}

} // namespace

void Particles2D::burst(Vec2 origin, u32 count) {
    for (u32 i = 0u; i < count; ++i) emit_one(origin);
}

void Particles2D::emit_one(Vec2 origin) {
    if (m_pool.empty()) return;

    const u32 slot_index = m_head;
    Particle2D& p = m_pool[slot_index];
    // A live particle being overwritten is an eviction, not a spawn: the alive
    // count is unchanged. A dead (or never-written) slot is a real addition.
    if (p.life <= 0.0f) ++m_alive;

    const u32 id = m_emit_counter++;
    const ParticleEmitter2D& e = m_emitter;

    // Direction: the cone is centred on `direction_deg` and `spread_deg` wide.
    // A full spread uses the whole circle, which is why the default is 360.
    const f32 angle_deg = e.direction_deg +
                          (field(id, 0u) - 0.5f) * e.spread_deg;
    const f32 rad = angle_deg * DEG_TO_RAD;

    const f32 speed = e.speed_min + field(id, 1u) * (e.speed_max - e.speed_min);
    const f32 life = e.life_min + field(id, 2u) * (e.life_max - e.life_min);
    const f32 spin = e.spin_min + field(id, 3u) * (e.spin_max - e.spin_min);
    const f32 ox = (field(id, 4u) - 0.5f) * 2.0f * e.emit_radius_x;
    const f32 oy = (field(id, 5u) - 0.5f) * 2.0f * e.emit_radius_y;

    p.position = origin + Vec2{ox, oy};
    p.velocity = Vec2{std::cos(rad), std::sin(rad)} * speed;
    p.life = life;
    p.age = 0.0f;
    p.initial_life = life;
    p.size = e.size_start;
    p.initial_size = e.size_start;
    p.spin = spin;
    p.rotation_deg = 0.0f;
    p.color = e.color_start;
    p.drag = e.drag;
    p.gravity_scale = e.gravity_scale;

    m_head = (m_head + 1u) % static_cast<u32>(m_pool.size());
    if (m_written < static_cast<u32>(m_pool.size())) ++m_written;
}

void Particles2D::update(Vec2 origin, f32 dt, Vec2 gravity) {
    if (dt <= 0.0f) return;
    if (m_pool.empty()) return;

    // Continuous emission. The fractional part carries over so 7.5/s emits 15
    // in two seconds, and emission is clamped to the pool per call so a huge dt
    // cannot spin this loop for a frame's worth of time.
    const u32 cap = static_cast<u32>(m_pool.size());
    m_emit_acc += m_emitter.rate * dt;
    u32 to_emit = m_emit_acc >= static_cast<f32>(cap) ? cap
                                                      : static_cast<u32>(m_emit_acc);
    m_emit_acc -= static_cast<f32>(to_emit);
    for (u32 i = 0u; i < to_emit; ++i) emit_one(origin);

    for (u32 i = 0u; i < m_written; ++i) {
        Particle2D& p = m_pool[i];
        if (p.life <= 0.0f) continue;

        p.age += dt;
        p.life -= dt;
        if (p.life <= 0.0f) {
            p.life = 0.0f;
            --m_alive;
            continue;
        }

        // exp(): frame-rate-independent damping. `1 - drag*dt` overshoots to a
        // negative velocity on a step longer than 1/drag.
        if (p.drag > 0.0f) {
            const f32 d = std::exp(-p.drag * dt);
            p.velocity = p.velocity * d;
        }
        p.velocity = p.velocity + gravity * (p.gravity_scale * dt);
        p.position = p.position + p.velocity * dt;
        p.rotation_deg += p.spin * dt;

        // Curves. Size and colour follow life, so a particle shrinks and fades
        // out rather than popping away.
        const f32 t = p.initial_life > EPSILON
                          ? clamp(p.age / p.initial_life, 0.0f, 1.0f)
                          : 0.0f;
        p.size = p.initial_size + (m_emitter.size_end - m_emitter.size_start) * t;
        p.color = lerp_color(m_emitter.color_start, m_emitter.color_end, t);
    }
}

void Particles2D::collect_draws(std::vector<SpriteDraw>& out) const {
    const ParticleEmitter2D& e = m_emitter;
    for (u32 i = 0u; i < m_written; ++i) {
        const Particle2D& p = m_pool[i];
        if (p.life <= 0.0f) continue;

        SpriteDraw d;
        d.page = e.page;
        d.u0 = e.u0;
        d.v0 = e.v0;
        d.u1 = e.u1;
        d.v1 = e.v1;
        d.position = p.position;
        // Size is a half-extent internally; SpriteDraw wants width/height.
        d.size = Vec2{p.size * 2.0f, p.size * 2.0f};
        d.rotation_deg = p.rotation_deg;
        d.anchor = Vec2{0.5f, 0.5f};
        d.color = p.color;
        d.depth = e.depth;
        d.parallax = e.parallax;
        out.push_back(d);
    }
}

} // namespace nf::scene2d
