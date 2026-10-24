#include <NF/Runtime/StatusEffect.hpp>

#include <algorithm>
#include <cmath>

namespace nf::runtime {

const char* status_kind_name(StatusKind kind) noexcept {
    switch (kind) {
    case StatusKind::Burning: return "burn";
    case StatusKind::Bleeding: return "bleed";
    case StatusKind::Corroding: return "corrode";
    case StatusKind::None: return "none";
    }
    return "none";
}

bool status_kind_from_name(std::string_view name, StatusKind& out) noexcept {
    if (name == "none") {
        out = StatusKind::None;
        return true;
    }
    if (name == "burn") {
        out = StatusKind::Burning;
        return true;
    }
    if (name == "bleed") {
        out = StatusKind::Bleeding;
        return true;
    }
    if (name == "corrode") {
        out = StatusKind::Corroding;
        return true;
    }
    return false;
}

DamageType status_damage_type(StatusKind kind) noexcept {
    switch (kind) {
    case StatusKind::Burning: return DamageType::Fire;
    case StatusKind::Bleeding: return DamageType::Ballistic;
    case StatusKind::Corroding: return DamageType::Explosive;
    case StatusKind::None: break;
    }
    return DamageType::Ballistic;
}

StatusEffect make_status(StatusKind kind, f32 damage_per_second, f32 duration_seconds,
                         f32 tick_seconds, ecs::Entity source) noexcept {
    StatusEffect effect;
    effect.kind = kind;
    effect.damage_per_second = damage_per_second;
    effect.remaining_seconds = duration_seconds;
    effect.tick_seconds = tick_seconds;
    effect.source = source;
    return effect;
}

bool status_valid(const StatusEffect& effect) noexcept {
    return effect.kind != StatusKind::None && std::isfinite(effect.damage_per_second) &&
           effect.damage_per_second > 0.0f && std::isfinite(effect.remaining_seconds) &&
           effect.remaining_seconds > 0.0f && std::isfinite(effect.tick_seconds) &&
           effect.tick_seconds > 0.0f && std::isfinite(effect.tick_accumulator) &&
           effect.tick_accumulator >= 0.0f;
}

bool status_add(StatusEffectSet& set, const StatusEffect& effect) noexcept {
    if (!status_valid(effect)) {
        return false;
    }
    for (u32 i = 0; i < set.count; ++i) {
        StatusEffect& existing = set.effects[i];
        if (existing.kind != effect.kind) {
            continue;
        }
        // Refresh, do not stack: two fire sources should not multiply into a
        // furnace. The stronger dps/duration wins; the newest cadence and a
        // valid new source are adopted.
        existing.damage_per_second = std::max(existing.damage_per_second,
                                              effect.damage_per_second);
        existing.remaining_seconds = std::max(existing.remaining_seconds,
                                              effect.remaining_seconds);
        existing.tick_seconds = effect.tick_seconds;
        if (effect.source.valid()) {
            existing.source = effect.source;
        }
        return true;
    }
    if (set.count < kMaxStatusEffects) {
        set.effects[set.count] = effect;
        set.count += 1u;
        return true;
    }
    // The capacity is a hard bound. Replace the effect closest to expiry; a
    // tie resolves to the lower index so the result is deterministic.
    u32 weakest = 0u;
    for (u32 i = 1; i < set.count; ++i) {
        if (set.effects[i].remaining_seconds < set.effects[weakest].remaining_seconds) {
            weakest = i;
        }
    }
    set.effects[weakest] = effect;
    return true;
}

bool status_remove(StatusEffectSet& set, StatusKind kind) noexcept {
    for (u32 i = 0; i < set.count; ++i) {
        if (set.effects[i].kind != kind) {
            continue;
        }
        for (u32 j = i + 1u; j < set.count; ++j) {
            set.effects[j - 1u] = set.effects[j];
        }
        set.count -= 1u;
        set.effects[set.count] = StatusEffect{};
        return true;
    }
    return false;
}

const StatusEffect* status_find(const StatusEffectSet& set, StatusKind kind) noexcept {
    for (u32 i = 0; i < set.count; ++i) {
        if (set.effects[i].kind == kind) {
            return &set.effects[i];
        }
    }
    return nullptr;
}

f32 status_remaining(const StatusEffectSet& set, StatusKind kind) noexcept {
    const StatusEffect* effect = status_find(set, kind);
    return effect != nullptr ? effect->remaining_seconds : 0.0f;
}

u32 status_step(StatusEffectSet& set, f32 dt, std::vector<StatusTick>& out) noexcept {
    out.clear();
    if (!(dt > 0.0f) || !std::isfinite(dt) || set.count == 0u) {
        return 0u;
    }

    u32 written = 0u;
    for (u32 i = 0; i < set.count; ++i) {
        StatusEffect effect = set.effects[i];
        if (!status_valid(effect)) {
            continue;
        }

        const f32 active = std::min(dt, effect.remaining_seconds);
        if (!(active > 0.0f)) {
            continue;
        }
        effect.tick_accumulator += active;
        effect.remaining_seconds = std::max(0.0f, effect.remaining_seconds - active);

        // A long frame can cross several ticks. Emitting each one keeps the
        // damage and the event count independent of frame rate.
        while (effect.tick_accumulator >= effect.tick_seconds - 1e-6f) {
            StatusTick tick;
            tick.kind = effect.kind;
            tick.damage_type = status_damage_type(effect.kind);
            tick.damage = effect.damage_per_second * effect.tick_seconds;
            tick.source = effect.source;
            out.push_back(tick);
            effect.tick_accumulator -= effect.tick_seconds;
        }

        if (effect.remaining_seconds <= 1e-6f) {
            // Pay the partial final tick. Without this, a 0.75 s burn with a
            // 0.5 s tick would silently lose half its authored damage.
            if (effect.tick_accumulator > 1e-6f) {
                StatusTick tick;
                tick.kind = effect.kind;
                tick.damage_type = status_damage_type(effect.kind);
                tick.damage = effect.damage_per_second * effect.tick_accumulator;
                tick.source = effect.source;
                out.push_back(tick);
            }
            continue; // expired: do not copy it to the live array
        }

        set.effects[written] = effect;
        written += 1u;
    }

    for (u32 i = written; i < set.count; ++i) {
        set.effects[i] = StatusEffect{};
    }
    set.count = written;
    return static_cast<u32>(out.size());
}

void status_clear(StatusEffectSet& set) noexcept {
    for (u32 i = 0; i < set.count; ++i) {
        set.effects[i] = StatusEffect{};
    }
    set.count = 0u;
}

} // namespace nf::runtime
