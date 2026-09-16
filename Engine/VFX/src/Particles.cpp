// NF/Vfx/Particles.cpp — deterministic CPU particle simulation.

#include <NF/Vfx/Particles.hpp>

#include <algorithm>
#include <cstdint>

namespace nf::vfx {

ParticleSystem::ParticleSystem(EmitterConfig config) : m_config(config) {
    m_particles.reserve(config.max_particles > 0 ? config.max_particles : 1);
}

float ParticleSystem::rand01() {
    // SplitMix64-style avalanche on the emission counter: stateless,
    // platform-independent, and unique per emitted particle.
    u64 z = m_emit_counter++ + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<float>((z >> 11) * (1.0 / 9007199254740992.0));
}

void ParticleSystem::burst(u32 count) {
    for (u32 i = 0; i < count; ++i) {
        if (m_particles.size() >= m_config.max_particles || m_config.max_particles == 0) return;
        Particle p;
        p.position = m_config.origin;
        const float spread = 2.0f;
        p.velocity = Vec3{m_config.velocity.x + (rand01() - 0.5f) * spread * m_config.velocity_spread.x,
                          m_config.velocity.y + (rand01() - 0.5f) * spread * m_config.velocity_spread.y,
                          m_config.velocity.z + (rand01() - 0.5f) * spread * m_config.velocity_spread.z};
        const float lt_spread = 1.0f + (rand01() - 0.5f) * 2.0f * m_config.lifetime_spread;
        p.max_life = m_config.lifetime * (lt_spread > 0.05f ? lt_spread : 0.05f);
        p.life = p.max_life;
        p.size = m_config.start_size;
        p.color = m_config.start_color;
        m_particles.push_back(p);
    }
}

void ParticleSystem::update(float dt) {
    if (!(dt > 0.0f)) return;
    // Emission by rate.
    if (m_config.rate > 0.0f && m_config.max_particles > 0) {
        m_emit_accumulator += m_config.rate * dt;
        u32 n = static_cast<u32>(m_emit_accumulator);
        m_emit_accumulator -= static_cast<float>(n);
        const usize room = m_config.max_particles > m_particles.size()
                               ? m_config.max_particles - m_particles.size()
                               : 0;
        if (n > room) {
            // Overflow is dropped (and the accumulator keeps only the
            // fractional part): a full emitter sheds load, never memory.
            n = static_cast<u32>(room);
        }
        burst(n);
    }
    // Integrate (semi-implicit Euler), grade, kill by swap-remove.
    for (usize i = 0; i < m_particles.size();) {
        Particle& p = m_particles[i];
        p.life -= dt;
        if (p.life <= 0.0f) {
            p = m_particles.back();
            m_particles.pop_back();
            continue;
        }
        p.velocity = p.velocity + m_config.gravity * dt;
        if (m_config.drag > 0.0f) {
            const float keep = 1.0f - std::min(m_config.drag * dt, 0.95f);
            p.velocity = p.velocity * keep;
        }
        p.position = p.position + p.velocity * dt;
        const float t = p.age_fraction();
        p.size = m_config.start_size + (m_config.end_size - m_config.start_size) * t;
        p.color = Vec3{m_config.start_color.x + (m_config.end_color.x - m_config.start_color.x) * t,
                       m_config.start_color.y + (m_config.end_color.y - m_config.start_color.y) * t,
                       m_config.start_color.z + (m_config.end_color.z - m_config.start_color.z) * t};
        ++i;
    }
}

void ParticleSystem::clear() {
    m_particles.clear();
    m_emit_accumulator = 0.0f;
}

} // namespace nf::vfx
