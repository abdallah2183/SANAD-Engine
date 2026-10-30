#include <NF/AI/Formation.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace nf::ai {

namespace {

constexpr f32 k_pi = 3.14159265358979323846f;

Vec3 clamp_vector(const Vec3& v, f32 max_len) {
    f32 sq = v.dot(v);
    if (sq > max_len * max_len && sq > 1e-6f) {
        return v * (max_len / std::sqrt(sq));
    }
    return v;
}

} // namespace

// =========================================================================
// FormationGroup
// =========================================================================

void FormationGroup::generate(FormationType type, u32 slot_count, f32 spacing) {
    m_type = type;
    m_spacing = std::max(0.1f, spacing);
    m_slots.clear();
    m_slots.reserve(slot_count);

    if (slot_count == 0) {
        return;
    }

    switch (type) {
        case FormationType::Line: {
            for (u32 i = 0; i < slot_count; ++i) {
                FormationSlot s;
                s.slot_id = i;
                if (i == 0) {
                    s.relative_offset = Vec3{0.0f, 0.0f, 0.0f};
                } else {
                    f32 side = (i % 2 == 1) ? 1.0f : -1.0f;
                    f32 mag = static_cast<f32>((i + 1) / 2);
                    s.relative_offset = Vec3{side * mag * m_spacing, 0.0f, 0.0f};
                }
                m_slots.push_back(s);
            }
            break;
        }
        case FormationType::Column: {
            for (u32 i = 0; i < slot_count; ++i) {
                FormationSlot s;
                s.slot_id = i;
                s.relative_offset = Vec3{0.0f, 0.0f, -static_cast<f32>(i) * m_spacing};
                m_slots.push_back(s);
            }
            break;
        }
        case FormationType::Wedge: {
            for (u32 i = 0; i < slot_count; ++i) {
                FormationSlot s;
                s.slot_id = i;
                if (i == 0) {
                    s.relative_offset = Vec3{0.0f, 0.0f, 0.0f};
                } else {
                    f32 side = (i % 2 == 1) ? 1.0f : -1.0f;
                    f32 rank = static_cast<f32>((i + 1) / 2);
                    s.relative_offset = Vec3{side * rank * m_spacing, 0.0f, -rank * m_spacing};
                }
                m_slots.push_back(s);
            }
            break;
        }
        case FormationType::Box: {
            u32 cols = std::max(1u, static_cast<u32>(std::ceil(std::sqrt(static_cast<f32>(slot_count)))));
            for (u32 i = 0; i < slot_count; ++i) {
                u32 r = i / cols;
                u32 c = i % cols;
                f32 x = (static_cast<f32>(c) - static_cast<f32>(cols - 1) * 0.5f) * m_spacing;
                f32 z = -static_cast<f32>(r) * m_spacing;
                FormationSlot s;
                s.slot_id = i;
                s.relative_offset = Vec3{x, 0.0f, z};
                m_slots.push_back(s);
            }
            break;
        }
        case FormationType::Circle: {
            f32 radius = std::max(m_spacing, (static_cast<f32>(slot_count) * m_spacing) / (2.0f * k_pi));
            f32 angle_step = (2.0f * k_pi) / static_cast<f32>(slot_count);
            for (u32 i = 0; i < slot_count; ++i) {
                f32 a = static_cast<f32>(i) * angle_step;
                FormationSlot s;
                s.slot_id = i;
                s.relative_offset = Vec3{radius * std::sin(a), 0.0f, radius * std::cos(a)};
                m_slots.push_back(s);
            }
            break;
        }
        case FormationType::Custom:
            break;
    }
}

u32 FormationGroup::add_custom_slot(const Vec3& local_offset) {
    m_type = FormationType::Custom;
    FormationSlot s;
    s.slot_id = static_cast<u32>(m_slots.size());
    s.relative_offset = local_offset;
    m_slots.push_back(s);
    return s.slot_id;
}

void FormationGroup::clear_slots() noexcept {
    m_slots.clear();
}

void FormationGroup::set_leader(const Vec3& position, const Vec3& forward) noexcept {
    m_leader_pos = position;
    f32 sq = forward.dot(forward);
    m_leader_fwd = (sq > 1e-4f) ? forward.normalized() : Vec3{0.0f, 0.0f, 1.0f};
}

i32 FormationGroup::assign_agent(u64 agent_id) {
    if (agent_id == 0) {
        return -1;
    }

    // If agent already has a slot, return it
    for (const auto& s : m_slots) {
        if (s.is_occupied && s.assigned_agent_id == agent_id) {
            return static_cast<i32>(s.slot_id);
        }
    }

    // Find first vacant slot
    for (auto& s : m_slots) {
        if (!s.is_occupied) {
            s.is_occupied = true;
            s.assigned_agent_id = agent_id;
            return static_cast<i32>(s.slot_id);
        }
    }

    return -1;
}

