#include <NF/Test/TestFramework.hpp>
#include <NF/Gameplay/GameplayEffect.hpp>
#include <NF/Gameplay/GameplayAbility.hpp>
#include <cmath>

using namespace nf;
using namespace nf::gameplay;

NF_TEST(effect_instant_damage_and_heal) {
    AttributeSet attrs;
    attrs.register_attribute("Health", 100.0f, 0.0f, 100.0f);
    TagContainer tags;
    ActiveEffectsContainer container;

    // Instant Damage: -35
    auto dmg = GameplayEffectDef::make_instant("InstantDamage", "Health", -35.0f);
    container.apply_effect(dmg, attrs, tags);
    NF_CHECK_NEAR(attrs.get_base_value("Health"), 65.0f, 1e-4f);
    NF_CHECK_EQ(container.active_effect_count(), 0u); // Instant effects aren't retained

    // Instant Heal: +20
    auto heal = GameplayEffectDef::make_instant("InstantHeal", "Health", 20.0f);
    container.apply_effect(heal, attrs, tags);
    NF_CHECK_NEAR(attrs.get_base_value("Health"), 85.0f, 1e-4f);
}

NF_TEST(effect_duration_buff_with_attribute_modifier) {
    AttributeSet attrs;
    attrs.register_attribute("Attack", 50.0f, 0.0f, 200.0f);
    TagContainer tags;
    ActiveEffectsContainer container;

    auto buff = GameplayEffectDef::make_buff("Enrage", 2.0f, "Attack", ModifierOp::Add, 30.0f);
    buff->granted_tags().add("Buff.Enrage");

    auto handle = container.apply_effect(buff, attrs, tags);
    NF_CHECK(handle.valid());
    NF_CHECK_EQ(container.active_effect_count(), 1u);
    NF_CHECK(tags.has("Buff.Enrage"));
    NF_CHECK_NEAR(attrs.get_current_value("Attack"), 80.0f, 1e-4f);

    // Advance 1.0s: still active
    container.update(1.0f, attrs, tags);
    NF_CHECK_EQ(container.active_effect_count(), 1u);
    NF_CHECK(tags.has("Buff.Enrage"));
    NF_CHECK_NEAR(attrs.get_current_value("Attack"), 80.0f, 1e-4f);

    // Advance 1.1s: expires!
    container.update(1.1f, attrs, tags);
    NF_CHECK_EQ(container.active_effect_count(), 0u);
    NF_CHECK(!tags.has("Buff.Enrage"));
    NF_CHECK_NEAR(attrs.get_current_value("Attack"), 50.0f, 1e-4f);
}

NF_TEST(effect_stacking_count_and_magnitude_multiplication) {
    AttributeSet attrs;
    attrs.register_attribute("Defense", 100.0f, 0.0f, 200.0f);
    TagContainer tags;
    ActiveEffectsContainer container;

    auto armor_shred = std::make_shared<GameplayEffectDef>("ArmorShred");
    armor_shred->set_duration_policy(EffectDurationPolicy::Duration, 3.0f);
    armor_shred->set_stacking(EffectStackingPolicy::StackCount, 3);
    armor_shred->add_modifier("Defense", ModifierOp::Add, -15.0f, false);

    // Stack 1: -15
    container.apply_effect(armor_shred, attrs, tags);
    NF_CHECK_EQ(container.get_stack_count("ArmorShred"), 1u);
    NF_CHECK_NEAR(attrs.get_current_value("Defense"), 85.0f, 1e-4f);

    // Stack 2: -30
    container.apply_effect(armor_shred, attrs, tags);
    NF_CHECK_EQ(container.get_stack_count("ArmorShred"), 2u);
    NF_CHECK_NEAR(attrs.get_current_value("Defense"), 70.0f, 1e-4f);

    // Stack 3: -45
    container.apply_effect(armor_shred, attrs, tags);
    NF_CHECK_EQ(container.get_stack_count("ArmorShred"), 3u);
    NF_CHECK_NEAR(attrs.get_current_value("Defense"), 55.0f, 1e-4f);

    // Stack 4: clamped at 3 stacks
    container.apply_effect(armor_shred, attrs, tags);
    NF_CHECK_EQ(container.get_stack_count("ArmorShred"), 3u);
    NF_CHECK_NEAR(attrs.get_current_value("Defense"), 55.0f, 1e-4f);
}

