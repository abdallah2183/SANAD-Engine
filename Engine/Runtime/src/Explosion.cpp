#include <NF/Runtime/Explosion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace nf::runtime {

bool explosion_valid(const ExplosionFalloff& blast) noexcept {
    return std::isfinite(blast.radius) && blast.radius > 0.0f &&
           std::isfinite(blast.damage) && blast.damage > 0.0f &&
           std::isfinite(blast.min_scale);
}

f32 explosion_damage_at(const ExplosionFalloff& blast, f32 distance) noexcept {
    if (!explosion_valid(blast)) {
        return 0.0f;
    }
    if (!std::isfinite(distance) || distance <= 0.0f) {
        return blast.damage;
    }
    if (distance >= blast.radius) {
        return 0.0f;
    }
    const f32 edge = std::clamp(blast.min_scale, 0.0f, 1.0f);
    const f32 t = distance / blast.radius;
    const f32 scale = 1.0f + (edge - 1.0f) * t;
    return blast.damage * std::max(scale, 0.0f);
}

f32 distance_point_to_aabb(const Vec3& point, const Vec3& min, const Vec3& max) noexcept {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
        !std::isfinite(min.x) || !std::isfinite(min.y) || !std::isfinite(min.z) ||
        !std::isfinite(max.x) || !std::isfinite(max.y) || !std::isfinite(max.z)) {
        return std::numeric_limits<f32>::infinity();
    }
    if (min.x > max.x || min.y > max.y || min.z > max.z) {
        return std::numeric_limits<f32>::infinity();
    }
    const f32 dx = std::max(std::max(min.x - point.x, point.x - max.x), 0.0f);
    const f32 dy = std::max(std::max(min.y - point.y, point.y - max.y), 0.0f);
    const f32 dz = std::max(std::max(min.z - point.z, point.z - max.z), 0.0f);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace nf::runtime
