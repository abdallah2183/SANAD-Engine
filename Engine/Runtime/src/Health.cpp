#include <NF/Runtime/Health.hpp>

#include <algorithm>
#include <cmath>

namespace nf::runtime {

bool health_valid(const HealthComponent& health) noexcept {
    return std::isfinite(health.max_health) && health.max_health > 0.0f &&
           std::isfinite(health.health) && std::isfinite(health.max_armor) &&
           health.max_armor >= 0.0f && std::isfinite(health.armor) && health.armor >= 0.0f &&
           std::isfinite(health.armor_absorb) && health.armor_absorb >= 0.0f &&
           health.armor_absorb <= 1.0f && std::isfinite(health.headshot_multiplier) &&
           health.headshot_multiplier > 0.0f && std::isfinite(health.limb_multiplier) &&
           health.limb_multiplier >= 0.0f && std::isfinite(health.ballistic_resistance) &&
           health.ballistic_resistance >= 0.0f && health.ballistic_resistance <= 1.0f &&
           std::isfinite(health.explosive_resistance) &&
           health.explosive_resistance >= 0.0f && health.explosive_resistance <= 1.0f &&
           std::isfinite(health.fire_resistance) && health.fire_resistance >= 0.0f &&
           health.fire_resistance <= 1.0f;
}

f32 health_fraction(const HealthComponent& health) noexcept {
    if (!health_valid(health)) {
        return 0.0f;
    }
    return std::clamp(health.health / health.max_health, 0.0f, 1.0f);
}

bool health_alive(const HealthComponent& health) noexcept {
    return health_valid(health) && health.health > 0.0f;
}

f32 health_apply_damage(HealthComponent& health, f32 amount) noexcept {
    if (!health.enabled || health.invulnerable || !health_valid(health)) {
        return 0.0f;
    }
    if (!std::isfinite(amount) || amount <= 0.0f) {
        return 0.0f;
    }
    const f32 before = health.health;
    health.health = std::max(0.0f, health.health - amount);
    health.since_damage = 0.0f;
    return before - health.health;
}

f32 health_heal(HealthComponent& health, f32 amount) noexcept {
    if (!health_valid(health)) {
        return 0.0f;
    }
    if (!std::isfinite(amount) || amount <= 0.0f) {
        return 0.0f;
    }
    const f32 before = health.health;
    health.health = std::min(health.max_health, health.health + amount);
    return health.health - before;
}

f32 health_step(HealthComponent& health, f32 dt) noexcept {
    if (!health.enabled || !health_valid(health)) {
        return 0.0f;
    }
    if (!(dt > 0.0f) || !std::isfinite(dt)) {
        return 0.0f;
    }
    health.since_damage += dt;
    if (health.regen_per_second <= 0.0f || health.since_damage < health.regen_delay) {
        return 0.0f;
    }
    return health_heal(health, health.regen_per_second * dt);
}

} // namespace nf::runtime
