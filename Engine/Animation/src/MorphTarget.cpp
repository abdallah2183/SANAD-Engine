#include <NF/Animation/MorphTarget.hpp>
#include <cmath>

namespace nf::animation {

void MorphTargetSet::set_vertex_count(size_t count) {
    m_vertex_count = count;
    for (auto& target : m_targets) {
        target.resize(count);
    }
}

size_t MorphTargetSet::add_target(std::string name) {
    MorphTarget t;
    t.name = std::move(name);
    t.resize(m_vertex_count);
    m_targets.push_back(std::move(t));
    return m_targets.size() - 1;
}

int MorphTargetSet::find_target_index(std::string_view name) const {
    for (size_t i = 0; i < m_targets.size(); ++i) {
        if (m_targets[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const MorphTarget* MorphTargetSet::find_target(std::string_view name) const {
    const int idx = find_target_index(name);
    return idx >= 0 ? &m_targets[static_cast<size_t>(idx)] : nullptr;
}

const MorphTarget* MorphTargetSet::get_target(size_t index) const {
    return index < m_targets.size() ? &m_targets[index] : nullptr;
}

MorphTarget* MorphTargetSet::get_target(size_t index) {
    return index < m_targets.size() ? &m_targets[index] : nullptr;
}

MorphController::MorphController(const MorphTargetSet* target_set) {
    bind_target_set(target_set);
}

void MorphController::bind_target_set(const MorphTargetSet* target_set) {
    m_target_set = target_set;
    if (m_target_set) {
        m_weights.assign(m_target_set->target_count(), 0.0f);
    } else {
        m_weights.clear();
    }
}

void MorphController::set_weight(size_t index, float weight) {
    if (index < m_weights.size()) {
        m_weights[index] = std::clamp(weight, m_min_weight, m_max_weight);
    }
}

bool MorphController::set_weight_by_name(std::string_view name, float weight) {
    if (!m_target_set) return false;
    const int idx = m_target_set->find_target_index(name);
    if (idx >= 0) {
        set_weight(static_cast<size_t>(idx), weight);
        return true;
    }
    return false;
}

float MorphController::get_weight(size_t index) const {
    return index < m_weights.size() ? m_weights[index] : 0.0f;
}

float MorphController::get_weight_by_name(std::string_view name) const {
    if (!m_target_set) return 0.0f;
    const int idx = m_target_set->find_target_index(name);
    return idx >= 0 ? get_weight(static_cast<size_t>(idx)) : 0.0f;
}

void MorphController::reset_weights() {
    std::fill(m_weights.begin(), m_weights.end(), 0.0f);
}

void MorphController::apply(const std::vector<Vec3>& base_positions,
                            const std::vector<Vec3>& base_normals,
                            std::vector<Vec3>& out_positions,
                            std::vector<Vec3>& out_normals) const {
    const size_t vcount = base_positions.size();
    out_positions = base_positions;
    out_normals = base_normals;

    if (!m_target_set || m_weights.empty()) return;

    for (size_t t_idx = 0; t_idx < m_weights.size(); ++t_idx) {
        const float w = m_weights[t_idx];
        if (std::abs(w) < 1e-4f) continue;

        const auto* target = m_target_set->get_target(t_idx);
        if (!target) continue;

        const size_t limit = std::min(vcount, target->position_deltas.size());
        for (size_t v = 0; v < limit; ++v) {
            out_positions[v] += target->position_deltas[v] * w;
        }

        const size_t norm_limit = std::min(vcount, target->normal_deltas.size());
        for (size_t v = 0; v < norm_limit; ++v) {
            out_normals[v] += target->normal_deltas[v] * w;
        }
    }

    // Renormalize normals
    for (size_t v = 0; v < out_normals.size(); ++v) {
        out_normals[v] = out_normals[v].normalized();
    }
}

} // namespace nf::animation
