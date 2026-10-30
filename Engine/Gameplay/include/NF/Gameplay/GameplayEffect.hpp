#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Gameplay/GameplayAttributes.hpp>
#include <NF/Gameplay/Tags.hpp>

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>

namespace nf::gameplay {

enum class EffectDurationPolicy : u8 {
    Instant = 0,
    Duration,
    Infinite
};

enum class EffectStackingPolicy : u8 {
    None = 0,       // Subsequent applications refresh duration but do not stack
    StackCount,     // Adds stacks up to max_stacks; multipliers scale by stack count
    StackDuration   // Adds new duration to existing duration
};

struct EffectModifierDef {
    std::string attribute_name;
    ModifierOp op = ModifierOp::Add;
    f32 magnitude = 0.0f;
    bool is_periodic = false; // True = applied to base value on period tick; False = continuous while active
};

struct ActiveEffectHandle {
    u64 id = 0;
    constexpr bool valid() const noexcept { return id != 0; }
    constexpr bool operator==(const ActiveEffectHandle& o) const noexcept { return id == o.id; }
    constexpr bool operator!=(const ActiveEffectHandle& o) const noexcept { return id != o.id; }
};

/// Definition of a gameplay effect (buff, debuff, damage over time, or instant modification)
class GameplayEffectDef {
public:
    explicit GameplayEffectDef(std::string name = "Effect");
    ~GameplayEffectDef() = default;

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void set_name(std::string name) { m_name = std::move(name); }

    void set_duration_policy(EffectDurationPolicy policy, f32 duration_seconds = 0.0f) {
        m_duration_policy = policy;
        m_duration = std::max(0.0f, duration_seconds);
    }
    [[nodiscard]] EffectDurationPolicy duration_policy() const noexcept { return m_duration_policy; }
    [[nodiscard]] f32 duration() const noexcept { return m_duration; }

    void set_period(f32 period_seconds) { m_period = std::max(0.0f, period_seconds); }
    [[nodiscard]] f32 period() const noexcept { return m_period; }
    [[nodiscard]] bool is_periodic() const noexcept { return m_period > 0.0f; }

    void set_stacking(EffectStackingPolicy policy, u32 max_stacks = 1, bool refresh_on_stack = true) {
        m_stacking_policy = policy;
        m_max_stacks = std::max(1u, max_stacks);
        m_refresh_on_stack = refresh_on_stack;
    }
    [[nodiscard]] EffectStackingPolicy stacking_policy() const noexcept { return m_stacking_policy; }
    [[nodiscard]] u32 max_stacks() const noexcept { return m_max_stacks; }
    [[nodiscard]] bool refresh_on_stack() const noexcept { return m_refresh_on_stack; }

    void add_modifier(std::string attribute_name, ModifierOp op, f32 magnitude, bool is_periodic = false) {
        m_modifiers.push_back({std::move(attribute_name), op, magnitude, is_periodic});
    }
    [[nodiscard]] const std::vector<EffectModifierDef>& modifiers() const noexcept { return m_modifiers; }

    TagContainer& granted_tags() noexcept { return m_granted_tags; }
    [[nodiscard]] const TagContainer& granted_tags() const noexcept { return m_granted_tags; }

    TagContainer& ongoing_required_tags() noexcept { return m_ongoing_required_tags; }
    [[nodiscard]] const TagContainer& ongoing_required_tags() const noexcept { return m_ongoing_required_tags; }

    TagContainer& ongoing_blocked_tags() noexcept { return m_ongoing_blocked_tags; }
    [[nodiscard]] const TagContainer& ongoing_blocked_tags() const noexcept { return m_ongoing_blocked_tags; }

    TagContainer& cleanse_tags() noexcept { return m_cleanse_tags; }
    [[nodiscard]] const TagContainer& cleanse_tags() const noexcept { return m_cleanse_tags; }

    // Fluent helper factories
    static std::shared_ptr<GameplayEffectDef> make_instant(std::string name, std::string attribute, f32 delta);
    static std::shared_ptr<GameplayEffectDef> make_buff(std::string name, f32 duration, std::string attribute, ModifierOp op, f32 magnitude);
    static std::shared_ptr<GameplayEffectDef> make_dot(std::string name, f32 duration, f32 period, std::string attribute, f32 tick_damage, u32 max_stacks = 1);

private:
    std::string m_name;
    EffectDurationPolicy m_duration_policy = EffectDurationPolicy::Instant;
    f32 m_duration = 0.0f;
    f32 m_period = 0.0f;
    EffectStackingPolicy m_stacking_policy = EffectStackingPolicy::None;
    u32 m_max_stacks = 1;
    bool m_refresh_on_stack = true;

    std::vector<EffectModifierDef> m_modifiers;
    TagContainer m_granted_tags;
    TagContainer m_ongoing_required_tags;
    TagContainer m_ongoing_blocked_tags;
    TagContainer m_cleanse_tags;
};

/// Live instance of an effect executing on an actor
struct ActiveGameplayEffect {
    ActiveEffectHandle handle;
    std::shared_ptr<const GameplayEffectDef> def;
    f32 duration_remaining = 0.0f;
    f32 period_remaining = 0.0f;
    u32 current_stacks = 1;
    u64 source_id = 0;
    bool is_inhibited = false;
};

/// Container managing active duration and periodic effects ticking on an actor
class ActiveEffectsContainer {
public:
    ActiveEffectsContainer() = default;
    ~ActiveEffectsContainer() = default;

    /// Apply an effect to the actor
    ActiveEffectHandle apply_effect(const std::shared_ptr<const GameplayEffectDef>& effect_def,
                                    AttributeSet& attributes, TagContainer& actor_tags, u64 source_id = 0);

    /// Manually remove an active effect by handle
    bool remove_effect(ActiveEffectHandle handle, AttributeSet& attributes, TagContainer& actor_tags);

    /// Cleanses/removes all active effects matching tags in cleanse_tags
    size_t remove_effects_with_tags(const TagContainer& tags, AttributeSet& attributes, TagContainer& actor_tags);

    /// Update tick: decrements durations, fires periodic ticks, updates inhibitions, purges expired
    void update(f32 dt, AttributeSet& attributes, TagContainer& actor_tags);

    [[nodiscard]] size_t active_effect_count() const noexcept { return m_active_effects.size(); }
    [[nodiscard]] bool has_active_effect(const std::string& effect_name) const;
    [[nodiscard]] u32 get_stack_count(const std::string& effect_name) const;
    [[nodiscard]] f32 get_duration_remaining(ActiveEffectHandle handle) const;

    /// Clears all active effects, revoking modifiers and tags
    void clear(AttributeSet& attributes, TagContainer& actor_tags);

private:
    void apply_continuous_modifiers(const ActiveGameplayEffect& active, AttributeSet& attributes);
    void remove_continuous_modifiers(const ActiveGameplayEffect& active, AttributeSet& attributes);
    void check_inhibition(ActiveGameplayEffect& active, TagContainer& actor_tags, AttributeSet& attributes);

    std::vector<ActiveGameplayEffect> m_active_effects;
    u64 m_next_handle_id = 1;
};

} // namespace nf::gameplay
