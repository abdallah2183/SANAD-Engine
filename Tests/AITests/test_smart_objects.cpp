#include <NF/Test/TestFramework.hpp>
#include <NF/AI/SmartObject.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::ai;

NF_TEST(smart_object_registration_and_slot_transforms) {
    SmartObjectManager mgr;
    NF_CHECK_EQ(mgr.object_count(), 0u);

    // Bench with two seats
    SmartObjectInstance bench;
    bench.world_position = Vec3{10.0f, 0.0f, 20.0f};
    bench.world_rotation = Quat::identity();
    bench.definition.activity_name = "Sit";

    SmartObjectSlot seat_left;
    seat_left.slot_index = 0;
    seat_left.offset = Vec3{-0.6f, 0.45f, 0.0f};

    SmartObjectSlot seat_right;
    seat_right.slot_index = 1;
    seat_right.offset = Vec3{0.6f, 0.45f, 0.0f};

    bench.slots.push_back(seat_left);
    bench.slots.push_back(seat_right);

    u32 id = mgr.register_smart_object(std::move(bench));
    NF_CHECK(id != 0u);
    NF_CHECK_EQ(mgr.object_count(), 1u);

    const SmartObjectInstance* found = mgr.get_smart_object(id);
    NF_CHECK(found != nullptr);
    NF_CHECK_EQ(found->slots.size(), 2u);

    Vec3 pos_l{0.0f, 0.0f, 0.0f};
    Quat rot_l = Quat::identity();
    NF_CHECK(found->get_slot_transform(0, pos_l, rot_l));
    NF_CHECK_NEAR(pos_l.x, 9.4f, 1e-4f);
    NF_CHECK_NEAR(pos_l.y, 0.45f, 1e-4f);
    NF_CHECK_NEAR(pos_l.z, 20.0f, 1e-4f);

    Vec3 pos_r{0.0f, 0.0f, 0.0f};
    Quat rot_r = Quat::identity();
    NF_CHECK(found->get_slot_transform(1, pos_r, rot_r));
    NF_CHECK_NEAR(pos_r.x, 10.6f, 1e-4f);
}

NF_TEST(smart_object_slot_claims_and_agent_ownership) {
    SmartObjectManager mgr;

    SmartObjectInstance chair;
    chair.definition.activity_name = "Sit";
    SmartObjectSlot s0;
    chair.slots.push_back(s0);
    u32 cid = mgr.register_smart_object(std::move(chair));

    // Agent 10 claims slot 0
    NF_CHECK(mgr.claim_slot(cid, 0, 10));

    u32 claim_obj = 0, claim_slot = 0;
    NF_CHECK(mgr.get_agent_claim(10, claim_obj, claim_slot));
    NF_CHECK_EQ(claim_obj, cid);
    NF_CHECK_EQ(claim_slot, 0u);

    // Agent 20 cannot claim occupied slot
    NF_CHECK(!mgr.claim_slot(cid, 0, 20));

    // Agent 10 re-claiming succeeds
    NF_CHECK(mgr.claim_slot(cid, 0, 10));

    // Release slot
    NF_CHECK(mgr.release_slot(cid, 0));
    NF_CHECK(!mgr.get_agent_claim(10, claim_obj, claim_slot));

    // Now Agent 20 can claim
    NF_CHECK(mgr.claim_slot(cid, 0, 20));

    // Release all for agent
    mgr.release_all_for_agent(20);
    NF_CHECK(!mgr.get_agent_claim(20, claim_obj, claim_slot));
}

