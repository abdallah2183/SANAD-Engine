#include <NF/Gameplay/GameplayEffect.hpp>

namespace nf::gameplay {

GameplayEffectDef::GameplayEffectDef(std::string name)
    : m_name(std::move(name)) {}

std::shared_ptr<GameplayEffectDef> GameplayEffectDef::make_instant(std::string name, std::string attribute, f32 delta) {
    auto def = std::make_shared<GameplayEffectDef>(std::move(name));
    def->set_duration_policy(EffectDurationPolicy::Instant);
    def->add_modifier(std::move(attribute), ModifierOp::Add, delta, false);
    return def;
}

std::shared_ptr<GameplayEffectDef> GameplayEffectDef::make_buff(std::string name, f32 duration, std::string attribute, ModifierOp op, f32 magnitude) {
    auto def = std::make_shared<GameplayEffectDef>(std::move(name));
    def->set_duration_policy(EffectDurationPolicy::Duration, duration);
    def->add_modifier(std::move(attribute), op, magnitude, false);
    return def;
}

std::shared_ptr<GameplayEffectDef> GameplayEffectDef::make_dot(std::string name, f32 duration, f32 period, std::string attribute, f32 tick_damage, u32 max_stacks) {
    auto def = std::make_shared<GameplayEffectDef>(std::move(name));
    def->set_duration_policy(EffectDurationPolicy::Duration, duration);
    def->set_period(period);
    def->set_stacking(max_stacks > 1 ? EffectStackingPolicy::StackCount : EffectStackingPolicy::None, max_stacks);
    def->add_modifier(std::move(attribute), ModifierOp::Add, tick_damage, true);
    return def;
}

// ----------------------------------------------------------------------------
// ActiveEffectsContainer
// ----------------------------------------------------------------------------

void ActiveEffectsContainer::apply_continuous_modifiers(const ActiveGameplayEffect& active, AttributeSet& attributes) {
    if (active.is_inhibited || !active.def) {
        return;
    }
    const f32 stack_multiplier = static_cast<f32>(active.current_stacks);
    for (const auto& mod : active.def->modifiers()) {
        if (!mod.is_periodic) {
            const f32 scaled_mag = (mod.op == ModifierOp::Add) ? (mod.magnitude * stack_multiplier) : mod.magnitude;
            attributes.add_modifier(mod.attribute_name, AttributeModifier{mod.op, scaled_mag, active.handle.id});
        }
    }
}

void ActiveEffectsContainer::remove_continuous_modifiers(const ActiveGameplayEffect& active, AttributeSet& attributes) {
    if (!active.def) {
        return;
    }
    for (const auto& mod : active.def->modifiers()) {
        if (!mod.is_periodic) {
            attributes.remove_modifiers_by_source(mod.attribute_name, active.handle.id);
        }
    }
}

void ActiveEffectsContainer::check_inhibition(ActiveGameplayEffect& active, TagContainer& actor_tags, AttributeSet& attributes) {
    if (!active.def) {
        return;
    }

    bool should_inhibit = false;
    for (const auto& req : active.def->ongoing_required_tags().all()) {
        if (!actor_tags.has(req)) {
            should_inhibit = true;
            break;
        }
    }
    if (!should_inhibit) {
        for (const auto& blk : active.def->ongoing_blocked_tags().all()) {
            if (actor_tags.has(blk)) {
                should_inhibit = true;
                break;
            }
        }
    }

    if (should_inhibit != active.is_inhibited) {
        active.is_inhibited = should_inhibit;
        if (active.is_inhibited) {
            remove_continuous_modifiers(active, attributes);
            for (const auto& tag : active.def->granted_tags().all()) {
                actor_tags.remove(tag);
            }
        } else {
            for (const auto& tag : active.def->granted_tags().all()) {
                actor_tags.add(tag);
            }
            apply_continuous_modifiers(active, attributes);
        }
    }
}

ActiveEffectHandle ActiveEffectsContainer::apply_effect(
    const std::shared_ptr<const GameplayEffectDef>& effect_def,
    AttributeSet& attributes,
    TagContainer& actor_tags,
    u64 source_id)
{
    if (!effect_def) {
        return {0};
    }

    // 1. Cleansing tags
    if (!effect_def->cleanse_tags().empty()) {
        remove_effects_with_tags(effect_def->cleanse_tags(), attributes, actor_tags);
    }

    // 2. Instant effect execution
    if (effect_def->duration_policy() == EffectDurationPolicy::Instant) {
        for (const auto& mod : effect_def->modifiers()) {
            f32 base = attributes.get_base_value(mod.attribute_name);
            if (mod.op == ModifierOp::Add) {
                base += mod.magnitude;
            } else if (mod.op == ModifierOp::Multiply) {
                base *= mod.magnitude;
            } else if (mod.op == ModifierOp::Override) {
                base = mod.magnitude;
            }
            attributes.set_base_value(mod.attribute_name, base);
        }
        return {0};
    }

    // 3. Stacking on existing active effect
    for (auto& active : m_active_effects) {
        if (active.def && active.def->name() == effect_def->name()) {
            if (effect_def->stacking_policy() == EffectStackingPolicy::StackCount) {
                remove_continuous_modifiers(active, attributes);
                if (active.current_stacks < effect_def->max_stacks()) {
                    active.current_stacks++;
                }
                if (effect_def->refresh_on_stack()) {
                    active.duration_remaining = effect_def->duration();
                }
                apply_continuous_modifiers(active, attributes);
                return active.handle;
            } else if (effect_def->stacking_policy() == EffectStackingPolicy::StackDuration) {
                active.duration_remaining += effect_def->duration();
                return active.handle;
            } else {
                if (effect_def->refresh_on_stack()) {
                    active.duration_remaining = effect_def->duration();
                }
                return active.handle;
            }
        }
    }

    // 4. New active effect
    ActiveGameplayEffect active;
    active.handle = {m_next_handle_id++};
    active.def = effect_def;
    active.duration_remaining = effect_def->duration();
    active.period_remaining = effect_def->period();
    active.current_stacks = 1;
    active.source_id = source_id;
    active.is_inhibited = false;

    // Grant tags
    for (const auto& tag : effect_def->granted_tags().all()) {
        actor_tags.add(tag);
    }

    apply_continuous_modifiers(active, attributes);
    check_inhibition(active, actor_tags, attributes);

    m_active_effects.push_back(std::move(active));
    return m_active_effects.back().handle;
}

bool ActiveEffectsContainer::remove_effect(ActiveEffectHandle handle, AttributeSet& attributes, TagContainer& actor_tags) {
    if (!handle.valid()) {
        return false;
    }

    auto it = std::find_if(m_active_effects.begin(), m_active_effects.end(), [handle](const ActiveGameplayEffect& e) {
        return e.handle == handle;
    });

    if (it != m_active_effects.end()) {
        remove_continuous_modifiers(*it, attributes);
        if (it->def) {
            for (const auto& tag : it->def->granted_tags().all()) {
                actor_tags.remove(tag);
            }
        }
        m_active_effects.erase(it);
        return true;
    }
    return false;
}

size_t ActiveEffectsContainer::remove_effects_with_tags(const TagContainer& tags, AttributeSet& attributes, TagContainer& actor_tags) {
    size_t removed_count = 0;
    for (auto it = m_active_effects.begin(); it != m_active_effects.end();) {
        bool matches = false;
        if (it->def) {
            for (const auto& tag : tags.all()) {
                if (it->def->granted_tags().has(tag)) {
                    matches = true;
                    break;
                }
            }
        }

        if (matches) {
            remove_continuous_modifiers(*it, attributes);
            if (it->def) {
                for (const auto& tag : it->def->granted_tags().all()) {
                    actor_tags.remove(tag);
                }
            }
            it = m_active_effects.erase(it);
            removed_count++;
        } else {
            ++it;
        }
    }
    return removed_count;
}

void ActiveEffectsContainer::update(f32 dt, AttributeSet& attributes, TagContainer& actor_tags) {
    const f32 safe_dt = std::max(0.0f, dt);

    for (auto it = m_active_effects.begin(); it != m_active_effects.end();) {
        auto& active = *it;
        if (!active.def) {
            it = m_active_effects.erase(it);
            continue;
        }

        check_inhibition(active, actor_tags, attributes);

        if (!active.is_inhibited) {
            // Periodic tick handling
            if (active.def->is_periodic()) {
                active.period_remaining -= safe_dt;
                while (active.period_remaining <= 0.0f) {
                    const f32 stack_multiplier = static_cast<f32>(active.current_stacks);
                    for (const auto& mod : active.def->modifiers()) {
                        if (mod.is_periodic) {
                            f32 base = attributes.get_base_value(mod.attribute_name);
                            if (mod.op == ModifierOp::Add) {
                                base += (mod.magnitude * stack_multiplier);
                            } else if (mod.op == ModifierOp::Multiply) {
                                base *= mod.magnitude;
                            } else if (mod.op == ModifierOp::Override) {
                                base = mod.magnitude;
                            }
                            attributes.set_base_value(mod.attribute_name, base);
                        }
                    }
                    active.period_remaining += active.def->period();
                }
            }

            // Duration handling
            if (active.def->duration_policy() == EffectDurationPolicy::Duration) {
                active.duration_remaining -= safe_dt;
                if (active.duration_remaining <= 0.0f) {
                    remove_continuous_modifiers(active, attributes);
                    for (const auto& tag : active.def->granted_tags().all()) {
                        actor_tags.remove(tag);
                    }
                    it = m_active_effects.erase(it);
                    continue;
                }
            }
        }

        ++it;
    }
}

bool ActiveEffectsContainer::has_active_effect(const std::string& effect_name) const {
    for (const auto& active : m_active_effects) {
        if (active.def && active.def->name() == effect_name) {
            return true;
        }
    }
    return false;
}

u32 ActiveEffectsContainer::get_stack_count(const std::string& effect_name) const {
    for (const auto& active : m_active_effects) {
        if (active.def && active.def->name() == effect_name) {
            return active.current_stacks;
        }
    }
    return 0;
}

f32 ActiveEffectsContainer::get_duration_remaining(ActiveEffectHandle handle) const {
    for (const auto& active : m_active_effects) {
        if (active.handle == handle) {
            return active.duration_remaining;
        }
    }
    return 0.0f;
}

void ActiveEffectsContainer::clear(AttributeSet& attributes, TagContainer& actor_tags) {
    for (const auto& active : m_active_effects) {
        if (active.def) {
            for (const auto& mod : active.def->modifiers()) {
                if (!mod.is_periodic) {
                    attributes.remove_modifiers_by_source(mod.attribute_name, active.handle.id);
                }
            }
            for (const auto& tag : active.def->granted_tags().all()) {
                actor_tags.remove(tag);
            }
        }
    }
    m_active_effects.clear();
}

} // namespace nf::gameplay
