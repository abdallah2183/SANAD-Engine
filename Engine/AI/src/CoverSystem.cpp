#include <NF/AI/CoverSystem.hpp>

#include <algorithm>
#include <cmath>

namespace nf::ai {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kDegToRad = kPi / 180.0f;

} // namespace

u32 CoverSystem::register_cover_point(CoverPoint point) {
    if (point.id == 0) {
        point.id = m_next_id++;
    } else {
        m_next_id = std::max(m_next_id, point.id + 1);
        // If an existing point has this ID, replace it
        for (auto& existing : m_cover_points) {
            if (existing.id == point.id) {
                existing = std::move(point);
                return existing.id;
            }
        }
    }

    // Ensure normal is normalized
    if (point.normal.length_sq() > 1e-6f) {
        point.normal = point.normal.normalized();
    } else {
        point.normal = Vec3{0.0f, 0.0f, 1.0f};
    }

    const u32 id = point.id;
    m_cover_points.push_back(std::move(point));
    return id;
}

bool CoverSystem::unregister_cover_point(u32 id) {
    auto it = std::remove_if(m_cover_points.begin(), m_cover_points.end(), [id](const CoverPoint& pt) {
        return pt.id == id;
    });
    if (it != m_cover_points.end()) {
        m_cover_points.erase(it, m_cover_points.end());
        return true;
    }
    return false;
}

const CoverPoint* CoverSystem::get_cover_point(u32 id) const {
    for (const auto& pt : m_cover_points) {
        if (pt.id == id) {
            return &pt;
        }
    }
    return nullptr;
}

bool CoverSystem::claim_cover(u32 cover_id, u32 agent_id) {
    if (agent_id == 0) {
        return release_cover(cover_id);
    }

    for (auto& pt : m_cover_points) {
        if (pt.id == cover_id) {
            if (pt.claimed_by == 0 || pt.claimed_by == agent_id) {
                pt.claimed_by = agent_id;
                return true;
            }
            return false; // Already claimed by another agent
        }
    }
    return false;
}

bool CoverSystem::release_cover(u32 cover_id) {
    for (auto& pt : m_cover_points) {
        if (pt.id == cover_id) {
            pt.claimed_by = 0;
            return true;
        }
    }
    return false;
}

void CoverSystem::release_all_for_agent(u32 agent_id) {
    if (agent_id == 0) return;
    for (auto& pt : m_cover_points) {
        if (pt.claimed_by == agent_id) {
            pt.claimed_by = 0;
        }
    }
}

u32 CoverSystem::get_agent_claimed_cover(u32 agent_id) const {
    if (agent_id == 0) return 0;
    for (const auto& pt : m_cover_points) {
        if (pt.claimed_by == agent_id) {
            return pt.id;
        }
    }
    return 0;
}

bool CoverSystem::is_flanked(u32 cover_id, const Vec3& threat_pos, f32 max_flank_angle_deg) const {
    const CoverPoint* pt = get_cover_point(cover_id);
    if (!pt) return true;

    const Vec3 to_threat = threat_pos - pt->position;
    const f32 dist_sq = to_threat.length_sq();
    if (dist_sq < 1e-6f) return true;

    const Vec3 dir_to_threat = to_threat * (1.0f / std::sqrt(dist_sq));
    const f32 dot = pt->normal.dot(dir_to_threat);
    const f32 min_dot = std::cos(max_flank_angle_deg * kDegToRad);

    return dot < min_dot;
}

bool CoverSystem::evaluate_cover_point(const CoverPoint& point, const CoverQuery& query, f32* out_score) const {
    if (!point.enabled) {
        return false;
    }

    if (query.require_unclaimed && point.claimed_by != 0) {
        return false;
    }

    if (query.filter_posture && point.posture != query.requested_posture) {
        return false;
    }

    if (query.required_peek_direction != CoverDirection::None) {
        if ((point.peek_directions & query.required_peek_direction) == 0) {
            return false;
        }
    }

    // Distance to querier
    const Vec3 to_querier = point.position - query.querier_position;
    const f32 dist_to_querier = to_querier.length();
    if (dist_to_querier < query.min_distance_from_querier || dist_to_querier > query.max_distance_from_querier) {
        return false;
    }

    // Distance to threat
    const Vec3 to_threat = query.threat_position - point.position;
    const f32 dist_to_threat = to_threat.length();
    if (dist_to_threat < query.min_distance_to_threat || dist_to_threat > query.max_distance_to_threat) {
        return false;
    }

    // Facing threat check (flank check)
    if (dist_to_threat < 1e-4f) {
        return false;
    }
    const Vec3 dir_to_threat = to_threat * (1.0f / dist_to_threat);
    const f32 facing_dot = point.normal.dot(dir_to_threat);
    const f32 min_dot = std::cos(query.flank_angle_degrees * kDegToRad);
    if (facing_dot < min_dot) {
        return false; // Threat is flanking the cover
    }

    if (out_score) {
        const f32 denom = std::max(1e-4f, 1.0f - min_dot);
        const f32 facing_score = (facing_dot - min_dot) / denom;

        const f32 max_q_dist = std::max(1.0f, query.max_distance_from_querier);
        const f32 querier_dist_score = 1.0f - std::clamp(dist_to_querier / max_q_dist, 0.0f, 1.0f);

        const f32 threat_range = std::max(1.0f, query.max_distance_to_threat - query.min_distance_to_threat);
        const f32 threat_dist_score = std::clamp((dist_to_threat - query.min_distance_to_threat) / threat_range, 0.0f, 1.0f);

        *out_score = query.weight_threat_facing * facing_score
                   + query.weight_distance_to_querier * querier_dist_score
                   + query.weight_distance_to_threat * threat_dist_score
                   + point.custom_score_modifier;
    }

    return true;
}

