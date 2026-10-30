#include <NF/Test/TestFramework.hpp>
#include <NF/AI/Formation.hpp>

#include <cmath>

using namespace nf;
using namespace nf::ai;

NF_TEST(formation_generate_line) {
    FormationGroup group;
    group.generate(FormationType::Line, 5, 2.0f);

    NF_CHECK_EQ(group.slot_count(), 5ULL);
    const auto& slots = group.slots();

    // Slot 0 at apex/leader (0, 0, 0)
    NF_CHECK_NEAR(slots[0].relative_offset.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(slots[0].relative_offset.z, 0.0f, 1e-4f);

    // Slot 1 at +2m X (right)
    NF_CHECK_NEAR(slots[1].relative_offset.x, 2.0f, 1e-4f);
    NF_CHECK_NEAR(slots[1].relative_offset.z, 0.0f, 1e-4f);

    // Slot 2 at -2m X (left)
    NF_CHECK_NEAR(slots[2].relative_offset.x, -2.0f, 1e-4f);

    // Slot 3 at +4m X
    NF_CHECK_NEAR(slots[3].relative_offset.x, 4.0f, 1e-4f);

    // Slot 4 at -4m X
    NF_CHECK_NEAR(slots[4].relative_offset.x, -4.0f, 1e-4f);
}

NF_TEST(formation_generate_column) {
    FormationGroup group;
    group.generate(FormationType::Column, 4, 3.0f);

    NF_CHECK_EQ(group.slot_count(), 4ULL);
    const auto& slots = group.slots();

    NF_CHECK_NEAR(slots[0].relative_offset.z, 0.0f, 1e-4f);
    NF_CHECK_NEAR(slots[1].relative_offset.z, -3.0f, 1e-4f);
    NF_CHECK_NEAR(slots[2].relative_offset.z, -6.0f, 1e-4f);
    NF_CHECK_NEAR(slots[3].relative_offset.z, -9.0f, 1e-4f);
}

NF_TEST(formation_generate_wedge) {
    FormationGroup group;
    group.generate(FormationType::Wedge, 5, 2.0f);

    const auto& slots = group.slots();
    // Apex
    NF_CHECK_NEAR(slots[0].relative_offset.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(slots[0].relative_offset.z, 0.0f, 1e-4f);

    // Rank 1 right: (+2, 0, -2)
    NF_CHECK_NEAR(slots[1].relative_offset.x, 2.0f, 1e-4f);
    NF_CHECK_NEAR(slots[1].relative_offset.z, -2.0f, 1e-4f);

    // Rank 1 left: (-2, 0, -2)
    NF_CHECK_NEAR(slots[2].relative_offset.x, -2.0f, 1e-4f);
    NF_CHECK_NEAR(slots[2].relative_offset.z, -2.0f, 1e-4f);

    // Rank 2 right: (+4, 0, -4)
    NF_CHECK_NEAR(slots[3].relative_offset.x, 4.0f, 1e-4f);
    NF_CHECK_NEAR(slots[3].relative_offset.z, -4.0f, 1e-4f);
}

NF_TEST(formation_generate_circle) {
    FormationGroup group;
    group.generate(FormationType::Circle, 4, 2.0f);

    NF_CHECK_EQ(group.slot_count(), 4ULL);
    const auto& slots = group.slots();

    f32 r = slots[0].relative_offset.length();
    NF_CHECK(r >= 2.0f);

    // All slots should be equidistant from center
    for (usize i = 1; i < 4; ++i) {
        NF_CHECK_NEAR(slots[i].relative_offset.length(), r, 1e-3f);
    }
}

NF_TEST(formation_world_transform_rotation) {
    FormationGroup group;
    group.generate(FormationType::Column, 3, 5.0f);

    // Leader placed at (10, 0, 10) facing East (+X)
    group.set_leader(Vec3{10.0f, 0.0f, 10.0f}, Vec3{1.0f, 0.0f, 0.0f});

    Vec3 p0, f0, p1, f1;
    group.get_slot_world_transform(0, p0, f0);
    group.get_slot_world_transform(1, p1, f1);

    // Slot 0 is at leader position
    NF_CHECK_NEAR(p0.x, 10.0f, 1e-3f);
    NF_CHECK_NEAR(p0.z, 10.0f, 1e-3f);

    // Slot 1 trails behind leader in -X direction by 5m (X = 5, Z = 10)
    NF_CHECK_NEAR(p1.x, 5.0f, 1e-3f);
    NF_CHECK_NEAR(p1.z, 10.0f, 1e-3f);
}

NF_TEST(formation_assign_and_unassign) {
    FormationGroup group;
    group.generate(FormationType::Line, 2, 2.0f);

    NF_CHECK(!group.is_full());
    NF_CHECK_EQ(group.occupied_count(), 0ULL);

    i32 s1 = group.assign_agent(101);
    NF_CHECK_EQ(s1, 0);
    NF_CHECK_EQ(group.occupied_count(), 1ULL);

    i32 s2 = group.assign_agent(102);
    NF_CHECK_EQ(s2, 1);
    NF_CHECK(group.is_full());

    // Already full -> fails with -1
    i32 s3 = group.assign_agent(103);
    NF_CHECK_EQ(s3, -1);

    // Unassign agent 101 -> frees slot 0
    NF_CHECK(group.unassign_agent(101));
    NF_CHECK_EQ(group.occupied_count(), 1ULL);
    NF_CHECK(!group.is_full());

    // Reassign agent 103 -> gets slot 0
    i32 s4 = group.assign_agent(103);
    NF_CHECK_EQ(s4, 0);
}

NF_TEST(formation_assign_nearest) {
    FormationGroup group;
    group.generate(FormationType::Line, 3, 10.0f);
    // Slots: 0 at (0,0,0), 1 at (+10,0,0), 2 at (-10,0,0)
    group.set_leader(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f});

    // Unit 501 is placed far on the left (-9.0, 0, 0) -> should get slot 2!
    i32 slot = group.assign_agent_nearest(501, Vec3{-9.0f, 0.0f, 0.0f});
    NF_CHECK_EQ(slot, 2);

    // Unit 502 is placed on the right (+8.0, 0, 0) -> should get slot 1!
    slot = group.assign_agent_nearest(502, Vec3{8.0f, 0.0f, 0.0f});
    NF_CHECK_EQ(slot, 1);
}

NF_TEST(flocking_separation_pushes_agents_apart) {
    FlockingConfig cfg;
    cfg.separation_radius = 5.0f;
    cfg.separation_weight = 2.0f;
    cfg.alignment_weight = 0.0f;
    cfg.cohesion_weight = 0.0f;
    cfg.goal_weight = 0.0f;

    FlockingSolver solver(cfg);
    // Two agents very close to each other along X
    solver.add_agent(1, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f});
    solver.add_agent(2, Vec3{0.5f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f});

    // Advance 0.5s
    solver.step(0.5f);

    const auto* a1 = solver.get_agent_by_id(1);
    const auto* a2 = solver.get_agent_by_id(2);

    // Agent 1 pushed in -X, Agent 2 pushed in +X -> distance increased
    f32 dist = (a2->position - a1->position).length();
    NF_CHECK(dist > 0.5f);
}

