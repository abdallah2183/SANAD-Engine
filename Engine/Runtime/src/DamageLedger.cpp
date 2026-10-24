#include <NF/Runtime/DamageLedger.hpp>

#include <algorithm>
#include <cmath>

namespace nf::runtime {

void damage_ledger_record(DamageLedger& ledger, ecs::Entity attacker, f32 health_damage,
                          f32 armor_damage, bool killed) noexcept {
    ledger.last_attacker = attacker;
    ledger.health_damage += std::isfinite(health_damage) ? std::max(health_damage, 0.0f) : 0.0f;
    ledger.armor_damage += std::isfinite(armor_damage) ? std::max(armor_damage, 0.0f) : 0.0f;
    ledger.hits += 1u;
    ledger.seconds_since_damage = 0.0f;
    ledger.has_damage = true;
    if (killed) {
        ledger.killed = true;
        ledger.killer = attacker;
    }
}

void damage_ledger_step(DamageLedger& ledger, f32 dt) noexcept {
    if (!(dt > 0.0f) || !std::isfinite(dt) || !ledger.has_damage) {
        return;
    }
    ledger.seconds_since_damage += dt;
}

} // namespace nf::runtime
