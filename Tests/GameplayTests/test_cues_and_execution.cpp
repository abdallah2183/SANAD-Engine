#include <NF/Test/TestFramework.hpp>
#include <NF/Gameplay/GameplayCue.hpp>
#include <NF/Gameplay/GameplayExecution.hpp>
#include <NF/Gameplay/GameplayAbility.hpp>

#include <cmath>

using namespace nf;
using namespace nf::gameplay;

NF_TEST(cues_registration_and_hierarchical_dispatch) {
    GameplayCueManager cue_mgr;

    int exact_hits = 0;
    int parent_hits = 0;
    int root_hits = 0;

    cue_mgr.register_handler("GameplayCue.Combat.Hit.Fire", [&](GameplayCueType type, const GameplayCueParameters& params) {
        NF_CHECK_EQ(type, GameplayCueType::Executed);
        NF_CHECK_NEAR(params.raw_magnitude, 45.0f, 1e-4f);
        exact_hits++;
    });

    cue_mgr.register_handler("GameplayCue.Combat.Hit", [&](GameplayCueType type, const GameplayCueParameters& /*params*/) {
        NF_CHECK_EQ(type, GameplayCueType::Executed);
        parent_hits++;
    });

    cue_mgr.register_handler("GameplayCue.Combat", [&](GameplayCueType type, const GameplayCueParameters& /*params*/) {
        NF_CHECK_EQ(type, GameplayCueType::Executed);
        root_hits++;
    });

    NF_CHECK_EQ(cue_mgr.handler_count(), 3u);
    NF_CHECK(cue_mgr.has_handler_for_tag("GameplayCue.Combat.Hit.Fire"));
    NF_CHECK(cue_mgr.has_handler_for_tag("GameplayCue.Combat.Hit"));
    NF_CHECK(cue_mgr.has_handler_for_tag("GameplayCue.Combat"));
    NF_CHECK(!cue_mgr.has_handler_for_tag("GameplayCue.Magic"));

    GameplayCueParameters params;
    params.raw_magnitude = 45.0f;
    params.instigator_id = 101;
    params.target_id = 202;

    cue_mgr.execute_cue("GameplayCue.Combat.Hit.Fire", params);

    NF_CHECK_EQ(exact_hits, 1);
    NF_CHECK_EQ(parent_hits, 1);
    NF_CHECK_EQ(root_hits, 1);

    // Unregister parent handler
    cue_mgr.unregister_handler("GameplayCue.Combat.Hit");
    NF_CHECK_EQ(cue_mgr.handler_count(), 2u);

    cue_mgr.execute_cue("GameplayCue.Combat.Hit.Fire", params);
    NF_CHECK_EQ(exact_hits, 2);
    NF_CHECK_EQ(parent_hits, 1); // unchanged
    NF_CHECK_EQ(root_hits, 2);
}

NF_TEST(cues_lifecycle_events_and_parameters) {
    GameplayCueManager cue_mgr;
    cue_mgr.set_recording_enabled(true);
    NF_CHECK(cue_mgr.is_recording_enabled());

    GameplayCueParameters params;
    params.location = Vec3{10.0f, 2.0f, -5.0f};
    params.normal = Vec3{0.0f, 1.0f, 0.0f};
    params.raw_magnitude = 100.0f;
    params.instigator_id = 42;
    params.target_id = 99;

    cue_mgr.add_cue("GameplayCue.Status.Frozen", params);
    cue_mgr.update_cue("GameplayCue.Status.Frozen", params);
    cue_mgr.remove_cue("GameplayCue.Status.Frozen", params);

    const auto& recorded = cue_mgr.recorded_events();
    NF_CHECK_EQ(recorded.size(), 3u);

    NF_CHECK_EQ(recorded[0].type, GameplayCueType::OnActive);
    NF_CHECK_EQ(recorded[0].tag, "GameplayCue.Status.Frozen");
    NF_CHECK_NEAR(recorded[0].params.location.x, 10.0f, 1e-4f);

    NF_CHECK_EQ(recorded[1].type, GameplayCueType::WhileActive);
    NF_CHECK_EQ(recorded[2].type, GameplayCueType::Removed);

    cue_mgr.clear_recorded_events();
    NF_CHECK_EQ(cue_mgr.recorded_events().size(), 0u);
}