NF_TEST(effect_periodic_damage_over_time_dot) {
    AttributeSet attrs;
    attrs.register_attribute("Health", 100.0f, 0.0f, 100.0f);
    TagContainer tags;
    ActiveEffectsContainer container;

    // Poison DoT: 3.0s duration, 1.0s period, -10 HP per tick
    auto poison = GameplayEffectDef::make_dot("Poison", 3.0f, 1.0f, "Health", -10.0f);
    container.apply_effect(poison, attrs, tags);
    NF_CHECK_EQ(container.active_effect_count(), 1u);
    NF_CHECK_NEAR(attrs.get_base_value("Health"), 100.0f, 1e-4f);

    // 0.5s: no tick yet
    container.update(0.5f, attrs, tags);
    NF_CHECK_NEAR(attrs.get_base_value("Health"), 100.0f, 1e-4f);

    // 0.6s (total 1.1s): 1st tick fires
    container.update(0.6f, attrs, tags);
    NF_CHECK_NEAR(attrs.get_base_value("Health"), 90.0f, 1e-4f);

    // 1.0s (total 2.1s): 2nd tick fires
    container.update(1.0f, attrs, tags);
    NF_CHECK_NEAR(attrs.get_base_value("Health"), 80.0f, 1e-4f);

    // 1.0s (total 3.1s): 3rd tick fires and expires
    container.update(1.0f, attrs, tags);
    NF_CHECK_NEAR(attrs.get_base_value("Health"), 70.0f, 1e-4f);
    NF_CHECK_EQ(container.active_effect_count(), 0u);
}

NF_TEST(effect_tag_inhibition_and_cleansing) {
    AttributeSet attrs;
    attrs.register_attribute("MoveSpeed", 100.0f, 0.0f, 200.0f);
    TagContainer tags;
    ActiveEffectsContainer container;

    // Slow effect: -40 MoveSpeed while active; blocked by Immunity.Slow
    auto slow = GameplayEffectDef::make_buff("Slow", 5.0f, "MoveSpeed", ModifierOp::Add, -40.0f);
    slow->granted_tags().add("Debuff.Slow");
    slow->ongoing_blocked_tags().add("Immunity.Slow");

    container.apply_effect(slow, attrs, tags);
    NF_CHECK(tags.has("Debuff.Slow"));
    NF_CHECK_NEAR(attrs.get_current_value("MoveSpeed"), 60.0f, 1e-4f);

    // Gain Immunity -> effect inhibited
    tags.add("Immunity.Slow");
    container.update(0.1f, attrs, tags);
    NF_CHECK(!tags.has("Debuff.Slow"));
    NF_CHECK_NEAR(attrs.get_current_value("MoveSpeed"), 100.0f, 1e-4f);

    // Cleanse test: CureSpell cleanses Debuff.Slow
    auto cure = std::make_shared<GameplayEffectDef>("CureSpell");
    cure->cleanse_tags().add("Debuff.Slow");
    container.apply_effect(cure, attrs, tags);
    NF_CHECK_EQ(container.active_effect_count(), 0u); // Slow completely wiped
}

NF_TEST(asc_gameplay_effects_integration) {
    AbilitySystemComponent asc;
    AttributeSet attrs;
    attrs.register_attribute("Health", 100.0f, 0.0f, 100.0f);
    attrs.register_attribute("Shield", 0.0f, 0.0f, 100.0f);
    TagContainer tags;

    // Apply shield buff via ASC
    auto shield = GameplayEffectDef::make_buff("DivineShield", 4.0f, "Shield", ModifierOp::Add, 50.0f);
    asc.apply_effect(shield, attrs, tags);

    NF_CHECK(asc.has_active_effect("DivineShield"));
    NF_CHECK_EQ(asc.active_effect_count(), 1u);
    NF_CHECK_NEAR(attrs.get_current_value("Shield"), 50.0f, 1e-4f);

    // ASC update ticks effects alongside abilities
    asc.update(2.0f, attrs, tags);
    NF_CHECK(asc.has_active_effect("DivineShield"));

    asc.update(2.1f, attrs, tags);
    NF_CHECK(!asc.has_active_effect("DivineShield"));
    NF_CHECK_NEAR(attrs.get_current_value("Shield"), 0.0f, 1e-4f);
}
