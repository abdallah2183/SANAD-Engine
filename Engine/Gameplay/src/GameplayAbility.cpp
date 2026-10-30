#include <NF/Gameplay/GameplayAbility.hpp>

namespace nf::gameplay {

GameplayAbility::GameplayAbility(std::string name)
    : m_name(std::move(name)) {}

bool GameplayAbility::can_activate(const AttributeSet& attributes, const TagContainer& actor_tags) const {
    // 1. Check required tags
    for (const auto& tag : m_required_tags.all()) {
        if (!actor_tags.has(tag)) {
            return false;
        }
    }

    // 2. Check blocked tags
    for (const auto& tag : m_blocked_tags.all()) {
        if (actor_tags.has(tag)) {
            return false;
        }
    }

    // 3. Check resource cost
    if (!m_cost_attribute.empty() && m_cost_amount > 0.0f) {
        if (!attributes.has_attribute(m_cost_attribute)) {
            return false;
        }
        if (attributes.get_current_value(m_cost_attribute) < m_cost_amount) {
            return false;
        }
    }

    return true;
}

bool GameplayAbility::commit_ability(AttributeSet& attributes, TagContainer& actor_tags) {
    if (!m_cost_attribute.empty() && m_cost_amount > 0.0f) {
        if (!attributes.has_attribute(m_cost_attribute)) {
            return false;
        }
        const f32 cur = attributes.get_current_value(m_cost_attribute);
        if (cur < m_cost_amount) {
            return false;
        }
        attributes.set_base_value(m_cost_attribute, cur - m_cost_amount);
    }

    // Apply activation tags to actor
    for (const auto& tag : m_applied_tags.all()) {
        actor_tags.add(tag);
    }

    return true;
}

void GameplayAbility::on_activate(AttributeSet& attributes, TagContainer& actor_tags) {
    if (m_custom_execute) {
        m_custom_execute(attributes, actor_tags);
    }
}

void GameplayAbility::on_tick(f32 /*dt*/, AttributeSet& /*attributes*/, TagContainer& /*actor_tags*/) {
    // Default no-op
}

void GameplayAbility::on_end(AttributeSet& /*attributes*/, TagContainer& actor_tags) {
    // Remove applied tags
    for (const auto& tag : m_applied_tags.all()) {
        actor_tags.remove(tag);
    }
}

// ----------------------------------------------------------------------------
// AbilitySystemComponent
// ----------------------------------------------------------------------------

void AbilitySystemComponent::grant_ability(std::shared_ptr<GameplayAbility> ability, u32 slot_index, u32 level) {
    if (!ability) {
        return;
    }
    AbilitySpec spec;
    spec.ability = std::move(ability);
    spec.slot_index = slot_index;
    spec.level = std::max(1u, level);
    spec.cooldown_remaining = 0.0f;
    spec.active_elapsed = 0.0f;
    spec.is_active = false;
    m_abilities[slot_index] = std::move(spec);
}

void AbilitySystemComponent::revoke_ability_by_slot(u32 slot_index) {
    m_abilities.erase(slot_index);
}

void AbilitySystemComponent::revoke_ability_by_name(const std::string& name) {
    for (auto it = m_abilities.begin(); it != m_abilities.end(); ++it) {
        if (it->second.ability && it->second.ability->name() == name) {
            m_abilities.erase(it);
            return;
        }
    }
}

bool AbilitySystemComponent::has_ability_in_slot(u32 slot_index) const {
    return m_abilities.find(slot_index) != m_abilities.end();
}

bool AbilitySystemComponent::has_ability_named(const std::string& name) const {
    return get_spec_by_name(name) != nullptr;
}

AbilitySpec* AbilitySystemComponent::get_spec(u32 slot_index) {
    auto it = m_abilities.find(slot_index);
    return (it != m_abilities.end()) ? &it->second : nullptr;
}

const AbilitySpec* AbilitySystemComponent::get_spec(u32 slot_index) const {
    auto it = m_abilities.find(slot_index);
    return (it != m_abilities.end()) ? &it->second : nullptr;
}

AbilitySpec* AbilitySystemComponent::get_spec_by_name(const std::string& name) {
    for (auto& pair : m_abilities) {
        if (pair.second.ability && pair.second.ability->name() == name) {
            return &pair.second;
        }
    }
    return nullptr;
}

const AbilitySpec* AbilitySystemComponent::get_spec_by_name(const std::string& name) const {
    for (const auto& pair : m_abilities) {
        if (pair.second.ability && pair.second.ability->name() == name) {
            return &pair.second;
        }
    }
    return nullptr;
}

bool AbilitySystemComponent::can_activate_ability(u32 slot_index, const AttributeSet& attributes, const TagContainer& actor_tags) const {
    const AbilitySpec* spec = get_spec(slot_index);
    if (!spec || !spec->ability) {
        return false;
    }
    if (spec->cooldown_remaining > 0.0f || spec->is_active) {
        return false;
    }
    return spec->ability->can_activate(attributes, actor_tags);
}

bool AbilitySystemComponent::try_activate_ability(u32 slot_index, AttributeSet& attributes, TagContainer& actor_tags) {
    AbilitySpec* spec = get_spec(slot_index);
    if (!spec || !spec->ability) {
        return false;
    }
    if (spec->cooldown_remaining > 0.0f || spec->is_active) {
        return false;
    }
    if (!spec->ability->can_activate(attributes, actor_tags)) {
        return false;
    }

    // Cancel conflicting abilities if cancel tags are set
    for (const auto& cancel_tag : spec->ability->cancel_abilities_with_tags().all()) {
        cancel_abilities_with_tag(cancel_tag, attributes, actor_tags);
    }

    if (!spec->ability->commit_ability(attributes, actor_tags)) {
        return false;
    }

    // Start cooldown
    spec->cooldown_remaining = spec->ability->cooldown_duration();

    if (spec->ability->is_instant()) {
        spec->ability->on_activate(attributes, actor_tags);
        spec->ability->on_end(attributes, actor_tags);
        spec->is_active = false;
        spec->active_elapsed = 0.0f;
    } else {
        spec->is_active = true;
        spec->active_elapsed = 0.0f;
        spec->ability->on_activate(attributes, actor_tags);
    }

    return true;
}

bool AbilitySystemComponent::try_activate_ability_by_name(const std::string& name, AttributeSet& attributes, TagContainer& actor_tags) {
    AbilitySpec* spec = get_spec_by_name(name);
    if (!spec) {
        return false;
    }
    return try_activate_ability(spec->slot_index, attributes, actor_tags);
}

void AbilitySystemComponent::end_ability(u32 slot_index, AttributeSet& attributes, TagContainer& actor_tags) {
    AbilitySpec* spec = get_spec(slot_index);
    if (!spec || !spec->ability || !spec->is_active) {
        return;
    }
    spec->ability->on_end(attributes, actor_tags);
    spec->is_active = false;
    spec->active_elapsed = 0.0f;
}

void AbilitySystemComponent::cancel_abilities_with_tag(const std::string& tag, AttributeSet& attributes, TagContainer& actor_tags) {
    for (auto& pair : m_abilities) {
        AbilitySpec& spec = pair.second;
        if (spec.is_active && spec.ability && spec.ability->ability_tags().has(tag)) {
            end_ability(spec.slot_index, attributes, actor_tags);
        }
    }
}

void AbilitySystemComponent::update(f32 dt, AttributeSet& attributes, TagContainer& actor_tags) {
    // 1. Update active abilities
    for (auto& pair : m_abilities) {
        AbilitySpec& spec = pair.second;

        // Decrement cooldown
        if (spec.cooldown_remaining > 0.0f) {
            spec.cooldown_remaining = std::max(0.0f, spec.cooldown_remaining - dt);
        }

        // Advance active ability
        if (spec.is_active && spec.ability) {
            spec.active_elapsed += dt;
            spec.ability->on_tick(dt, attributes, actor_tags);

            if (spec.active_elapsed >= spec.ability->duration()) {
                end_ability(spec.slot_index, attributes, actor_tags);
            }
        }
    }

    // 2. Update active gameplay effects
    m_effects.update(dt, attributes, actor_tags);
}

ActiveEffectHandle AbilitySystemComponent::apply_effect(
    const std::shared_ptr<const GameplayEffectDef>& effect,
    AttributeSet& attributes,
    TagContainer& actor_tags,
    u64 source_id)
{
    return m_effects.apply_effect(effect, attributes, actor_tags, source_id);
}

bool AbilitySystemComponent::remove_effect(ActiveEffectHandle handle, AttributeSet& attributes, TagContainer& actor_tags) {
    return m_effects.remove_effect(handle, attributes, actor_tags);
}

size_t AbilitySystemComponent::remove_effects_with_tags(const TagContainer& tags, AttributeSet& attributes, TagContainer& actor_tags) {
    return m_effects.remove_effects_with_tags(tags, attributes, actor_tags);
}

bool AbilitySystemComponent::has_active_effect(const std::string& effect_name) const {
    return m_effects.has_active_effect(effect_name);
}

u32 AbilitySystemComponent::get_effect_stack_count(const std::string& effect_name) const {
    return m_effects.get_stack_count(effect_name);
}

size_t AbilitySystemComponent::active_effect_count() const noexcept {
    return m_effects.active_effect_count();
}

bool AbilitySystemComponent::is_on_cooldown(u32 slot_index) const {
    const AbilitySpec* spec = get_spec(slot_index);
    return spec ? (spec->cooldown_remaining > 0.0f) : false;
}

f32 AbilitySystemComponent::get_cooldown_remaining(u32 slot_index) const {
    const AbilitySpec* spec = get_spec(slot_index);
    return spec ? spec->cooldown_remaining : 0.0f;
}

void AbilitySystemComponent::clear() {
    m_abilities.clear();
}

} // namespace nf::gameplay