NF_TEST(damage_execution_armor_mitigation_formula) {
    AttributeSet attacker_attrs;
    attacker_attrs.register_attribute("AttackPower", 100.0f, 0.0f, 1000.0f);

    AttributeSet defender_attrs;
    defender_attrs.register_attribute("Health", 200.0f, 0.0f, 200.0f);
    defender_attrs.register_attribute("Armor", 100.0f, 0.0f, 1000.0f);

    TagContainer dummy_tags;
    DamageExecutionCalculation calc(100.0f); // armor_scaling = 100

    ExecutionParams params{
        attacker_attrs,
        defender_attrs,
        dummy_tags,
        dummy_tags,
        50.0f, // base_value
        1,     // source_id
        2,     // target_id
        0.5f   // roll
    };

    ExecutionOutput out = calc.execute(params);

    // raw_damage = 50 base + 100 attack = 150
    NF_CHECK_NEAR(out.calculated_raw_damage, 150.0f, 1e-4f);
    NF_CHECK(!out.is_critical);
    NF_CHECK(out.is_mitigated);

    // mitigation_ratio = 100 / (100 + 100) = 0.5 (50%)
    NF_CHECK_NEAR(out.calculated_mitigation_ratio, 0.5f, 1e-4f);

    // effective_damage = 150 * (1 - 0.5) = 75
    NF_CHECK_NEAR(out.calculated_effective_damage, 75.0f, 1e-4f);
    NF_CHECK_EQ(out.modifiers.size(), 1u);
    NF_CHECK_EQ(out.modifiers[0].attribute_name, "Health");
    NF_CHECK_NEAR(out.modifiers[0].magnitude, -75.0f, 1e-4f);
    NF_CHECK_EQ(out.modifiers[0].apply_to_source, false);

    NF_CHECK_EQ(out.cue_tag, "GameplayCue.Combat.Hit");
    NF_CHECK_NEAR(out.cue_params.raw_magnitude, 75.0f, 1e-4f);
}

NF_TEST(damage_execution_critical_strike_and_lifesteal) {
    AttributeSet attacker_attrs;
    attacker_attrs.register_attribute("AttackPower", 50.0f, 0.0f, 1000.0f);
    attacker_attrs.register_attribute("CritRate", 0.50f, 0.0f, 1.0f);
    attacker_attrs.register_attribute("CritMultiplier", 2.0f, 1.0f, 10.0f);
    attacker_attrs.register_attribute("LifeSteal", 0.20f, 0.0f, 1.0f); // 20% LifeSteal

    AttributeSet defender_attrs;
    defender_attrs.register_attribute("Health", 300.0f, 0.0f, 300.0f);
    defender_attrs.register_attribute("Armor", 0.0f, 0.0f, 1000.0f); // 0 Armor

    TagContainer dummy_tags;
    DamageExecutionCalculation calc;

    // Roll is 0.25 < CritRate 0.50 -> Critical Hit triggers!
    ExecutionParams params{
        attacker_attrs,
        defender_attrs,
        dummy_tags,
        dummy_tags,
        50.0f, // base_value
        10,
        20,
        0.25f // roll
    };

    ExecutionOutput out = calc.execute(params);

    NF_CHECK(out.is_critical);
    // raw = (50 + 50) * 2.0 = 200
    NF_CHECK_NEAR(out.calculated_raw_damage, 200.0f, 1e-4f);
    NF_CHECK_NEAR(out.calculated_effective_damage, 200.0f, 1e-4f);

    // LifeSteal heal = 200 * 0.20 = 40
    NF_CHECK_NEAR(out.calculated_lifesteal_heal, 40.0f, 1e-4f);

    NF_CHECK_EQ(out.modifiers.size(), 2u);
    // Modifier 1: -200 to target Health
    NF_CHECK_EQ(out.modifiers[0].attribute_name, "Health");
    NF_CHECK_NEAR(out.modifiers[0].magnitude, -200.0f, 1e-4f);
    NF_CHECK_EQ(out.modifiers[0].apply_to_source, false);

    // Modifier 2: +40 to source Health
    NF_CHECK_EQ(out.modifiers[1].attribute_name, "Health");
    NF_CHECK_NEAR(out.modifiers[1].magnitude, 40.0f, 1e-4f);
    NF_CHECK_EQ(out.modifiers[1].apply_to_source, true);

    NF_CHECK_EQ(out.cue_tag, "GameplayCue.Combat.Hit.Critical");
    NF_CHECK_NEAR(out.cue_params.raw_magnitude, 200.0f, 1e-4f);
}

NF_TEST(elemental_execution_resistance_and_vulnerability) {
    AttributeSet attacker_attrs;
    attacker_attrs.register_attribute("FirePower", 80.0f, 0.0f, 500.0f);

    AttributeSet defender_attrs;
    defender_attrs.register_attribute("Health", 200.0f, 0.0f, 200.0f);
    defender_attrs.register_attribute("FireResistance", 0.40f, -1.0f, 0.95f); // 40% Fire Resistance

    TagContainer dummy_tags;
    ElementalDamageExecutionCalculation fire_calc("Fire");

    // Case 1: 40% Resistance
    ExecutionParams params{
        attacker_attrs,
        defender_attrs,
        dummy_tags,
        dummy_tags,
        20.0f, // base
        1,
        2,
        0.0f
    };
    // raw = 20 + 80 = 100; effective = 100 * (1 - 0.40) = 60
    ExecutionOutput out1 = fire_calc.execute(params);
    NF_CHECK_NEAR(out1.calculated_effective_damage, 60.0f, 1e-4f);
    NF_CHECK_EQ(out1.cue_tag, "GameplayCue.Combat.Elemental.Fire");

    // Case 2: 50% Fire Vulnerability (-0.50 resistance)
    defender_attrs.set_base_value("FireResistance", -0.50f);
    ExecutionOutput out2 = fire_calc.execute(params);
    // raw = 100; effective = 100 * (1 - (-0.50)) = 150!
    NF_CHECK_NEAR(out2.calculated_effective_damage, 150.0f, 1e-4f);
}

