#include <NF/Test/TestFramework.hpp>
#include <NF/AI/CoverSystem.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::ai;

NF_TEST(cover_point_registration_and_lookup) {
    CoverSystem cs;
    NF_CHECK_EQ(cs.cover_point_count(), 0u);

    CoverPoint pt1;
    pt1.position = Vec3{10.0f, 0.0f, 0.0f};
    pt1.normal = Vec3{0.0f, 0.0f, 2.0f}; // Should be normalized to (0, 0, 1)
    pt1.posture = CoverPosture::Low;
    pt1.peek_directions = CoverDirection::Over | CoverDirection::Left;

    u32 id1 = cs.register_cover_point(pt1);
    NF_CHECK(id1 != 0u);
    NF_CHECK_EQ(cs.cover_point_count(), 1u);

    const CoverPoint* found1 = cs.get_cover_point(id1);
    NF_CHECK(found1 != nullptr);
    NF_CHECK_NEAR(found1->normal.z, 1.0f, 1e-4f);
    NF_CHECK_EQ(found1->posture, CoverPosture::Low);
    NF_CHECK((found1->peek_directions & CoverDirection::Over) != 0);
    NF_CHECK((found1->peek_directions & CoverDirection::Left) != 0);

    // Register with explicit ID
    CoverPoint pt2;
    pt2.id = 50;
    pt2.position = Vec3{20.0f, 0.0f, 0.0f};
    pt2.normal = Vec3{1.0f, 0.0f, 0.0f};
    pt2.posture = CoverPosture::High;
    u32 id2 = cs.register_cover_point(pt2);
    NF_CHECK_EQ(id2, 50u);
    NF_CHECK_EQ(cs.cover_point_count(), 2u);

    // Unregister
    NF_CHECK(cs.unregister_cover_point(id1));
    NF_CHECK_EQ(cs.cover_point_count(), 1u);
    NF_CHECK(cs.get_cover_point(id1) == nullptr);
    NF_CHECK(!cs.unregister_cover_point(999));
}

NF_TEST(cover_claims_and_agent_ownership) {
    CoverSystem cs;

    CoverPoint pt1;
    pt1.position = Vec3{0.0f, 0.0f, 0.0f};
    u32 id1 = cs.register_cover_point(pt1);

    CoverPoint pt2;
    pt2.position = Vec3{5.0f, 0.0f, 0.0f};
    u32 id2 = cs.register_cover_point(pt2);

    // Claim cover 1 by agent 10
    NF_CHECK(cs.claim_cover(id1, 10));
    NF_CHECK_EQ(cs.get_agent_claimed_cover(10), id1);

    // Agent 10 re-claiming same cover succeeds
    NF_CHECK(cs.claim_cover(id1, 10));

    // Agent 20 cannot claim cover 1 (already claimed)
    NF_CHECK(!cs.claim_cover(id1, 20));

    // Agent 20 claims cover 2
    NF_CHECK(cs.claim_cover(id2, 20));
    NF_CHECK_EQ(cs.get_agent_claimed_cover(20), id2);

    // Release cover 1
    NF_CHECK(cs.release_cover(id1));
    NF_CHECK_EQ(cs.get_agent_claimed_cover(10), 0u);

    // Now Agent 20 can claim cover 1
    NF_CHECK(cs.claim_cover(id1, 20));

    // Release all for agent 20
    cs.release_all_for_agent(20);
    NF_CHECK_EQ(cs.get_agent_claimed_cover(20), 0u);
}

NF_TEST(cover_flanking_and_angle_validation) {
    CoverSystem cs;

    // Cover point at (0, 0, 0) with normal facing +Z (0, 0, 1)
    CoverPoint pt;
    pt.position = Vec3{0.0f, 0.0f, 0.0f};
    pt.normal = Vec3{0.0f, 0.0f, 1.0f};
    u32 cid = cs.register_cover_point(pt);

    // Threat directly ahead at (0, 0, 10): 0 deg from normal -> NOT flanked
    NF_CHECK(!cs.is_flanked(cid, Vec3{0.0f, 0.0f, 10.0f}, 60.0f));

    // Threat at 45 degrees ahead: (10, 0, 10) -> NOT flanked (45 deg <= 60 deg)
    NF_CHECK(!cs.is_flanked(cid, Vec3{10.0f, 0.0f, 10.0f}, 60.0f));

    // Threat at 90 degrees to the side: (10, 0, 0) -> FLANKED (90 deg > 60 deg)
    NF_CHECK(cs.is_flanked(cid, Vec3{10.0f, 0.0f, 0.0f}, 60.0f));

    // Threat behind cover: (0, 0, -10) -> FLANKED (180 deg)
    NF_CHECK(cs.is_flanked(cid, Vec3{0.0f, 0.0f, -10.0f}, 60.0f));
}

