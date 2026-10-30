#pragma once

#include <NF/Core/Math.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

namespace nf::animation {

/// A single morph target (blend shape) storing position and normal offsets per vertex.
struct MorphTarget {
    std::string name;
    std::vector<Vec3> position_deltas;
    std::vector<Vec3> normal_deltas;

    void resize(size_t vertex_count) {
        position_deltas.assign(vertex_count, Vec3{0, 0, 0});
        normal_deltas.assign(vertex_count, Vec3{0, 0, 0});
    }

    void set_delta(size_t index, const Vec3& pos_delta, const Vec3& norm_delta = Vec3{}) {
        if (index < position_deltas.size()) {
            position_deltas[index] = pos_delta;
        }
        if (index < normal_deltas.size()) {
            normal_deltas[index] = norm_delta;
        }
    }
};

/// A collection of morph targets belonging to a single mesh (e.g. facial expression shapes).
class MorphTargetSet {
public:
    explicit MorphTargetSet(size_t vertex_count = 0) : m_vertex_count(vertex_count) {}

    void set_vertex_count(size_t count);
    [[nodiscard]] size_t vertex_count() const { return m_vertex_count; }

    size_t add_target(std::string name);
    [[nodiscard]] size_t target_count() const { return m_targets.size(); }

    [[nodiscard]] int find_target_index(std::string_view name) const;
    [[nodiscard]] const MorphTarget* find_target(std::string_view name) const;
    [[nodiscard]] const MorphTarget* get_target(size_t index) const;
    MorphTarget* get_target(size_t index);

    [[nodiscard]] const std::vector<MorphTarget>& targets() const { return m_targets; }

private:
    size_t m_vertex_count = 0;
    std::vector<MorphTarget> m_targets;
};

/// Runtime controller managing blend shape channel weights and computing deformed vertex positions/normals.
class MorphController {
public:
    MorphController() = default;
    explicit MorphController(const MorphTargetSet* target_set);

    void bind_target_set(const MorphTargetSet* target_set);
    [[nodiscard]] const MorphTargetSet* target_set() const { return m_target_set; }

    void set_weight(size_t index, float weight);
    bool set_weight_by_name(std::string_view name, float weight);

    [[nodiscard]] float get_weight(size_t index) const;
    [[nodiscard]] float get_weight_by_name(std::string_view name) const;

    void reset_weights();

    void set_weight_range(float min_w, float max_w) {
        m_min_weight = min_w;
        m_max_weight = max_w;
    }

    /// Evaluates accumulated morph target displacement on input base mesh buffers.
    void apply(const std::vector<Vec3>& base_positions,
               const std::vector<Vec3>& base_normals,
               std::vector<Vec3>& out_positions,
               std::vector<Vec3>& out_normals) const;

private:
    const MorphTargetSet* m_target_set = nullptr;
    std::vector<float> m_weights;
    float m_min_weight = 0.0f;
    float m_max_weight = 1.0f;
};

} // namespace nf::animation
