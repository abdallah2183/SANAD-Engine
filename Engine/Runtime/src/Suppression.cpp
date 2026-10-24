#include <NF/Runtime/Suppression.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace nf::runtime {

void suppression_add(SuppressionState& state, f32 amount) noexcept {
    if (!std::isfinite(amount) || amount <= 0.0f) {
        return;
    }
    state.level = std::clamp(state.level + amount, 0.0f, 1.0f);
}

void suppression_step(SuppressionState& state, f32 dt) noexcept {
    if (!(dt > 0.0f) || !std::isfinite(dt)) {
        return;
    }
    state.level = std::clamp(state.level - kSuppressionDecayPerSecond * dt, 0.0f, 1.0f);
}

f32 suppression_aim_scale(f32 level) noexcept {
    const f32 clamped = std::clamp(std::isfinite(level) ? level : 0.0f, 0.0f, 1.0f);
    return 1.0f - kSuppressionAimSpeedScale * clamped;
}

f32 suppression_cone_scale(f32 level) noexcept {
    const f32 clamped = std::clamp(std::isfinite(level) ? level : 0.0f, 0.0f, 1.0f);
    return 1.0f - kSuppressionConeScale * clamped;
}

bool suppression_blocks_fire(f32 level) noexcept {
    return std::isfinite(level) && level >= kSuppressionFireBlock;
}

f32 distance_point_to_ray(const Vec3& point, const Vec3& origin, const Vec3& direction,
                          f32 max_distance) noexcept {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
        !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
        !std::isfinite(direction.x) || !std::isfinite(direction.y) ||
        !std::isfinite(direction.z) || !std::isfinite(max_distance)) {
        return std::numeric_limits<f32>::infinity();
    }
    const f32 length_sq = direction.length_sq();
    if (!(length_sq > 1e-12f)) {
        return (point - origin).length();
    }
    const f32 limit = std::max(max_distance, 0.0f);
    f32 t = (point - origin).dot(direction) / length_sq;
    t = std::clamp(t, 0.0f, limit);
    const Vec3 closest = origin + direction * t;
    return (point - closest).length();
}

} // namespace nf::runtime