CoverQueryResult CoverSystem::find_best_cover(const CoverQuery& query) const {
    CoverQueryResult best_result;
    f32 best_score = -1e9f;

    for (const auto& pt : m_cover_points) {
        f32 score = 0.0f;
        if (evaluate_cover_point(pt, query, &score)) {
            if (score > best_score) {
                best_score = score;
                best_result.cover_id = pt.id;
                best_result.position = pt.position;
                best_result.normal = pt.normal;
                best_result.posture = pt.posture;
                best_result.peek_directions = pt.peek_directions;
                best_result.total_score = score;
                best_result.valid = true;
            }
        }
    }

    return best_result;
}

void CoverSystem::find_all_valid_covers(const CoverQuery& query, std::vector<CoverQueryResult>& out_results) const {
    out_results.clear();

    for (const auto& pt : m_cover_points) {
        f32 score = 0.0f;
        if (evaluate_cover_point(pt, query, &score)) {
            CoverQueryResult r;
            r.cover_id = pt.id;
            r.position = pt.position;
            r.normal = pt.normal;
            r.posture = pt.posture;
            r.peek_directions = pt.peek_directions;
            r.total_score = score;
            r.valid = true;
            out_results.push_back(r);
        }
    }

    std::sort(out_results.begin(), out_results.end(), [](const CoverQueryResult& a, const CoverQueryResult& b) {
        return a.total_score > b.total_score;
    });
}

std::vector<u32> CoverSystem::generate_wall_cover(
    const Vec3& wall_start,
    const Vec3& wall_end,
    const Vec3& wall_normal,
    CoverPosture posture,
    f32 spacing,
    f32 standoff
) {
    std::vector<u32> created_ids;

    const Vec3 norm = wall_normal.length_sq() > 1e-6f ? wall_normal.normalized() : Vec3{0.0f, 0.0f, 1.0f};
    const Vec3 wall_dir = wall_end - wall_start;
    const f32 wall_len = wall_dir.length();

    if (wall_len < 0.2f) {
        // Degenerate wall, single point
        CoverPoint pt;
        pt.position = wall_start - norm * standoff;
        pt.normal = norm;
        pt.posture = posture;
        pt.peek_directions = CoverDirection::Left | CoverDirection::Right;
        if (posture == CoverPosture::Low) {
            pt.peek_directions |= CoverDirection::Over;
        }
        created_ids.push_back(register_cover_point(std::move(pt)));
        return created_ids;
    }

    const Vec3 unit_dir = wall_dir * (1.0f / wall_len);
    const u32 num_pts = std::max(2u, static_cast<u32>(std::floor(wall_len / std::max(0.5f, spacing))) + 1u);
    const f32 step = wall_len / static_cast<f32>(num_pts - 1);

    for (u32 i = 0; i < num_pts; ++i) {
        const Vec3 base = wall_start + unit_dir * (step * static_cast<f32>(i));
        CoverPoint pt;
        pt.position = base - norm * standoff;
        pt.normal = norm;
        pt.posture = posture;
        pt.peek_directions = CoverDirection::None;

        if (posture == CoverPosture::Low) {
            pt.peek_directions |= CoverDirection::Over;
        }
        if (i == 0) {
            pt.peek_directions |= CoverDirection::Left;
        }
        if (i == num_pts - 1) {
            pt.peek_directions |= CoverDirection::Right;
        }

        created_ids.push_back(register_cover_point(std::move(pt)));
    }

    return created_ids;
}

void CoverSystem::clear() {
    m_cover_points.clear();
}

} // namespace nf::ai
