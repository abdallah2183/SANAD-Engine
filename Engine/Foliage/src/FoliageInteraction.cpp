#include <NF/Foliage/FoliageInteraction.hpp>
#include <algorithm>
#include <cmath>

namespace nf::foliage {

namespace {

inline f32 clamp01(f32 v) {
    return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
}

Vec3 closest_point_on_segment(const Vec3& p, const Vec3& a, const Vec3& b) {
    Vec3 ab = b - a;
    f32 len_sq = ab.length_sq();
    if (len_sq < 1e-6f) {
        return a;
    }
    f32 t = (p - a).dot(ab) / len_sq;
    t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
    return a + ab * t;
}

} // namespace

u32 FoliageBendField::register_interactor(const FoliageInteractor& interactor) {
    u32 id = m_next_id++;
    FoliageInteractor copy = interactor;
    copy.id = id;
    m_interactors.push_back(copy);
    return id;
}

bool FoliageBendField::update_interactor(u32 id, const Vec3& new_pos, const Vec3& velocity) {
    for (auto& it : m_interactors) {
        if (it.id == id) {
            it.position = new_pos;
            it.velocity = velocity;
            return true;
        }
    }
    return false;
}

bool FoliageBendField::set_interactor(const FoliageInteractor& interactor) {
    for (auto& it : m_interactors) {
        if (it.id == interactor.id) {
            it = interactor;
            return true;
        }
    }
    return false;
}

bool FoliageBendField::unregister_interactor(u32 id) {
    auto it = std::find_if(m_interactors.begin(), m_interactors.end(),
                           [id](const FoliageInteractor& elem) { return elem.id == id; });
    if (it != m_interactors.end()) {
        m_interactors.erase(it);
        return true;
    }
    return false;
}

void FoliageBendField::clear() {
    m_interactors.clear();
    m_stamps.clear();
}

void FoliageBendField::add_trample_stamp(const Vec3& position, f32 radius, f32 depth,
                                         f32 lifetime, const Vec3& push_dir) {
    if (radius <= 0.001f || lifetime <= 0.001f) {
        return;
    }
    TrampleStamp stamp;
    stamp.position = position;
    stamp.radius = radius;
    stamp.depth = clamp01(depth);
    stamp.age = 0.0f;
    stamp.lifetime = lifetime;
    stamp.push_dir = push_dir;
    m_stamps.push_back(stamp);
}

void FoliageBendField::update(f32 dt) {
    if (dt <= 0.0f) {
        return;
    }

    // Advance stamps and remove expired ones
    for (auto& stamp : m_stamps) {
        stamp.age += dt;
    }
    m_stamps.erase(
        std::remove_if(m_stamps.begin(), m_stamps.end(),
                       [](const TrampleStamp& s) { return s.age >= s.lifetime; }),
        m_stamps.end());
}

FoliageBendResult FoliageBendField::evaluate(const Vec3& world_pos, f32 plant_height, f32 plant_stiffness) const {
    FoliageBendResult result;
    f32 effective_height = (plant_height > 0.05f) ? plant_height : 0.05f;
    f32 stiffness = (plant_stiffness > 0.1f) ? plant_stiffness : 0.1f;

    Vec3 accum_disp{0.0f, 0.0f, 0.0f};
    f32 total_weight = 0.0f;
    f32 max_depression = 0.0f;

    // 1. Evaluate active dynamic interactors
    for (const auto& interactor : m_interactors) {
        if (!interactor.active || interactor.radius <= 0.001f) {
            continue;
        }

        Vec3 closest_pt = interactor.position;
        if (interactor.shape == InteractorShape::Capsule) {
            closest_pt = closest_point_on_segment(world_pos, interactor.position, interactor.end_position);
        }

        Vec3 delta = world_pos - closest_pt;
        f32 dist_sq = delta.length_sq();
        f32 rad_sq = interactor.radius * interactor.radius;
        if (dist_sq >= rad_sq) {
            continue;
        }

        f32 dist = std::sqrt(dist_sq);
        f32 norm_dist = dist / interactor.radius;
        // Smooth quadratic falloff (1 - u^2)^2
        f32 falloff = 1.0f - norm_dist * norm_dist;
        falloff = falloff * falloff;

        // Push direction: away from interactor horizontally, plus downward push
        Vec3 push_dir{delta.x, 0.0f, delta.z};
        f32 h_dist = std::sqrt(push_dir.x * push_dir.x + push_dir.z * push_dir.z);
        if (h_dist > 1e-4f) {
            push_dir.x /= h_dist;
            push_dir.z /= h_dist;
        } else {
            push_dir = Vec3{1.0f, 0.0f, 0.0f};
        }

        // Add velocity wake drag if moving
        f32 v_speed = interactor.velocity.length();
        if (v_speed > 0.05f) {
            Vec3 norm_v = interactor.velocity * (1.0f / v_speed);
            push_dir = push_dir * 0.7f + norm_v * 0.3f;
            f32 p_len = push_dir.length();
            if (p_len > 1e-4f) {
                push_dir = push_dir * (1.0f / p_len);
            }
        }

        f32 down_ratio = clamp01(interactor.push_down_ratio);
        push_dir.x *= (1.0f - down_ratio);
        push_dir.z *= (1.0f - down_ratio);
        push_dir.y = -down_ratio;

        f32 strength = interactor.strength * (effective_height / stiffness);
        f32 disp_magnitude = falloff * strength;

        accum_disp = accum_disp + push_dir * disp_magnitude;
        total_weight += falloff;

        f32 depression_sample = falloff * down_ratio * (interactor.strength / stiffness);
        if (depression_sample > max_depression) {
            max_depression = depression_sample;
        }
    }

    // 2. Evaluate persistent trample stamps
    for (const auto& stamp : m_stamps) {
        if (stamp.age >= stamp.lifetime || stamp.radius <= 0.001f) {
            continue;
        }

        Vec3 delta = world_pos - stamp.position;
        f32 dist_sq = delta.length_sq();
        f32 rad_sq = stamp.radius * stamp.radius;
        if (dist_sq >= rad_sq) {
            continue;
        }

        f32 dist = std::sqrt(dist_sq);
        f32 norm_dist = dist / stamp.radius;
        f32 spatial_falloff = 1.0f - norm_dist * norm_dist;
        spatial_falloff = spatial_falloff * spatial_falloff;

        f32 temporal_rem = 1.0f - (stamp.age / stamp.lifetime);
        f32 stamp_weight = spatial_falloff * temporal_rem * stamp.depth;

        f32 stamp_mag = stamp_weight * (effective_height / stiffness);
        accum_disp = accum_disp + stamp.push_dir * stamp_mag;
        total_weight += stamp_weight;

        if (stamp_weight > max_depression) {
            max_depression = stamp_weight;
        }
    }

    // Clamp total displacement to configured ceiling
    f32 total_mag = accum_disp.length();
    if (total_mag > m_max_displacement && total_mag > 1e-4f) {
        accum_disp = accum_disp * (m_max_displacement / total_mag);
    }

    result.displacement = accum_disp;
    result.depression = clamp01(max_depression);
    result.influence = clamp01(total_weight);

    // Approximate bend angle based on displacement horizontal vs vertical offset
    f32 horiz_offset = std::sqrt(accum_disp.x * accum_disp.x + accum_disp.z * accum_disp.z);
    result.bend_angle = std::atan2(horiz_offset, effective_height);

    return result;
}

Vec3 FoliageBendField::evaluate_combined(const FoliageInstance& instance,
                                        const FoliageType& type,
                                        const WindState& wind,
                                        f32 plant_stiffness) const {
    Vec3 inst_pos{instance.position[0], instance.position[1], instance.position[2]};

    // Calculate natural ambient wind deflection
    Vec3 wind_offset = wind_displacement(instance, type, wind);

    // Calculate dynamic interactor deflection
    f32 stiffness = (plant_stiffness > 0.05f) ? plant_stiffness : 1.0f;
    f32 height = instance.scale;

    FoliageBendResult bend = evaluate(inst_pos, height, stiffness);

    Vec3 combined = wind_offset + bend.displacement;
    f32 c_mag = combined.length();
    if (c_mag > m_max_displacement && c_mag > 1e-4f) {
        combined = combined * (m_max_displacement / c_mag);
    }
    return combined;
}

void FoliageBendField::evaluate_batch(std::span<const FoliageInstance> instances,
                                      const FoliageType& type,
                                      const WindState& wind,
                                      std::span<Vec3> out_displacements,
                                      f32 plant_stiffness) const {
    size_t count = std::min(instances.size(), out_displacements.size());
    for (size_t i = 0; i < count; ++i) {
        out_displacements[i] = evaluate_combined(instances[i], type, wind, plant_stiffness);
    }
}

size_t FoliageBendField::active_interactor_count() const {
    size_t count = 0;
    for (const auto& it : m_interactors) {
        if (it.active) {
            count++;
        }
    }
    return count;
}

} // namespace nf::foliage
