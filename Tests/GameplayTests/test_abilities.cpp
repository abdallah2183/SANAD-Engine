#include <NF/Test/TestFramework.hpp>
#include <NF/Gameplay/GameplayAbility.hpp>

using namespace nf;
using namespace nf::gameplay;

NF_TEST(ability_instant_activation_cost_and_cooldown) {
    AttributeSet attrs;
    attrs.register_attribute("Mana", 100.0f, 0.0f, 100.0f);

    TagContainer tags;
    AbilitySystemComponent comp;

    auto fireball = std::make_shared<GameplayAbility>("Fireball");
    fireball->set_cost("Mana", 25.0f);
    fireball->set_cooldown(2.0f);

    bool executed = false;
    fireball->set_custom_execute([&](AttributeSet&, TagContainer&) {
        executed = true;
    });

    comp.grant_ability(fireball, 0);

    // 1. Initial activation succeeds
    NF_CHECK(comp.can_activate_ability(0, attrs, tags));
    bool ok = comp.try_activate_ability(0, attrs, tags);
    NF_CHECK(ok);
    NF_CHECK(executed);
    NF_CHECK_NEAR(attrs.get_current_value("Mana"), 75.0f, 1e-4f);
    NF_CHECK(comp.is_on_cooldown(0));
    NF_CHECK_NEAR(comp.get_cooldown_remaining(0), 2.0f, 1e-4f);

    // 2. Second activation immediately fails due to cooldown
    executed = false;
    NF_CHECK(!comp.can_activate_ability(0, attrs, tags));
    ok = comp.try_activate_ability(0, attrs, tags);
    NF_CHECK(!ok);
    NF_CHECK(!executed);
    NF_CHECK_NEAR(attrs.get_current_value("Mana"), 75.0f, 1e-4f);

    // 3. Advance time past cooldown
    comp.update(1.0f, attrs, tags);
    NF_CHECK(comp.is_on_cooldown(0));
    comp.update(1.1f, attrs, tags);
    NF_CHECK(!comp.is_on_cooldown(0));

    // 4. Can activate again
    ok = comp.try_activate_ability(0, attrs, tags);
    NF_CHECK(ok);
    NF_CHECK(executed);
    NF_CHECK_NEAR(attrs.get_current_value("Mana"), 50.0f, 1e-4f);
}

NF_TEST(ability_insufficient_resource_fails) {
    AttributeSet attrs;
    attrs.register_attribute("Mana", 10.0f, 0.0f, 100.0f);

    TagContainer tags;
    AbilitySystemComponent comp;

    auto spell = std::make_shared<GameplayAbility>("CostlySpell");
    spell->set_cost("Mana", 30.0f);
    comp.grant_ability(spell, 0);

    NF_CHECK(!comp.can_activate_ability(0, attrs, tags));
    bool ok = comp.try_activate_ability(0, attrs, tags);
    NF_CHECK(!ok);
    NF_CHECK_NEAR(attrs.get_current_value("Mana"), 10.0f, 1e-4f);
}

NF_TEST(ability_tag_requirements_and_blocks) {
    AttributeSet attrs;
    TagContainer tags;
    AbilitySystemComponent comp;

    auto dash = std::make_shared<GameplayAbility>("GroundedDash");
    dash->activation_required_tags().add("state.grounded");
    dash->activation_blocked_tags().add("state.stunned");
    comp.grant_ability(dash, 0);

    // 1. Missing required tag -> fails
    NF_CHECK(!comp.can_activate_ability(0, attrs, tags));

    // 2. Add required tag -> succeeds
    tags.add("state.grounded");
    NF_CHECK(comp.can_activate_ability(0, attrs, tags));

    // 3. Add blocked tag -> fails
    tags.add("state.stunned");
    NF_CHECK(!comp.can_activate_ability(0, attrs, tags));

    // 4. Remove blocked tag -> succeeds again
    tags.remove("state.stunned");
    NF_CHECK(comp.can_activate_ability(0, attrs, tags));
}

NF_TEST(ability_duration_applied_tags_and_end) {
    AttributeSet attrs;
    TagContainer tags;
    AbilitySystemComponent comp;

    auto shield = std::make_shared<GameplayAbility>("MagicShield");
    shield->set_duration(3.0f);
    shield->activation_applied_tags().add("state.shielded");
    comp.grant_ability(shield, 0);

    // 1. Activate duration ability
    bool ok = comp.try_activate_ability(0, attrs, tags);
    NF_CHECK(ok);

    auto* spec = comp.get_spec(0);
    NF_CHECK(spec != nullptr);
    NF_CHECK(spec->is_active);
    NF_CHECK(tags.has("state.shielded"));

    // 2. Tick halfway through
    comp.update(1.5f, attrs, tags);
    NF_CHECK(spec->is_active);
    NF_CHECK(tags.has("state.shielded"));

    // 3. Tick to completion (total 3.1s >= 3.0s)
    comp.update(1.6f, attrs, tags);
    NF_CHECK(!spec->is_active);
    NF_CHECK(!tags.has("state.shielded"));
}

NF_TEST(ability_cancel_tags_preemption) {
    AttributeSet attrs;
    TagContainer tags;
    AbilitySystemComponent comp;

    // Ability 0: Long Channel
    auto channel = std::make_shared<GameplayAbility>("LongChannel");
    channel->set_duration(5.0f);
    channel->ability_tags().add("ability.channeled");
    channel->activation_applied_tags().add("state.channeling");
    comp.grant_ability(channel, 0);

    // Ability 1: Interrupting Dash
    auto dash = std::make_shared<GameplayAbility>("InterruptDash");
    dash->cancel_abilities_with_tags().add("ability.channeled");
    comp.grant_ability(dash, 1);

    // 1. Activate Channel
    comp.try_activate_ability(0, attrs, tags);
    NF_CHECK(comp.get_spec(0)->is_active);
    NF_CHECK(tags.has("state.channeling"));

    // 2. Activate Dash -> cancels Channel immediately!
    comp.try_activate_ability(1, attrs, tags);
    NF_CHECK(!comp.get_spec(0)->is_active);
    NF_CHECK(!tags.has("state.channeling"));
}
