#include <NF/Runtime/Damage.hpp>

#include <algorithm>
#include <cmath>

namespace nf::runtime {

const char* damage_type_name(DamageType type) noexcept {
    switch (type) {
    case DamageType::Explosive: return "explosive";
    case DamageType::Fire: return "fire";
    case DamageType::Ballistic: return "ballistic";
    }
    return "ballistic";
}

bool damage_type_from_name(std::string_view name, DamageType& out) noexcept {
    if (name == "ballistic") {
        out = DamageType::Ballistic;
        return true;
    }
    if (name == "explosive") {
        out = DamageType::Explosive;
        return true;
    }
    if (name == "fire") {
        out = DamageType::Fire;
        return true;
    }
    return false;
}

f32 health_resistance(const HealthComponent& health, DamageType type) noexcept {
    if (!health_valid(health)) {
        return 0.0f;
    }
    f32 value = 0.0f;
    switch (type) {
    case DamageType::Ballistic: value = health.ballistic_resistance; break;
    case DamageType::Explosive: value = health.explosive_resistance; break;
    case DamageType::Fire: value = health.fire_resistance; break;
    }
    return std::clamp(value, 0.0f, 1.0f);
}

const char* hit_zone_name(HitZone zone) noexcept {
    switch (zone) {
    case HitZone::Head: return "head";
    case HitZone::Limbs: return "limbs";
    case HitZone::Body: return "body";
    }
    return "body";
}

bool hit_zone_from_name(std::string_view name, HitZone& out) noexcept {
    if (name == "body") {
        out = HitZone::Body;
        return true;
    }
    if (name == "head") {
        out = HitZone::Head;
        return true;
    }
    if (name == "limbs") {
        out = HitZone::Limbs;
        return true;
    }
    return false;
}

HitZone hit_zone_from_height(f32 point_y, f32 min_y, f32 max_y) noexcept {
    if (!std::isfinite(point_y) || !std::isfinite(min_y) || !std::isfinite(max_y)) {
        return HitZone::Body;
    }
    const f32 height = max_y - min_y;
    if (!(height > 1e-6f)) {
        return HitZone::Body;
    }
    const f32 t = std::clamp((point_y - min_y) / height, 0.0f, 1.0f);
    if (t >= 1.0f - kHeadZoneFraction) {
        return HitZone::Head;
    }
    if (t <= kLimbZoneFraction) {
        return HitZone::Limbs;
    }
    return HitZone::Body;
}

DamageResult health_apply_hit(HealthComponent& health, f32 amount, HitZone zone,
                              f32 armor_penetration, DamageType type) noexcept {
    DamageResult result;
    result.incoming = amount;
    result.zone = zone;
    result.type = type;

    if (!health.enabled || health.invulnerable || !health_valid(health)) {
        return result;
    }
    if (!std::isfinite(amount) || amount <= 0.0f) {
        return result;
    }

    f32 multiplier = 1.0f;
    if (health.hit_zones) {
        switch (zone) {
        case HitZone::Head: multiplier = health.headshot_multiplier; break;
        case HitZone::Limbs: multiplier = health.limb_multiplier; break;
        case HitZone::Body: break;
        }
    }
    result.zoned = amount * multiplier;
    const f32 resistance = health_resistance(health, type);
    result.resisted = result.zoned * resistance;
    const f32 effective = result.zoned - result.resisted;

    // A malformed penetration is a scene bug, but it must not poison the hit.
    // Clamping to the documented range is the same choice the spread and
    // falloff paths make: refuse at the loader, stay finite at runtime.
    f32 pierce = armor_penetration;
    if (!std::isfinite(pierce)) {
        pierce = 0.0f;
    }
    pierce = std::clamp(pierce, 0.0f, 1.0f);

    f32 health_damage = effective;
    if (health.armor > 0.0f && health.armor_absorb > 0.0f) {
        const f32 protectable = effective * (1.0f - pierce);
        const f32 wanted = protectable * health.armor_absorb;
        const f32 absorbed = std::min(std::max(wanted, 0.0f), health.armor);
        health.armor = std::max(0.0f, health.armor - absorbed);
        health_damage = std::max(0.0f, effective - absorbed);
        result.armor_absorbed = absorbed;
    }

    const f32 before = health.health;
    health.health = std::max(0.0f, health.health - health_damage);
    health.since_damage = 0.0f;

    result.health_damage = before - health.health;
    result.accepted = true;
    result.killed = before > 0.0f && health.health <= 0.0f;
    return result;
}

f32 health_armor_fraction(const HealthComponent& health) noexcept {
    if (!health_valid(health) || !(health.max_armor > 0.0f)) {
        return 0.0f;
    }
    return std::clamp(health.armor / health.max_armor, 0.0f, 1.0f);
}

} // namespace nf::runtime