NF_TEST(smart_object_query_activity_and_tag_filtering) {
    SmartObjectManager mgr;

    // Forge (requires Role.Blacksmith)
    SmartObjectInstance forge;
    forge.world_position = Vec3{5.0f, 0.0f, 0.0f};
    forge.definition.activity_name = "Craft";
    forge.definition.required_tags.push_back("Role.Blacksmith");
    mgr.register_smart_object(forge);

    // Campfire (no tag requirements)
    SmartObjectInstance campfire;
    campfire.world_position = Vec3{10.0f, 0.0f, 0.0f};
    campfire.definition.activity_name = "Rest";
    mgr.register_smart_object(campfire);

    // 1. Query for Craft without required tag -> Fails
    SmartObjectQuery q1;
    q1.querier_position = Vec3{0.0f, 0.0f, 0.0f};
    q1.activity_name = "Craft";
    q1.actor_tags = {"Role.Civilian"};
    SmartObjectQueryResult r1 = mgr.find_best_smart_object(q1);
    NF_CHECK(!r1.valid);

    // 2. Query for Craft with Role.Blacksmith -> Finds Forge
    q1.actor_tags.push_back("Role.Blacksmith");
    SmartObjectQueryResult r2 = mgr.find_best_smart_object(q1);
    NF_CHECK(r2.valid);
    NF_CHECK_EQ(r2.activity_name, "Craft");

    // 3. Query for Rest -> Finds Campfire
    SmartObjectQuery q2;
    q2.querier_position = Vec3{0.0f, 0.0f, 0.0f};
    q2.activity_name = "Rest";
    SmartObjectQueryResult r3 = mgr.find_best_smart_object(q2);
    NF_CHECK(r3.valid);
    NF_CHECK_EQ(r3.activity_name, "Rest");
}

NF_TEST(smart_object_spatial_scoring_and_priorities) {
    SmartObjectManager mgr;

    // Chair 1: close at x = 10, priority 1.0
    SmartObjectInstance c1;
    c1.world_position = Vec3{10.0f, 0.0f, 0.0f};
    c1.definition.activity_name = "Sit";
    c1.definition.priority = 1.0f;
    u32 id1 = mgr.register_smart_object(c1);

    // Chair 2: farther at x = 30, but royal priority 5.0
    SmartObjectInstance c2;
    c2.world_position = Vec3{30.0f, 0.0f, 0.0f};
    c2.definition.activity_name = "Sit";
    c2.definition.priority = 5.0f;
    u32 id2 = mgr.register_smart_object(c2);

    SmartObjectQuery q;
    q.querier_position = Vec3{0.0f, 0.0f, 0.0f};
    q.activity_name = "Sit";
    q.max_distance = 100.0f;

    // Chair 2 should win due to high priority
    SmartObjectQueryResult best = mgr.find_best_smart_object(q);
    NF_CHECK(best.valid);
    NF_CHECK_EQ(best.object_id, id2);

    // Find all matching returns 2 results in sorted order
    std::vector<SmartObjectQueryResult> results;
    mgr.find_all_matching(q, results);
    NF_CHECK_EQ(results.size(), 2u);
    NF_CHECK_EQ(results[0].object_id, id2);
    NF_CHECK_EQ(results[1].object_id, id1);
}

NF_TEST(smart_object_cooldown_lifecycle) {
    SmartObjectManager mgr;

    SmartObjectInstance obj;
    obj.definition.activity_name = "OperateTerminal";
    obj.definition.cooldown = 4.0f; // 4s cooldown after use
    u32 id = mgr.register_smart_object(obj);

    // 1. Initially available
    NF_CHECK(mgr.get_smart_object(id)->has_available_slot());

    // 2. Claim and release
    NF_CHECK(mgr.claim_slot(id, 0, 100));
    NF_CHECK(mgr.release_slot(id, 0));

    // 3. Immediately on cooldown -> not available
    const SmartObjectInstance* cur = mgr.get_smart_object(id);
    NF_CHECK_NEAR(cur->current_cooldown, 4.0f, 1e-4f);
    NF_CHECK(!cur->has_available_slot());

    // Query also ignores object on cooldown
    SmartObjectQuery q;
    q.querier_position = Vec3{0.0f, 0.0f, 0.0f};
    q.activity_name = "OperateTerminal";
    NF_CHECK(!mgr.find_best_smart_object(q).valid);

    // 4. Update 2.5s -> still on cooldown (1.5s remaining)
    mgr.update(2.5f);
    cur = mgr.get_smart_object(id);
    NF_CHECK_NEAR(cur->current_cooldown, 1.5f, 1e-4f);
    NF_CHECK(!cur->has_available_slot());

    // 5. Update 2.0s -> cooldown expires (0.0s)
    mgr.update(2.0f);
    cur = mgr.get_smart_object(id);
    NF_CHECK_NEAR(cur->current_cooldown, 0.0f, 1e-4f);
    NF_CHECK(cur->has_available_slot());

    // Query now succeeds
    NF_CHECK(mgr.find_best_smart_object(q).valid);
}
