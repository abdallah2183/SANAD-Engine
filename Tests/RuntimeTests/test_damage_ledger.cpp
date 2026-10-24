// Tests/RuntimeTests/test_damage_ledger.cpp — Phase 55: who hurt me.
//
// ImpactEvent is frame-local. An NPC that retaliates and a HUD that says
// "last hit by" need memory that survives the frame. These cases pin the
// ledger's totals, quiet clock, killer attribution and refusal of bad numbers.

#include <NF/Test/TestFramework.hpp>
#include <NF/Runtime/DamageLedger.hpp>

#include <cmath>

using namespace nf;
using namespace nf::runtime;

NF_TEST(a_ledger_records_the_last_attacker_and_totals) {
    DamageLedger ledger;
    NF_CHECK(!ledger.has_damage);
    NF_CHECK(!ledger.last_attacker.valid());

    const ecs::Entity attacker{7u, 2u};
    damage_ledger_record(ledger, attacker, 12.0f, 3.0f, false);
    NF_CHECK(ledger.has_damage);
    NF_CHECK(ledger.last_attacker == attacker);
    NF_CHECK_NEAR(ledger.health_damage, 12.0f, 1e-5f);
    NF_CHECK_NEAR(ledger.armor_damage, 3.0f, 1e-5f);
    NF_CHECK_EQ(ledger.hits, 1u);
    NF_CHECK_NEAR(ledger.seconds_since_damage, 0.0f, 1e-5f);

    damage_ledger_record(ledger, ecs::Entity{8u, 0u}, 4.0f, 0.0f, false);
    NF_CHECK(ledger.last_attacker == (ecs::Entity{8u, 0u}));
    NF_CHECK_NEAR(ledger.health_damage, 16.0f, 1e-5f);
    NF_CHECK_EQ(ledger.hits, 2u);
}

NF_TEST(a_killing_hit_names_the_killer_once) {
    DamageLedger ledger;
    const ecs::Entity attacker{9u, 1u};
    damage_ledger_record(ledger, attacker, 50.0f, 0.0f, true);
    NF_CHECK(ledger.killed);
    NF_CHECK(ledger.killer == attacker);

    // A later ledger record cannot un-kill the victim.
    damage_ledger_record(ledger, ecs::Entity{1u, 0u}, 1.0f, 0.0f, false);
    NF_CHECK(ledger.killed);
    NF_CHECK(ledger.killer == attacker);
}

NF_TEST(the_quiet_clock_advances_only_after_damage) {
    DamageLedger ledger;
    damage_ledger_step(ledger, 5.0f);
    NF_CHECK_NEAR(ledger.seconds_since_damage, 0.0f, 1e-5f);

    damage_ledger_record(ledger, ecs::Entity{1u, 0u}, 2.0f, 0.0f, false);
    damage_ledger_step(ledger, 0.25f);
    NF_CHECK_NEAR(ledger.seconds_since_damage, 0.25f, 1e-5f);
    damage_ledger_step(ledger, -1.0f);
    NF_CHECK_NEAR(ledger.seconds_since_damage, 0.25f, 1e-5f);
}

NF_TEST(non_finite_damage_does_not_poison_the_ledger) {
    DamageLedger ledger;
    damage_ledger_record(ledger, ecs::Entity{3u, 0u}, std::nanf(""), -5.0f, false);
    NF_CHECK_NEAR(ledger.health_damage, 0.0f, 1e-5f);
    NF_CHECK_NEAR(ledger.armor_damage, 0.0f, 1e-5f);
    NF_CHECK_EQ(ledger.hits, 1u);
}