i32 FormationGroup::assign_agent_nearest(u64 agent_id, const Vec3& agent_world_pos) {
    if (agent_id == 0 || m_slots.empty()) {
        return -1;
    }

    // If agent already assigned, return current slot
    for (const auto& s : m_slots) {
        if (s.is_occupied && s.assigned_agent_id == agent_id) {
            return static_cast<i32>(s.slot_id);
        }
    }

    i32 best_slot = -1;
    f32 best_dist_sq = std::numeric_limits<f32>::max();

    for (usize i = 0; i < m_slots.size(); ++i) {
        if (!m_slots[i].is_occupied) {
            Vec3 slot_pos, slot_fwd;
            if (get_slot_world_transform(m_slots[i].slot_id, slot_pos, slot_fwd)) {
                Vec3 d = slot_pos - agent_world_pos;
                f32 dist_sq = d.dot(d);
                if (dist_sq < best_dist_sq) {
                    best_dist_sq = dist_sq;
                    best_slot = static_cast<i32>(m_slots[i].slot_id);
                }
            }
        }
    }

    if (best_slot >= 0) {
        for (auto& s : m_slots) {
            if (s.slot_id == static_cast<u32>(best_slot)) {
                s.is_occupied = true;
                s.assigned_agent_id = agent_id;
                return best_slot;
            }
        }
    }

    return -1;
}

bool FormationGroup::unassign_agent(u64 agent_id) {
    if (agent_id == 0) {
        return false;
    }
    for (auto& s : m_slots) {
        if (s.is_occupied && s.assigned_agent_id == agent_id) {
            s.is_occupied = false;
            s.assigned_agent_id = 0;
            return true;
        }
    }
    return false;
}

bool FormationGroup::get_slot_world_transform(u32 slot_id, Vec3& out_world_pos, Vec3& out_world_fwd) const {
    const FormationSlot* found = nullptr;
    for (const auto& s : m_slots) {
        if (s.slot_id == slot_id) {
            found = &s;
            break;
        }
    }
    if (!found) {
        return false;
    }

    Vec3 F = m_leader_fwd;
    Vec3 world_up{0.0f, 1.0f, 0.0f};
    if (std::abs(F.dot(world_up)) > 0.99f) {
        world_up = Vec3{0.0f, 0.0f, 1.0f};
    }

    Vec3 R = world_up.cross(F).normalized();
    Vec3 U = F.cross(R).normalized();

    out_world_pos = m_leader_pos + R * found->relative_offset.x + U * found->relative_offset.y + F * found->relative_offset.z;
    out_world_fwd = F;
    return true;
}

const FormationSlot* FormationGroup::get_agent_slot(u64 agent_id) const {
    if (agent_id == 0) {
        return nullptr;
    }
    for (const auto& s : m_slots) {
        if (s.is_occupied && s.assigned_agent_id == agent_id) {
            return &s;
        }
    }
    return nullptr;
}

usize FormationGroup::occupied_count() const noexcept {
    usize count = 0;
    for (const auto& s : m_slots) {
        if (s.is_occupied) {
            count++;
        }
    }
    return count;
}

bool FormationGroup::is_full() const noexcept {
    return occupied_count() == m_slots.size();
}

// =========================================================================
// FlockingSolver
// =========================================================================

FlockingSolver::FlockingSolver(FlockingConfig config)
    : m_config(std::move(config)) {}

void FlockingSolver::add_agent(u64 id, const Vec3& pos, const Vec3& vel, f32 max_speed, f32 max_force) {
    FlockingAgent a;
    a.id = id;
    a.position = pos;
    a.velocity = vel;
    a.max_speed = std::max(0.1f, max_speed);
    a.max_force = std::max(0.1f, max_force);
    m_agents.push_back(a);
}

bool FlockingSolver::remove_agent(u64 id) {
    for (auto it = m_agents.begin(); it != m_agents.end(); ++it) {
        if (it->id == id) {
            m_agents.erase(it);
            return true;
        }
    }
    return false;
}

void FlockingSolver::clear() noexcept {
    m_agents.clear();
}

FlockingAgent* FlockingSolver::get_agent_by_index(usize index) {
    if (index >= m_agents.size()) {
        return nullptr;
    }
    return &m_agents[index];
}

const FlockingAgent* FlockingSolver::get_agent_by_index(usize index) const {
    if (index >= m_agents.size()) {
        return nullptr;
    }
    return &m_agents[index];
}

FlockingAgent* FlockingSolver::get_agent_by_id(u64 id) {
    for (auto& a : m_agents) {
        if (a.id == id) {
            return &a;
        }
    }
    return nullptr;
}