NF_TEST(healing_execution_and_healing_received) {
    AttributeSet caster_attrs;
    caster_attrs.register_attribute("HealPower", 50.0f, 0.0f, 500.0f);

    AttributeSet target_attrs;
    target_attrs.register_attribute("Health", 20.0f, 0.0f, 200.0f);
    target_attrs.register_attribute("HealingReceived", 1.20f, 0.0f, 5.0f); // +20% heal buff

    TagContainer dummy_tags;
    HealingExecutionCalculation heal_calc;

    ExecutionParams params{
        caster_attrs,
        target_attrs,
        dummy_tags,
        dummy_tags,
        50.0f, // base heal
        1,
        2,
        0.0f
    };

    // raw = 50 + 50 = 100; effective = 100 * 1.20 = 120
    ExecutionOutput out = heal_calc.execute(params);
    NF_CHECK_NEAR(out.calculated_effective_damage, 120.0f, 1e-4f);
    NF_CHECK_EQ(out.modifiers.size(), 1u);
    NF_CHECK_EQ(out.modifiers[0].attribute_name, "Health");
    NF_CHECK_NEAR(out.modifiers[0].magnitude, 120.0f, 1e-4f);
    NF_CHECK_EQ(out.cue_tag, "GameplayCue.Heal");
}

NF_TEST(asc_full_combat_and_tag_listeners_integration) {
    AbilitySystemComponent attacker_asc;
    AbilitySystemComponent defender_asc;

    auto cue_mgr = std::make_shared<GameplayCueManager>();
    cue_mgr->set_recording_enabled(true);
    attacker_asc.set_cue_manager(cue_mgr);

    AttributeSet attacker_attrs;
    attacker_attrs.register_attribute("Health", 80.0f, 0.0f, 100.0f);
    attacker_attrs.register_attribute("AttackPower", 60.0f, 0.0f, 500.0f);
    attacker_attrs.register_attribute("LifeSteal", 0.25f, 0.0f, 1.0f); // 25% LifeSteal

    AttributeSet defender_attrs;
    defender_attrs.register_attribute("Health", 150.0f, 0.0f, 150.0f);
    defender_attrs.register_attribute("Armor", 50.0f, 0.0f, 500.0f); // 50 / (50 + 100) = 33.33% mitigation

    TagContainer attacker_tags;
    TagContainer defender_tags;

    // Register tag listener on attacker for hierarchical "Combat" tags
    int tag_added_count = 0;
    int tag_removed_count = 0;
    attacker_asc.register_tag_listener("Combat", [&](const std::string& /*tag*/, bool added) {
        if (added) tag_added_count++;
        else tag_removed_count++;
    });

    // Grant ability that applies "Combat.Attacking"
    auto ability = std::make_shared<GameplayAbility>("PowerStrike");
    ability->set_duration(1.0f);
    ability->activation_applied_tags().add("Combat.Attacking");
    attacker_asc.grant_ability(ability, 0);

    // Activate ability
    bool activated = attacker_asc.try_activate_ability(0, attacker_attrs, attacker_tags);
    NF_CHECK(activated);
    NF_CHECK(attacker_tags.has("Combat.Attacking"));
    NF_CHECK_EQ(tag_added_count, 1);

    // Execute combat damage calculation
    DamageExecutionCalculation combat_calc(100.0f);
    ExecutionOutput result = attacker_asc.execute_calculation(
        combat_calc,
        defender_asc,
        attacker_attrs,
        defender_attrs,
        attacker_tags,
        defender_tags,
        40.0f, // base_damage (total raw = 40 + 60 = 100)
        0.0f,  // no crit
        1,
        2
    );

    // Raw = 100; mitigation = 50 / 150 = 1/3; effective = 100 * (2/3) = 66.6667
    NF_CHECK_NEAR(result.calculated_effective_damage, 66.6667f, 1e-3f);

    // Defender health was 150 -> now 150 - 66.6667 = 83.3333
    NF_CHECK_NEAR(defender_attrs.get_base_value("Health"), 83.3333f, 1e-3f);

    // Attacker health was 80 -> lifesteal heals 66.6667 * 0.25 = 16.6667 -> 80 + 16.6667 = 96.6667
    NF_CHECK_NEAR(attacker_attrs.get_base_value("Health"), 96.6667f, 1e-3f);

    // Cue was recorded by cue_mgr
    NF_CHECK_EQ(cue_mgr->recorded_events().size(), 1u);
    NF_CHECK_EQ(cue_mgr->recorded_events()[0].tag, "GameplayCue.Combat.Hit");

    // End ability and verify tag removed notification
    attacker_asc.end_ability(0, attacker_attrs, attacker_tags);
    NF_CHECK(!attacker_tags.has("Combat.Attacking"));
    NF_CHECK_EQ(tag_removed_count, 1);
}
