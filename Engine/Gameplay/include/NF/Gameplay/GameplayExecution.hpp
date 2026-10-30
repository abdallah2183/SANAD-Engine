#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Gameplay/GameplayAttributes.hpp>
#include <NF/Gameplay/GameplayCue.hpp>
#include <NF/Gameplay/Tags.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace nf::gameplay {

/// Inputs to an execution calculation
struct ExecutionParams {
    const AttributeSet& source_attributes;
    const AttributeSet& target_attributes;
    const TagContainer& source_tags;
    const TagContainer& target_tags;
    f32 base_value = 0.0f;
    u64 source_id = 0;
    u64 target_id = 0;
    f32 roll = 0.0f; // Normalized [0, 1] roll for critical strikes or deterministic tests
};

/// Single attribute modification emitted by an execution
struct ExecutionMod {
    std::string attribute_name;
    ModifierOp op = ModifierOp::Add;
    f32 magnitude = 0.0f;
    bool apply_to_source = false; // true = apply to source, false = apply to target
};

/// Resulting outputs calculated by the execution calculation
struct ExecutionOutput {
    std::vector<ExecutionMod> modifiers;
    bool is_critical = false;
    bool is_mitigated = false;
    f32 calculated_raw_damage = 0.0f;
    f32 calculated_effective_damage = 0.0f;
    f32 calculated_mitigation_ratio = 0.0f;
    f32 calculated_lifesteal_heal = 0.0f;

    // Gameplay cue triggered by this execution
    std::string cue_tag;
    GameplayCueParameters cue_params;
};

/// Base class for complex combat, damage, healing, and attribute calculations
class GameplayExecutionCalculation {
public:
    virtual ~GameplayExecutionCalculation() = default;
    [[nodiscard]] virtual ExecutionOutput execute(const ExecutionParams& params) const = 0;
};

/// Standard Physical / Direct Combat Damage Calculation
/// Formulas:
///   raw_damage = base_value + source["AttackPower"] (or "PhysicalDamage")
///   if roll < source["CritRate"]:
///       raw_damage *= source["CritMultiplier"] (defaults to 1.5 if not found)
///       is_critical = true
///   mitigation_ratio = clamp(target["Armor"] / (target["Armor"] + armor_scaling_constant), 0.0f, 0.95f)
///   effective_damage = raw_damage * (1.0f - mitigation_ratio)
///   target["Health"] -= effective_damage
///   if source["LifeSteal"] > 0:
///       source["Health"] += effective_damage * source["LifeSteal"]
class DamageExecutionCalculation : public GameplayExecutionCalculation {
public:
    explicit DamageExecutionCalculation(f32 armor_scaling = 100.0f);

    [[nodiscard]] ExecutionOutput execute(const ExecutionParams& params) const override;

    void set_armor_scaling(f32 scaling) noexcept { m_armor_scaling = std::max(1.0f, scaling); }
    [[nodiscard]] f32 armor_scaling() const noexcept { return m_armor_scaling; }

private:
    f32 m_armor_scaling = 100.0f;
};

/// Elemental / Magic Damage Calculation
/// Formulas:
///   raw_damage = base_value + source[element + "Power"] (or "MagicPower")
///   res = clamp(target[element + "Resistance"], -1.0f, 0.95f)
///   effective_damage = raw_damage * (1.0f - res)
///   target["Health"] -= effective_damage
class ElementalDamageExecutionCalculation : public GameplayExecutionCalculation {
public:
    explicit ElementalDamageExecutionCalculation(std::string element = "Fire");

    [[nodiscard]] ExecutionOutput execute(const ExecutionParams& params) const override;

    [[nodiscard]] const std::string& element() const noexcept { return m_element; }
    void set_element(std::string element) { m_element = std::move(element); }

private:
    std::string m_element;
};

/// Healing and Recovery Calculation
/// Formulas:
///   heal = (base_value + source["HealPower"]) * max(0.0f, target["HealingReceived"])
///   target["Health"] += heal
class HealingExecutionCalculation : public GameplayExecutionCalculation {
public:
    HealingExecutionCalculation() = default;

    [[nodiscard]] ExecutionOutput execute(const ExecutionParams& params) const override;
};

} // namespace nf::gameplay
