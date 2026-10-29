#pragma once

#include <NF/Core/Math.hpp>
#include <vector>
#include <functional>

namespace nf::ai {

/// An individual candidate spatial position evaluated by the Environment Query System.
struct EQSItem {
    Vec3 position{0.0f, 0.0f, 0.0f};
    float score = 0.0f;
    bool filtered = false;
};

/// Result of an EQS tactical position query.
struct EQSQueryResult {
    Vec3 best_position{0.0f, 0.0f, 0.0f};
    float best_score = -1e9f;
    bool found = false;
    std::vector<EQSItem> items;
};

/// Environment Query System (EQS) for AI tactical positioning, cover finding, and flanking.
class EQSQuery {
public:
    // Generators
    static std::vector<EQSItem> generate_circle(const Vec3& center, float radius, float spacing, float min_radius = 0.0f);
    static std::vector<EQSItem> generate_grid(const Vec3& center, float half_extents, float spacing);
    static std::vector<EQSItem> generate_donut(const Vec3& center, float inner_radius, float outer_radius, float spacing);

    // Scoring & Filtering Tests
    static void score_distance(std::vector<EQSItem>& items, const Vec3& target, float preferred_distance, float weight = 1.0f);
    static void score_direction(std::vector<EQSItem>& items, const Vec3& origin, const Vec3& forward_dir, float preferred_dot = 1.0f, float weight = 1.0f);
    static void filter_range(std::vector<EQSItem>& items, const Vec3& target, float min_dist, float max_dist);

    // Execution
    static EQSQueryResult run(std::vector<EQSItem> items, const std::vector<std::function<void(std::vector<EQSItem>&)>>& tests);
};

} // namespace nf::ai