const FlockingAgent* FlockingSolver::get_agent_by_id(u64 id) const {
    for (const auto& a : m_agents) {
        if (a.id == id) {
            return &a;
        }
    }
    return nullptr;
}

Vec3 FlockingSolver::compute_steering(usize agent_index, const Vec3* optional_goal) const {
    if (agent_index >= m_agents.size()) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }

    const auto& agent = m_agents[agent_index];
    const Vec3& p_i = agent.position;
    const Vec3& v_i = agent.velocity;

    Vec3 sep_accum{0.0f, 0.0f, 0.0f};
    u32 sep_count = 0;

    Vec3 align_accum{0.0f, 0.0f, 0.0f};
    u32 align_count = 0;

    Vec3 coh_accum{0.0f, 0.0f, 0.0f};
    u32 coh_count = 0;

    for (usize j = 0; j < m_agents.size(); ++j) {
        if (j == agent_index) {
            continue;
        }

        const auto& other = m_agents[j];
        Vec3 diff = p_i - other.position;
        f32 dist_sq = diff.dot(diff);

        if (dist_sq < 1e-6f) {
            continue;
        }

        f32 dist = std::sqrt(dist_sq);

        // Separation: push away from nearby flockmates (inversely proportional to distance)
        if (dist < m_config.separation_radius) {
            sep_accum += diff * (1.0f / (dist * dist));
            sep_count++;
        }

        // Alignment: match heading with flockmates within alignment radius
        if (dist < m_config.alignment_radius) {
            align_accum += other.velocity;
            align_count++;
        }

        // Cohesion: gravitate towards centroid of local flock
        if (dist < m_config.cohesion_radius) {
            coh_accum += other.position;
            coh_count++;
        }
    }

    Vec3 sep_force{0.0f, 0.0f, 0.0f};
    if (sep_count > 0 && sep_accum.dot(sep_accum) > 1e-6f) {
        Vec3 desired = sep_accum.normalized() * agent.max_speed;
        sep_force = clamp_vector(desired - v_i, agent.max_force);
    }

    Vec3 align_force{0.0f, 0.0f, 0.0f};
    if (align_count > 0 && align_accum.dot(align_accum) > 1e-6f) {
        Vec3 desired = (align_accum * (1.0f / static_cast<f32>(align_count))).normalized() * agent.max_speed;
        align_force = clamp_vector(desired - v_i, agent.max_force);
    }

    Vec3 coh_force{0.0f, 0.0f, 0.0f};
    if (coh_count > 0) {
        Vec3 centroid = coh_accum * (1.0f / static_cast<f32>(coh_count));
        Vec3 to_centroid = centroid - p_i;
        if (to_centroid.dot(to_centroid) > 1e-6f) {
            Vec3 desired = to_centroid.normalized() * agent.max_speed;
            coh_force = clamp_vector(desired - v_i, agent.max_force);
        }
    }

    Vec3 goal_force{0.0f, 0.0f, 0.0f};
    if (optional_goal) {
        Vec3 to_goal = (*optional_goal) - p_i;
        if (to_goal.dot(to_goal) > 1e-6f) {
            Vec3 desired = to_goal.normalized() * agent.max_speed;
            goal_force = clamp_vector(desired - v_i, agent.max_force);
        }
    }

    Vec3 boundary_force{0.0f, 0.0f, 0.0f};
    if (m_config.boundary_radius > 0.0f) {
        Vec3 to_center = m_config.boundary_center - p_i;
        f32 dist_to_center = std::sqrt(to_center.dot(to_center));
        if (dist_to_center > m_config.boundary_radius && dist_to_center > 1e-4f) {
            Vec3 desired = to_center.normalized() * agent.max_speed;
            f32 penetration = (dist_to_center - m_config.boundary_radius) / m_config.boundary_radius;
            boundary_force = clamp_vector((desired - v_i) * penetration, agent.max_force);
        }
    }

    Vec3 total_steer = sep_force * m_config.separation_weight +
                       align_force * m_config.alignment_weight +
                       coh_force * m_config.cohesion_weight +
                       goal_force * m_config.goal_weight +
                       boundary_force * m_config.boundary_weight;

    return clamp_vector(total_steer, agent.max_force);
}

void FlockingSolver::step(f32 dt, const Vec3* optional_goal) {
    if (dt <= 0.0f || m_agents.empty()) {
        return;
    }

    std::vector<Vec3> steerings(m_agents.size());
    for (usize i = 0; i < m_agents.size(); ++i) {
        steerings[i] = compute_steering(i, optional_goal);
    }

    for (usize i = 0; i < m_agents.size(); ++i) {
        auto& a = m_agents[i];
        a.velocity += steerings[i] * dt;
        a.velocity = clamp_vector(a.velocity, a.max_speed);
        a.position += a.velocity * dt;
    }
}

} // namespace nf::ai