NF_TEST(flocking_alignment_matches_headings) {
    FlockingConfig cfg;
    cfg.separation_weight = 0.0f;
    cfg.cohesion_weight = 0.0f;
    cfg.alignment_radius = 20.0f;
    cfg.alignment_weight = 2.0f;

    FlockingSolver solver(cfg);
    // Agent 1 moving East (+X at 5 m/s)
    solver.add_agent(1, Vec3{0.0f, 0.0f, 0.0f}, Vec3{5.0f, 0.0f, 0.0f});
    // Agent 2 initially stationary at (1, 0, 0)
    solver.add_agent(2, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f});

    // Step simulation
    for (int i = 0; i < 10; ++i) {
        solver.step(0.1f);
    }

    const auto* a2 = solver.get_agent_by_id(2);
    // Agent 2 should have aligned its heading towards +X
    NF_CHECK(a2->velocity.x > 1.0f);
}

NF_TEST(flocking_cohesion_and_goal_convergence) {
    FlockingConfig cfg;
    cfg.cohesion_radius = 50.0f;
    cfg.cohesion_weight = 1.0f;
    cfg.goal_weight = 1.5f;

    FlockingSolver solver(cfg);
    solver.add_agent(1, Vec3{-10.0f, 0.0f, 0.0f});
    solver.add_agent(2, Vec3{10.0f, 0.0f, 0.0f});

    Vec3 goal{0.0f, 0.0f, 50.0f};

    for (int i = 0; i < 20; ++i) {
        solver.step(0.1f, &goal);
    }

    const auto* a1 = solver.get_agent_by_id(1);
    const auto* a2 = solver.get_agent_by_id(2);

    // Both should have progressed towards the goal (+Z > 0)
    NF_CHECK(a1->position.z > 0.0f);
    NF_CHECK(a2->position.z > 0.0f);
}