NF_TEST(cover_query_scoring_and_filtering) {
    CoverSystem cs;

    // Cover A: Low, at (0, 0, 10), normal facing +Z towards threat at (0, 0, 30)
    CoverPoint cA;
    cA.position = Vec3{0.0f, 0.0f, 10.0f};
    cA.normal = Vec3{0.0f, 0.0f, 1.0f};
    cA.posture = CoverPosture::Low;
    cA.peek_directions = CoverDirection::Over;
    cs.register_cover_point(cA);

    // Cover B: High, at (5, 0, 10), normal facing +Z, Left and Right peeks
    CoverPoint cB;
    cB.position = Vec3{5.0f, 0.0f, 10.0f};
    cB.normal = Vec3{0.0f, 0.0f, 1.0f};
    cB.posture = CoverPosture::High;
    cB.peek_directions = CoverDirection::Left | CoverDirection::Right;
    cs.register_cover_point(cB);

    // Cover C: Low, at (20, 0, 10), normal facing -Z (flanked by threat!)
    CoverPoint cC;
    cC.position = Vec3{20.0f, 0.0f, 10.0f};
    cC.normal = Vec3{0.0f, 0.0f, -1.0f};
    cC.posture = CoverPosture::Low;
    cs.register_cover_point(cC);

    CoverQuery query;
    query.querier_position = Vec3{0.0f, 0.0f, 0.0f};
    query.threat_position = Vec3{0.0f, 0.0f, 30.0f};
    query.max_distance_from_querier = 25.0f;
    query.flank_angle_degrees = 60.0f;

    // 1. Unfiltered best cover: cA is closer to querier (dist 10 vs 11.18) and faces threat directly
    CoverQueryResult best = cs.find_best_cover(query);
    NF_CHECK(best.valid);
    NF_CHECK_EQ(best.posture, CoverPosture::Low);
    NF_CHECK_NEAR(best.position.x, 0.0f, 1e-4f);

    // 2. Filter by High posture: should pick cB
    query.filter_posture = true;
    query.requested_posture = CoverPosture::High;
    CoverQueryResult best_high = cs.find_best_cover(query);
    NF_CHECK(best_high.valid);
    NF_CHECK_EQ(best_high.posture, CoverPosture::High);
    NF_CHECK_NEAR(best_high.position.x, 5.0f, 1e-4f);

    // 3. Filter by required peek direction Left: cA doesn't have Left, cB does
    query.filter_posture = false;
    query.required_peek_direction = CoverDirection::Left;
    CoverQueryResult best_peek = cs.find_best_cover(query);
    NF_CHECK(best_peek.valid);
    NF_CHECK_NEAR(best_peek.position.x, 5.0f, 1e-4f);

    // 4. Query all valid covers: cC must be rejected because it faces away from threat
    query.required_peek_direction = CoverDirection::None;
    std::vector<CoverQueryResult> results;
    cs.find_all_valid_covers(query, results);
    NF_CHECK_EQ(results.size(), 2u); // Only cA and cB
    NF_CHECK(results[0].total_score >= results[1].total_score);
}

NF_TEST(cover_procedural_wall_generation) {
    CoverSystem cs;

    const Vec3 wall_start{0.0f, 0.0f, 0.0f};
    const Vec3 wall_end{10.0f, 0.0f, 0.0f};
    const Vec3 wall_normal{0.0f, 0.0f, 1.0f};

    // Generate wall cover with 2.5m spacing, standoff 0.5m, Low posture
    std::vector<u32> ids = cs.generate_wall_cover(wall_start, wall_end, wall_normal, CoverPosture::Low, 2.5f, 0.5f);
    NF_CHECK(ids.size() >= 5u); // 10m length / 2.5m = 4 segments + 1 = 5 points

    // First point (start of wall)
    const CoverPoint* first_pt = cs.get_cover_point(ids.front());
    NF_CHECK(first_pt != nullptr);
    NF_CHECK_NEAR(first_pt->position.x, 0.0f, 1e-4f);
    NF_CHECK_NEAR(first_pt->position.z, -0.5f, 1e-4f); // Standoff behind normal
    NF_CHECK((first_pt->peek_directions & CoverDirection::Left) != 0);
    NF_CHECK((first_pt->peek_directions & CoverDirection::Over) != 0);

    // Last point (end of wall)
    const CoverPoint* last_pt = cs.get_cover_point(ids.back());
    NF_CHECK(last_pt != nullptr);
    NF_CHECK_NEAR(last_pt->position.x, 10.0f, 1e-4f);
    NF_CHECK_NEAR(last_pt->position.z, -0.5f, 1e-4f);
    NF_CHECK((last_pt->peek_directions & CoverDirection::Right) != 0);
    NF_CHECK((last_pt->peek_directions & CoverDirection::Over) != 0);
}
