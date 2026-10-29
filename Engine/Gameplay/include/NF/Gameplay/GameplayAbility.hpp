#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Gameplay/GameplayAttributes.hpp>
#include <NF/Gameplay/Tags.hpp>

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

namespace nf::gameplay {

class AbilitySystemComponent;

/// Gameplay Ability Definition
/// Encapsulates activation conditions, resource costs, cooldown, tags, and execution lifecycle.
class GameplayAbility {
public:
    explicit GameplayAbility(std::string name = "Ability");
    virtual ~GameplayAbility() = default;

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void set_name(std::string name) { m_name = std::move(name); }

    // Resource Cost
    void set_cost(std::string attribute_name, f32 cost) {
        m_cost_attribute = std::move(attribute_name);
        m_cost_amount = std::max(0.0f, cost);
    }
    [[nodiscard]] const std::string& cost_attribute() const noexcept { return m_cost_attribute; }
    [[nodiscard]] f32 cost_amount() const noexcept { return m_cost_amount; }

    // Cooldown
    void set_cooldown(f32 duration_seconds) { m_cooldown_seconds = std::max(0.0f, duration_seconds); }
    [[nodiscard]] f32 cooldown_duration() const noexcept { return m_cooldown_seconds; }

    // Duration (0 for instant execution)
    void set_duration(f32 duration_seconds) { m_duration_seconds = std::max(0.0f, duration_seconds); }
    [[nodiscard]] f32 duration() const noexcept { return m_duration_seconds; }
    [[nodiscard]] bool is_instant() const noexcept { return m_duration_seconds <= 0.0f; }

    // Tags
    TagContainer& ability_tags() noexcept { return m_ability_tags; }
    [[nodiscard]] const TagContainer& ability_tags() const noexcept { return m_ability_tags; }

    TagContainer& activation_required_tags() noexcept { return m_required_tags; }
    [[nodiscard]] const TagContainer& activation_required_tags() const noexcept { return m_required_tags; }

    TagContainer& activation_blocked_tags() noexcept { return m_blocked_tags; }
    [[nodiscard]] const TagContainer& activation_blocked_tags() const noexcept { return m_blocked_tags; }

    TagContainer& activation_applied_tags() noexcept { return m_applied_tags; }
    [[nodiscard]] const TagContainer& activation_applied_tags() const noexcept { return m_applied_tags; }

    TagContainer& cancel_abilities_with_tags() noexcept { return m_cancel_tags; }
    [[nodiscard]] const TagContainer& cancel_abilities_with_tags() const noexcept { return m_cancel_tags; }

    // Core validation
    virtual bool can_activate(const AttributeSet& attributes, const TagContainer& actor_tags) const;

    // Deducts cost from attributes
    virtual bool commit_ability(AttributeSet& attributes, TagContainer& actor_tags);

    // Lifecycle callbacks
    virtual void on_activate(AttributeSet& attributes, TagContainer& actor_tags);
    virtual void on_tick(f32 dt, AttributeSet& attributes, TagContainer& actor_tags);
    virtual void on_end(AttributeSet& attributes, TagContainer& actor_tags);

    // Custom execution delegate
    using ExecuteCallback = std::function<void(AttributeSet&, TagContainer&)>;
    void set_custom_execute(ExecuteCallback cb) { m_custom_execute = std::move(cb); }

private:
    std::string m_name;
    std::string m_cost_attribute;
    f32 m_cost_amount = 0.0f;
    f32 m_cooldown_seconds = 0.0f;
    f32 m_duration_seconds = 0.0f;

    TagContainer m_ability_tags;
    TagContainer m_required_tags;
    TagContainer m_blocked_tags;
    TagContainer m_applied_tags;
    TagContainer m_cancel_tags;

    ExecuteCallback m_custom_execute;
};

/// Granted ability specification in an ability slot
struct AbilitySpec {
    std::shared_ptr<GameplayAbility> ability;
    u32 slot_index = 0;
    u32 level = 1;
    f32 cooldown_remaining = 0.0f;
    f32 active_elapsed = 0.0f;
    bool is_active = false;
};

/// Ability System Component / Manager
class AbilitySystemComponent {
public:
    AbilitySystemComponent() = default;

    /// Grant an ability to a specific slot
    void grant_ability(std::shared_ptr<GameplayAbility> ability, u32 slot_index, u32 level = 1);

    /// Revoke ability from slot
    void revoke_ability_by_slot(u32 slot_index);

    /// Revoke ability by name
    void revoke_ability_by_name(const std::string& name);

    [[nodiscard]] bool has_ability_in_slot(u32 slot_index) const;
    [[nodiscard]] bool has_ability_named(const std::string& name) const;

    [[nodiscard]] AbilitySpec* get_spec(u32 slot_index);
    [[nodiscard]] const AbilitySpec* get_spec(u32 slot_index) const;

    [[nodiscard]] AbilitySpec* get_spec_by_name(const std::string& name);
    [[nodiscard]] const AbilitySpec* get_spec_by_name(const std::string& name) const;

    /// Check if ability can be activated
    [[nodiscard]] bool can_activate_ability(u32 slot_index, const AttributeSet& attributes, const TagContainer& actor_tags) const;

    /// Try to activate ability in slot
    bool try_activate_ability(u32 slot_index, AttributeSet& attributes, TagContainer& actor_tags);

    /// Try to activate ability by name
    bool try_activate_ability_by_name(const std::string& name, AttributeSet& attributes, TagContainer& actor_tags);

    /// End an active ability manually
    void end_ability(u32 slot_index, AttributeSet& attributes, TagContainer& actor_tags);

    /// Cancel all currently active abilities that match any cancel tag
    void cancel_abilities_with_tag(const std::string& tag, AttributeSet& attributes, TagContainer& actor_tags);

    /// Update tick: decrements cooldowns, advances active abilities, ends expired ones
    void update(f32 dt, AttributeSet& attributes, TagContainer& actor_tags);

    [[nodiscard]] bool is_on_cooldown(u32 slot_index) const;
    [[nodiscard]] f32 get_cooldown_remaining(u32 slot_index) const;

    [[nodiscard]] size_t granted_ability_count() const noexcept { return m_abilities.size(); }

    /// Clear all abilities and reset cooldowns
    void clear();

private:
    std::unordered_map<u32, AbilitySpec> m_abilities;
};

} // namespace nf::gameplay
