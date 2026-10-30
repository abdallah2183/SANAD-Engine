#include <NF/Gameplay/GameplayExecution.hpp>

namespace nf::gameplay {

// ----------------------------------------------------------------------------
// DamageExecutionCalculation
// ----------------------------------------------------------------------------

DamageExecutionCalculation::DamageExecutionCalculation(f32 armor_scaling)
    : m_armor_scaling(std::max(1.0f, armor_scaling)) {}

ExecutionOutput DamageExecutionCalculation::execute(const ExecutionParams& params) const {
    ExecutionOutput out;

    // 1. Gather offensive attributes
    f32 attack_power = 0.0f;
    if (params.source_attributes.has_attribute("AttackPower")) {
        attack_power = params.source_attributes.get_current_value("AttackPower");
    } else if (params.source_attributes.has_attribute("PhysicalDamage")) {
        attack_power = params.source_attributes.get_current_value("PhysicalDamage");
    }

    f32 raw_damage = std::max(0.0f, params.base_value + attack_power);
    out.calculated_raw_damage = raw_damage;

    // 2. Critical hit evaluation
    f32 crit_rate = 0.0f;
    if (params.source_attributes.has_attribute("CritRate")) {
        crit_rate = params.source_attributes.get_current_value("CritRate");
    }

    f32 crit_multiplier = 1.5f;
    if (params.source_attributes.has_attribute("CritMultiplier")) {
        crit_multiplier = std::max(1.0f, params.source_attributes.get_current_value("CritMultiplier"));
    }

    if (crit_rate > 0.0f && (params.roll < crit_rate || crit_rate >= 1.0f)) {
        out.is_critical = true;
        raw_damage *= crit_multiplier;
        out.calculated_raw_damage = raw_damage;
    }

    // 3. Defensive mitigation (diminishing returns formula: armor / (armor + scaling))
    f32 armor = 0.0f;
    if (params.target_attributes.has_attribute("Armor")) {
        armor = params.target_attributes.get_current_value("Armor");
    } else if (params.target_attributes.has_attribute("PhysicalDefense")) {
        armor = params.target_attributes.get_current_value("PhysicalDefense");
    }
    armor = std::max(0.0f, armor);

    f32 mitigation_ratio = 0.0f;
    if (armor > 0.0f) {
        mitigation_ratio = std::clamp(armor / (armor + m_armor_scaling), 0.0f, 0.95f);
        out.is_mitigated = true;
    }
    out.calculated_mitigation_ratio = mitigation_ratio;

    f32 effective_damage = raw_damage * (1.0f - mitigation_ratio);
    out.calculated_effective_damage = effective_damage;

    // 4. Target health reduction modifier
    out.modifiers.push_back(ExecutionMod{
        "Health",
        ModifierOp::Add,
        -effective_damage,
        false // apply to target
    });

    // 5. Life steal to source
    f32 lifesteal = 0.0f;
    if (params.source_attributes.has_attribute("LifeSteal")) {
        lifesteal = std::clamp(params.source_attributes.get_current_value("LifeSteal"), 0.0f, 1.0f);
    }
    if (lifesteal > 0.0f && effective_damage > 0.0f) {
        const f32 heal_amount = effective_damage * lifesteal;
        out.calculated_lifesteal_heal = heal_amount;
        out.modifiers.push_back(ExecutionMod{
            "Health",
            ModifierOp::Add,
            heal_amount,
            true // apply to source
        });
    }

    // 6. Gameplay Cue triggering
    out.cue_tag = out.is_critical ? "GameplayCue.Combat.Hit.Critical" : "GameplayCue.Combat.Hit";
    out.cue_params.raw_magnitude = effective_damage;
    out.cue_params.normalized_magnitude = out.is_critical ? 1.0f : (effective_damage > 50.0f ? 0.7f : 0.4f);
    out.cue_params.instigator_id = params.source_id;
    out.cue_params.target_id = params.target_id;

    return out;
}

// ----------------------------------------------------------------------------
// ElementalDamageExecutionCalculation
// ----------------------------------------------------------------------------

ElementalDamageExecutionCalculation::ElementalDamageExecutionCalculation(std::string element)
    : m_element(std::move(element)) {}

ExecutionOutput ElementalDamageExecutionCalculation::execute(const ExecutionParams& params) const {
    ExecutionOutput out;

    // 1. Gather elemental power
    f32 power = 0.0f;
    const std::string element_power_key = m_element + "Power";
    if (params.source_attributes.has_attribute(element_power_key)) {
        power = params.source_attributes.get_current_value(element_power_key);
    } else if (params.source_attributes.has_attribute("MagicPower")) {
        power = params.source_attributes.get_current_value("MagicPower");
    }

    const f32 raw_damage = std::max(0.0f, params.base_value + power);
    out.calculated_raw_damage = raw_damage;

    // 2. Resistance calculation (negative = vulnerability, positive = resistance up to 95%)
    f32 resistance = 0.0f;
    const std::string element_res_key = m_element + "Resistance";
    if (params.target_attributes.has_attribute(element_res_key)) {
        resistance = std::clamp(params.target_attributes.get_current_value(element_res_key), -1.0f, 0.95f);
    }

    out.calculated_mitigation_ratio = resistance;
    out.is_mitigated = (resistance != 0.0f);

    const f32 effective_damage = raw_damage * (1.0f - resistance);
    out.calculated_effective_damage = effective_damage;

    // 3. Target health reduction modifier
    out.modifiers.push_back(ExecutionMod{
        "Health",
        ModifierOp::Add,
        -effective_damage,
        false
    });

    // 4. Gameplay Cue
    out.cue_tag = "GameplayCue.Combat.Elemental." + m_element;
    out.cue_params.raw_magnitude = effective_damage;
    out.cue_params.normalized_magnitude = std::clamp(effective_damage / 100.0f, 0.0f, 1.0f);
    out.cue_params.instigator_id = params.source_id;
    out.cue_params.target_id = params.target_id;

    return out;
}

// ----------------------------------------------------------------------------
// HealingExecutionCalculation
// ----------------------------------------------------------------------------

ExecutionOutput HealingExecutionCalculation::execute(const ExecutionParams& params) const {
    ExecutionOutput out;

    // 1. Gather heal power
    f32 heal_power = 0.0f;
    if (params.source_attributes.has_attribute("HealPower")) {
        heal_power = params.source_attributes.get_current_value("HealPower");
    } else if (params.source_attributes.has_attribute("Wisdom")) {
        heal_power = params.source_attributes.get_current_value("Wisdom");
    }

    const f32 raw_heal = std::max(0.0f, params.base_value + heal_power);

    // 2. Target healing received multiplier
    f32 healing_multiplier = 1.0f;
    if (params.target_attributes.has_attribute("HealingReceived")) {
        healing_multiplier = std::max(0.0f, params.target_attributes.get_current_value("HealingReceived"));
    }

    const f32 effective_heal = raw_heal * healing_multiplier;
    out.calculated_effective_damage = effective_heal; // stored in effective field

    // 3. Target health restore modifier
    out.modifiers.push_back(ExecutionMod{
        "Health",
        ModifierOp::Add,
        effective_heal,
        false
    });

    // 4. Gameplay Cue
    out.cue_tag = "GameplayCue.Heal";
    out.cue_params.raw_magnitude = effective_heal;
    out.cue_params.normalized_magnitude = std::clamp(effective_heal / 100.0f, 0.0f, 1.0f);
    out.cue_params.instigator_id = params.source_id;
    out.cue_params.target_id = params.target_id;

    return out;
}

} // namespace nf::gameplay
