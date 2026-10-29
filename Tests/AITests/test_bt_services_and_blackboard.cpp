#include <NF/Test/TestFramework.hpp>
#include <NF/AI/BehaviorTree.hpp>
#include <cmath>

using namespace nf;
using namespace nf::ai;

NF_TEST(bt_blackboard_vectors_and_entities) {
    Blackboard bb;
    bb.set_vector("target_pos", Vec3(12.0f, 34.0f, 56.0f));
    bb.set_entity("target_actor", 1042);

    NF_CHECK(bb.has_vector("target_pos"));
    NF_CHECK(bb.has_entity("target_actor"));
    NF_CHECK(!bb.has_vector("non_existent"));

    Vec3 pos = bb.get_vector("target_pos");
    NF_CHECK(pos.nearly_equals(Vec3(12.0f, 34.0f, 56.0f), 0.001f));
    NF_CHECK_EQ(bb.get_entity("target_actor"), 1042u);

    // Unset vector
    bb.unset("target_pos");
    NF_CHECK(!bb.has_vector("target_pos"));
    NF_CHECK(!bb.has("target_pos"));
}

NF_TEST(bt_blackboard_parent_delegation) {
    Blackboard squad_bb;
    squad_bb.set_flag("squad_alert", true);
    squad_bb.set_vector("rally_point", Vec3(100.0f, 0.0f, 50.0f));
    squad_bb.set_number("threat_level", 4.0);

    Blackboard soldier_bb;
    soldier_bb.set_parent(&squad_bb);
    soldier_bb.set_number("ammo", 25.0);

    // Soldier inherits squad variables
    NF_CHECK(soldier_bb.has("squad_alert"));
    NF_CHECK(soldier_bb.get_flag("squad_alert"));
    NF_CHECK(soldier_bb.get_vector("rally_point").nearly_equals(Vec3(100.0f, 0.0f, 50.0f), 0.001f));
    NF_CHECK_EQ(soldier_bb.get_number("threat_level"), 4.0);
    NF_CHECK_EQ(soldier_bb.get_number("ammo"), 25.0);

    // Local override on soldier
    soldier_bb.set_flag("squad_alert", false);
    NF_CHECK(!soldier_bb.get_flag("squad_alert"));
    // Squad's original value remains true
    NF_CHECK(squad_bb.get_flag("squad_alert"));
}

NF_TEST(bt_blackboard_observers) {
    Blackboard bb;
    int threat_changed_count = 0;
    int any_changed_count = 0;

    u32 h1 = bb.register_observer("threat", [&](const std::string&) {
        threat_changed_count++;
    });

    u32 h2 = bb.register_observer("", [&](const std::string&) {
        any_changed_count++;
    });

    bb.set_number("threat", 1.0);
    NF_CHECK_EQ(threat_changed_count, 1);
    NF_CHECK_EQ(any_changed_count, 1);

    bb.set_flag("patrol_mode", true);
    NF_CHECK_EQ(threat_changed_count, 1);
    NF_CHECK_EQ(any_changed_count, 2);

    bb.unregister_observer(h1);
    bb.set_number("threat", 2.0);
    NF_CHECK_EQ(threat_changed_count, 1); // Unregistered, not incremented
    NF_CHECK_EQ(any_changed_count, 3);

    bb.unregister_observer(h2);
}

NF_TEST(bt_service_periodic_update) {
    Blackboard bb;
    bb.set_number("service_ticks", 0.0);

    auto action_node = std::make_unique<BTAction>([](Blackboard&, float) {
        return BTStatus::Running;
    });

    auto service_node = std::make_unique<BTServiceNode>(std::move(action_node));
    // Service ticks every 0.5s
    service_node->add_service(std::make_unique<BTLambdaService>(0.5f, [](Blackboard& b, float) {
        b.set_number("service_ticks", b.get_number("service_ticks") + 1.0);
    }));

    // First tick with 0.2s: service shouldn't fire yet
    BTStatus s1 = service_node->tick(bb, 0.2f);
    NF_CHECK(s1 == BTStatus::Running);
    NF_CHECK_EQ(bb.get_number("service_ticks"), 0.0);

    // Second tick with 0.35s (total 0.55s): service fires once
    BTStatus s2 = service_node->tick(bb, 0.35f);
    NF_CHECK(s2 == BTStatus::Running);
    NF_CHECK_EQ(bb.get_number("service_ticks"), 1.0);

    // Third tick with 0.6s: service fires again
    service_node->tick(bb, 0.6f);
    NF_CHECK_EQ(bb.get_number("service_ticks"), 2.0);
}

NF_TEST(bt_blackboard_decorator_conditions) {
    Blackboard bb;
    bb.set_flag("has_ammo", true);
    bb.set_number("health", 75.0);
    bb.set_vector("enemy_pos", Vec3(5.0f, 0.0f, 0.0f));

    // Test Bool condition
    auto action = std::make_unique<BTAction>([](Blackboard&, float) { return BTStatus::Success; });
    BTBlackboardDecorator dec_ammo(std::move(action), BBCondition::bool_equals("has_ammo", true));
    NF_CHECK(dec_ammo.tick(bb, 0.1f) == BTStatus::Success);

    bb.set_flag("has_ammo", false);
    NF_CHECK(dec_ammo.tick(bb, 0.1f) == BTStatus::Failure);

    // Test Distance condition (reference point (0,0,0), distance <= 10.0m)
    auto action2 = std::make_unique<BTAction>([](Blackboard&, float) { return BTStatus::Success; });
    BTBlackboardDecorator dec_dist(std::move(action2), BBCondition::distance_compare("enemy_pos", BBConditionOp::DistanceLess, Vec3(0,0,0), 10.0f));
    NF_CHECK(dec_dist.tick(bb, 0.1f) == BTStatus::Success);

    // Move enemy beyond 10m
    bb.set_vector("enemy_pos", Vec3(15.0f, 0.0f, 0.0f));
    NF_CHECK(dec_dist.tick(bb, 0.1f) == BTStatus::Failure);
}

NF_TEST(bt_builder_service_and_blackboard_decorator) {
    Blackboard bb;
    bb.set_flag("combat_active", true);
    bb.set_number("radar_pings", 0.0);

    BTBuilder builder;
    auto tree = builder
        .sequence()
            .service(0.25f, [](Blackboard& b, float) {
                b.set_number("radar_pings", b.get_number("radar_pings") + 1.0);
            })
            .blackboard_check(BBCondition::bool_equals("combat_active", true))
                .action([](Blackboard&, float) { return BTStatus::Success; })
            .end()
        .end()
        .build();

    NF_CHECK(tree != nullptr);

    // Tick tree
    BTStatus s = tree->tick(bb, 0.3f);
    NF_CHECK(s == BTStatus::Success);
    NF_CHECK_EQ(bb.get_number("radar_pings"), 1.0);
}
