#include <NF/Physics/CableSystem.hpp>

#include <algorithm>
#include <cmath>

namespace nf::physics {

CableSimulation::CableSimulation()
    : CableSimulation(CableConfig{}) {}

CableSimulation::CableSimulation(const CableConfig& config)
    : m_config(config) {
    Vec3 start{0.0f, 5.0f, 0.0f};
    Vec3 end{10.0f, 5.0f, 0.0f};
    initialize_endpoints(start, end);
}

void CableSimulation::set_config(const CableConfig& config) {
    m_config = config;
    if (!m_particles.empty()) {
        Vec3 start = m_particles.front().position;
        Vec3 end = m_particles.back().position;
        initialize_endpoints(start, end);
    }
}

void CableSimulation::initialize_endpoints(const Vec3& start_point, const Vec3& end_point) {
    u32 count = std::max(2u, m_config.particle_count);
    m_particles.resize(count);

    f32 total_mass = std::max(0.01f, m_config.total_length * m_config.linear_density);
    f32 particle_mass = total_mass / static_cast<f32>(count);
    f32 inv_m = 1.0f / particle_mass;

    for (u32 i = 0; i < count; ++i) {
        f32 alpha = static_cast<f32>(i) / static_cast<f32>(count - 1);
        Vec3 pos = start_point + (end_point - start_point) * alpha;
        m_particles[i].position = pos;
        m_particles[i].old_position = pos;
        m_particles[i].velocity = Vec3{0.0f, 0.0f, 0.0f};
        m_particles[i].inv_mass = inv_m;
    }

    // Default: both ends pinned
    m_particles.front().inv_mass = 0.0f;
    m_particles.back().inv_mass = 0.0f;

    // Create segments
    u32 seg_count = count - 1;
    m_constraints.resize(seg_count);
    f32 seg_rest_len = m_config.total_length / static_cast<f32>(seg_count);

    for (u32 i = 0; i < seg_count; ++i) {
        m_constraints[i].p0 = i;
        m_constraints[i].p1 = i + 1;
        m_constraints[i].rest_length = seg_rest_len;
        m_constraints[i].current_tension = 0.0f;
        m_constraints[i].active = true;
    }

    m_broken = false;
}

void CableSimulation::pin_particle(u32 index, const Vec3& pos) {
    if (index < m_particles.size()) {
        m_particles[index].position = pos;
        m_particles[index].old_position = pos;
        m_particles[index].inv_mass = 0.0f;
    }
}

void CableSimulation::unpin_particle(u32 index) {
    if (index < m_particles.size() && m_particles.size() > 0) {
        f32 total_mass = std::max(0.01f, m_config.total_length * m_config.linear_density);
        f32 particle_mass = total_mass / static_cast<f32>(m_particles.size());
        m_particles[index].inv_mass = 1.0f / particle_mass;
    }
}

void CableSimulation::move_pinned_particle(u32 index, const Vec3& new_pos) {
    if (index < m_particles.size()) {
        m_particles[index].position = new_pos;
        m_particles[index].old_position = new_pos;
    }
}

void CableSimulation::set_start_point(const Vec3& pos, bool pinned) {
    if (!m_particles.empty()) {
        if (pinned) {
            pin_particle(0, pos);
        } else {
            m_particles.front().position = pos;
            unpin_particle(0);
        }
    }
}

void CableSimulation::set_end_point(const Vec3& pos, bool pinned) {
    if (!m_particles.empty()) {
        u32 last_idx = static_cast<u32>(m_particles.size() - 1);
        if (pinned) {
            pin_particle(last_idx, pos);
        } else {
            m_particles.back().position = pos;
            unpin_particle(last_idx);
        }
    }
}

void CableSimulation::add_sphere_obstacle(const Vec3& center, f32 radius) {
    m_sphere_obstacles.push_back(SphereObstacle{center, radius});
}

void CableSimulation::step(f32 dt) {
    if (dt <= 0.0f || m_particles.empty()) {
        return;
    }

    u32 substeps = std::max(1u, m_config.substeps);
    f32 sub_dt = dt / static_cast<f32>(substeps);

    for (u32 s = 0; s < substeps; ++s) {
        solve_substep(sub_dt);
    }
}

void CableSimulation::solve_substep(f32 sub_dt) {
    f32 damping_factor = std::clamp(1.0f - m_config.damping * sub_dt, 0.0f, 1.0f);
    Vec3 grav_step = m_config.gravity * (sub_dt * sub_dt);

    // Verlet integration
    for (auto& p : m_particles) {
        if (p.inv_mass <= 0.0f) {
            continue;
        }

        Vec3 vel = (p.position - p.old_position) * damping_factor;
        p.old_position = p.position;
        p.position = p.position + vel + grav_step;
    }

    // Distance constraints relaxation
    solve_constraints(sub_dt);

    // Collision projection
    solve_collisions();
}

void CableSimulation::solve_constraints(f32 sub_dt) {
    u32 iters = std::max(1u, m_config.solver_iterations);

    for (u32 iter = 0; iter < iters; ++iter) {
        for (auto& c : m_constraints) {
            if (!c.active) {
                continue;
            }

            CableParticle& p0 = m_particles[c.p0];
            CableParticle& p1 = m_particles[c.p1];

            Vec3 delta = p1.position - p0.position;
            f32 dist = delta.length();
            if (dist < 1e-5f) {
                continue;
            }

            f32 diff = (dist - c.rest_length) / dist;
            f32 sum_inv = p0.inv_mass + p1.inv_mass;
            if (sum_inv < 1e-6f) {
                continue;
            }

            f32 w0 = p0.inv_mass / sum_inv;
            f32 w1 = p1.inv_mass / sum_inv;

            Vec3 correction = delta * (diff * m_config.stiffness);

            if (p0.inv_mass > 0.0f) {
                p0.position = p0.position + correction * w0;
            }
            if (p1.inv_mass > 0.0f) {
                p1.position = p1.position - correction * w1;
            }

            // Estimate tension on final iteration
            if (iter + 1 == iters) {
                f32 stretch = std::max(0.0f, dist - c.rest_length);
                f32 mass = (p0.inv_mass > 0.0f ? 1.0f / p0.inv_mass : 1.0f);
                c.current_tension = (stretch * mass) / (sub_dt * sub_dt + 1e-6f);

                if (m_config.breakable && c.current_tension > m_config.max_tension) {
                    c.active = false;
                    m_broken = true;
                }
            }
        }
    }
}

void CableSimulation::solve_collisions() {
    for (auto& p : m_particles) {
        if (p.inv_mass <= 0.0f) {
            continue;
        }

        // Ground plane collision
        if (m_ground_collision && p.position.y < m_ground_y) {
            p.position.y = m_ground_y;
            p.old_position.y = m_ground_y; // Zero vertical bounce
        }

        // Sphere obstacles
        for (const auto& sph : m_sphere_obstacles) {
            Vec3 diff = p.position - sph.center;
            f32 d_sq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
            f32 r_sq = sph.radius * sph.radius;

            if (d_sq < r_sq) {
                f32 d = std::sqrt(std::max(1e-6f, d_sq));
                Vec3 normal = diff * (1.0f / d);
                p.position = sph.center + normal * sph.radius;
            }
        }
    }
}

f32 CableSimulation::start_tension() const noexcept {
    if (m_constraints.empty() || !m_constraints.front().active) {
        return 0.0f;
    }
    return m_constraints.front().current_tension;
}

f32 CableSimulation::end_tension() const noexcept {
    if (m_constraints.empty() || !m_constraints.back().active) {
        return 0.0f;
    }
    return m_constraints.back().current_tension;
}

f32 CableSimulation::max_current_tension() const noexcept {
    f32 max_t = 0.0f;
    for (const auto& c : m_constraints) {
        if (c.active && c.current_tension > max_t) {
            max_t = c.current_tension;
        }
    }
    return max_t;
}

std::vector<Vec3> CableSimulation::get_positions() const {
    std::vector<Vec3> positions;
    positions.reserve(m_particles.size());
    for (const auto& p : m_particles) {
        positions.push_back(p.position);
    }
    return positions;
}

Vec3 CableSimulation::sample_position(f32 t) const {
    if (m_particles.empty()) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }
    if (m_particles.size() == 1 || t <= 0.0f) {
        return m_particles.front().position;
    }
    if (t >= 1.0f) {
        return m_particles.back().position;
    }

    f32 scaled = t * static_cast<f32>(m_particles.size() - 1);
    u32 idx = static_cast<u32>(scaled);
    f32 frac = scaled - static_cast<f32>(idx);

    if (idx + 1 >= m_particles.size()) {
        return m_particles.back().position;
    }

    const Vec3& a = m_particles[idx].position;
    const Vec3& b = m_particles[idx + 1].position;
    return Vec3{
        a.x + (b.x - a.x) * frac,
        a.y + (b.y - a.y) * frac,
        a.z + (b.z - a.z) * frac
    };
}

} // namespace nf::physics
