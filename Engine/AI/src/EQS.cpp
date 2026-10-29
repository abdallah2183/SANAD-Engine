#include <NF/AI/EQS.hpp>
#include <algorithm>
#include <cmath>

namespace nf::ai {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
}

std::vector<EQSItem> EQSQuery::generate_circle(const Vec3& center, float radius, float spacing, float min_radius) {
    std::vector<EQSItem> items;
    spacing = std::max(0.1f, spacing);
    radius = std::max(spacing, radius);
    min_radius = std::max(0.0f, min_radius);

    if (min_radius <= 0.01f) {
        items.push_back({center, 0.0f, false});
        min_radius = spacing;
    }

    for (float r = min_radius; r <= radius + 0.001f; r += spacing) {
        int count = std::max(4, static_cast<int>(std::round(kTwoPi * r / spacing)));
        for (int i = 0; i < count; ++i) {
            float theta = (kTwoPi * static_cast<float>(i)) / static_cast<float>(count);
            Vec3 pos(
                center.x + r * std::cos(theta),
                center.y,
                center.z + r * std::sin(theta)
            );
            items.push_back({pos, 0.0f, false});
        }
    }
    return items;
}

std::vector<EQSItem> EQSQuery::generate_grid(const Vec3& center, float half_extents, float spacing) {
    std::vector<EQSItem> items;
    spacing = std::max(0.1f, spacing);
    half_extents = std::max(0.0f, half_extents);

    for (float x = -half_extents; x <= half_extents + 0.001f; x += spacing) {
        for (float z = -half_extents; z <= half_extents + 0.001f; z += spacing) {
            items.push_back({center + Vec3(x, 0.0f, z), 0.0f, false});
        }
    }
    return items;
}

std::vector<EQSItem> EQSQuery::generate_donut(const Vec3& center, float inner_radius, float outer_radius, float spacing) {
    return generate_circle(center, outer_radius, spacing, inner_radius);
}

void EQSQuery::score_distance(std::vector<EQSItem>& items, const Vec3& target, float preferred_distance, float weight) {
    preferred_distance = std::max(0.0f, preferred_distance);
    for (auto& item : items) {
        if (item.filtered) continue;
        float d = (item.position - target).length();
        float delta = std::abs(d - preferred_distance);
        float score_val = std::max(0.0f, 1.0f - (delta / (preferred_distance + 1.0f)));
        item.score += score_val * weight;
    }
}

void EQSQuery::score_direction(std::vector<EQSItem>& items, const Vec3& origin, const Vec3& forward_dir, float preferred_dot, float weight) {
    float fwd_len = forward_dir.length();
    Vec3 fwd = (fwd_len > 0.0001f) ? (forward_dir / fwd_len) : Vec3(0, 0, 1);

    for (auto& item : items) {
        if (item.filtered) continue;
        Vec3 diff = item.position - origin;
        float d = diff.length();
        if (d > 0.0001f) {
            Vec3 dir = diff / d;
            float dot = std::clamp(dir.dot(fwd), -1.0f, 1.0f);
            float delta = std::abs(dot - preferred_dot);
            float score_val = std::max(0.0f, 1.0f - delta * 0.5f);
            item.score += score_val * weight;
        }
    }
}

void EQSQuery::filter_range(std::vector<EQSItem>& items, const Vec3& target, float min_dist, float max_dist) {
    for (auto& item : items) {
        if (item.filtered) continue;
        float d = (item.position - target).length();
        if (d < min_dist || d > max_dist) {
            item.filtered = true;
        }
    }
}

EQSQueryResult EQSQuery::run(std::vector<EQSItem> items, const std::vector<std::function<void(std::vector<EQSItem>&)>>& tests) {
    for (const auto& test : tests) {
        if (test) {
            test(items);
        }
    }

    EQSQueryResult res;
    res.items = items;
    res.best_score = -1e9f;
    res.found = false;

    for (const auto& item : items) {
        if (!item.filtered && item.score > res.best_score) {
            res.best_score = item.score;
            res.best_position = item.position;
            res.found = true;
        }
    }

    return res;
}

} // namespace nf::ai
