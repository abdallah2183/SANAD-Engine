#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Gameplay/Tags.hpp>

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <algorithm>

namespace nf::gameplay {

enum class ModifierOp : u8 {
    Add = 0,
    Multiply,
    Override
};

struct AttributeModifier {
    ModifierOp op = ModifierOp::Add;
    float value = 0.0f;
    uint64_t source_id = 0;
};

struct GameplayAttribute {
    std::string name;
    float base_value = 0.0f;
    float current_value = 0.0f;
    float min_value = -1e30f;
    float max_value = 1e30f;
    std::vector<AttributeModifier> modifiers;

    void recalculate();
};

/// Extensible gameplay attribute set (Health, Stamina, Attack, Speed, etc.)
/// with support for additive, multiplicative, and overriding modifiers.
class AttributeSet {
public:
    using ChangeCallback = std::function<void(const std::string& name, float old_val, float new_val)>;

    AttributeSet() = default;

    /// Registers a new attribute with base value and clamp range.
    bool register_attribute(const std::string& name, float base_val, float min_val = -1e30f, float max_val = 1e30f);

    /// Checks if attribute is registered.
    bool has_attribute(const std::string& name) const;

    /// Returns current modified value.
    float get_current_value(const std::string& name, float fallback = 0.0f) const;

    /// Returns base unmodified value.
    float get_base_value(const std::string& name, float fallback = 0.0f) const;

    /// Modifies base value directly (e.g. permanent leveling or base stat purchase).
    bool set_base_value(const std::string& name, float new_base);

    /// Applies a modifier.
    bool add_modifier(const std::string& name, const AttributeModifier& mod);

    /// Removes all modifiers matching source_id.
    bool remove_modifiers_by_source(const std::string& name, uint64_t source_id);

    /// Subscribes to value change notifications.
    void on_change(const std::string& name, ChangeCallback cb);

    /// Lists all registered attribute names in sorted order.
    std::vector<std::string> attribute_names() const;

private:
    void notify_change(const std::string& name, float old_val, float new_val);

    std::unordered_map<std::string, GameplayAttribute> m_attributes;
    std::unordered_map<std::string, std::vector<ChangeCallback>> m_callbacks;
};

enum class EffectDurationType : u8 {
    Instant = 0,
    Duration,
    Periodic
};

struct GameplayEffect {
    std::string name;
    EffectDurationType duration_type = EffectDurationType::Instant;
    float duration = 0.0f;
    float period = 1.0f;
    float time_remaining = 0.0f;
    float time_to_next_tick = 0.0f;
    uint64_t source_id = 0;

    TagContainer tags;
    std::vector<std::pair<std::string, AttributeModifier>> modifiers;
    std::vector<std::pair<std::string, float>> instant_deltas;
};

/// Manages active duration buffs and periodic effects ticking on an AttributeSet.
class EffectManager {
public:
    explicit EffectManager(AttributeSet& attributes) : m_attributes(attributes) {}

    void apply_effect(GameplayEffect effect);
    void update(float dt);
    void remove_effects_by_source(uint64_t source_id);
    void clear();

    size_t active_effect_count() const { return m_active_effects.size(); }

private:
    AttributeSet& m_attributes;
    std::vector<GameplayEffect> m_active_effects;
};

} // namespace nf::gameplay
