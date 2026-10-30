#include <NF/Water/RiverFlow.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace nf::water {

// =========================================================================
// RiverCorridor
// =========================================================================

RiverCorridor::RiverCorridor(std::string name)
    : m_name(std::move(name)) {}

void RiverCorridor::add_waypoint(const RiverWaypoint& wp) {
    m_waypoints.push_back(wp);
    update_spline_cache();
}

void RiverCorridor::clear() noexcept {
    m_waypoints.clear();
    m_segment_lengths.clear();
    m_total_length = 0.0f;
}

const RiverWaypoint* RiverCorridor::waypoint(usize index) const {
    if (index >= m_waypoints.size()) {
        return nullptr;
    }
    return &m_waypoints[index];
}

void RiverCorridor::update_spline_cache() noexcept {
    m_segment_lengths.clear();
    m_total_length = 0.0f;

    if (m_waypoints.size() < 2) {
        return;
    }

    m_segment_lengths.reserve(m_waypoints.size() - 1);
    for (usize i = 0; i < m_waypoints.size() - 1; ++i) {
        Vec3 d = m_waypoints[i + 1].position - m_waypoints[i].position;
        f32 len = d.length();
        m_segment_lengths.push_back(len);
        m_total_length += len;
    }
}

RiverSample RiverCorridor::sample(const Vec3& world_pos) const {
    RiverSample res;
    if (m_waypoints.size() < 2) {
        return res;
    }

    usize best_seg = 0;
    f32 best_t = 0.0f;
    f32 min_dist_sq = std::numeric_limits<f32>::max();
    Vec3 best_c{0.0f, 0.0f, 0.0f};
    Vec3 best_T{0.0f, 0.0f, 1.0f};

    for (usize i = 0; i < m_waypoints.size() - 1; ++i) {
        f32 L = m_segment_lengths[i];
        if (L < 1e-4f) {
            continue;
        }

        const Vec3& A = m_waypoints[i].position;
        const Vec3& B = m_waypoints[i + 1].position;
        Vec3 V = B - A;
        Vec3 T = V * (1.0f / L);

        Vec3 AP = world_pos - A;
        f32 t = std::clamp(AP.dot(V) / (L * L), 0.0f, 1.0f);
        Vec3 C = A + V * t;

        Vec3 diff = world_pos - C;
        f32 dist_sq = diff.dot(diff);

        if (dist_sq < min_dist_sq) {
            min_dist_sq = dist_sq;
            best_seg = i;
            best_t = t;
            best_c = C;
            best_T = T;
        }
    }

    const auto& wpA = m_waypoints[best_seg];
    const auto& wpB = m_waypoints[best_seg + 1];

    f32 width = (1.0f - best_t) * wpA.width + best_t * wpB.width;
    f32 depth = (1.0f - best_t) * wpA.depth + best_t * wpB.depth;
    f32 flow_speed = (1.0f - best_t) * wpA.flow_speed + best_t * wpB.flow_speed;

    Vec3 world_up{0.0f, 1.0f, 0.0f};
    if (std::abs(best_T.dot(world_up)) > 0.99f) {
        world_up = Vec3{0.0f, 0.0f, 1.0f};
    }
    Vec3 R = world_up.cross(best_T).normalized();

    Vec3 CP = world_pos - best_c;
    f32 lat_dist = CP.dot(R);
    f32 half_w = width * 0.5f;

    if (std::abs(lat_dist) <= half_w) {
        res.in_river = true;
        res.normalized_cross = std::clamp(lat_dist / half_w, -1.0f, 1.0f);

        res.surface_point = best_c + R * lat_dist;
        res.water_surface_y = res.surface_point.y;

        f32 cross_sq = res.normalized_cross * res.normalized_cross;
        res.depth = depth * (1.0f - cross_sq);
        res.riverbed_y = res.water_surface_y - res.depth;

        f32 speed_factor = 1.0f - 0.35f * cross_sq;
        res.flow_velocity = best_T * (flow_speed * speed_factor);

        f32 edge_dist = std::abs(res.normalized_cross);
        res.bank_foam_factor = (edge_dist > 0.6f) ? std::clamp((edge_dist - 0.6f) / 0.4f, 0.0f, 1.0f) : 0.0f;
    }

    return res;
}

// =========================================================================
// RiverFlowSystem
// =========================================================================

usize RiverFlowSystem::add_river(RiverCorridor river) {
    m_rivers.push_back(std::move(river));
    return m_rivers.size() - 1;
}

void RiverFlowSystem::clear() noexcept {
    m_rivers.clear();
}

RiverCorridor* RiverFlowSystem::get_river(usize index) {
    if (index >= m_rivers.size()) {
        return nullptr;
    }
    return &m_rivers[index];
}

const RiverCorridor* RiverFlowSystem::get_river(usize index) const {
    if (index >= m_rivers.size()) {
        return nullptr;
    }
    return &m_rivers[index];
}

RiverSample RiverFlowSystem::sample(const Vec3& world_pos) const {
    RiverSample best_sample;
    for (const auto& river : m_rivers) {
        RiverSample s = river.sample(world_pos);
        if (s.in_river) {
            return s; // Inside river channel
        }
    }
    return best_sample;
}

void RiverFlowSystem::apply_drift(
    Vec3& object_pos,
    Vec3& object_vel,
    f32 buoyancy,
    f32 water_drag,
    f32 dt,
    f32 gravity
) const {
    if (dt <= 0.0f) {
        return;
    }

    RiverSample s = sample(object_pos);

    if (!s.in_river) {
        // Free fall under gravity outside river
        object_vel.y -= gravity * dt;
        object_pos += object_vel * dt;
        return;
    }

    f32 immersion = s.water_surface_y - object_pos.y;
    if (immersion > 0.0f) {
        // Submerged: buoyancy pushes upward
        f32 submersion_ratio = std::clamp(immersion / 0.5f, 0.0f, 2.0f);
        f32 upward_accel = buoyancy * submersion_ratio - gravity;
        object_vel.y += upward_accel * dt;
        // Hydrodynamic vertical drag damping
        object_vel.y *= std::exp(-water_drag * dt);
    } else {
        // Above water: freefall downward
        object_vel.y -= gravity * dt;
    }

    // Horizontal current drag accelerating object towards flow velocity
    f32 drag_factor = 1.0f - std::exp(-water_drag * dt);
    object_vel.x += (s.flow_velocity.x - object_vel.x) * drag_factor;
    object_vel.z += (s.flow_velocity.z - object_vel.z) * drag_factor;

    object_pos += object_vel * dt;
}

} // namespace nf::water
