#include <NF/Test/TestFramework.hpp>
#include <NF/AI/BehaviorTree.hpp>
#include <NF/AI/EQS.hpp>
#include <cmath>

using namespace nf;
using namespace nf::ai;

NF_TEST(bt_parallel_require_all_and_one) {
    Blackboard bb;

    // 1. RequireAll
    auto par_all = std::make_unique<BTParallel>(BTParallelPolicy::RequireAll);
    par_all->add(std::make_unique<BTAction>([](Blackboard&, float) { return BTStatus::Success; }));
    par_all->add(std::make_unique<BTAction>([](Blackboard&, float) { return BTStatus::Success; }));
    NF_CHECK(par_all->tick(bb, 0.1f) == BTStatus::Success);

    auto par_all_fail = std::make_unique<BTParallel>(BTParallelPolicy::RequireAll);
    par_all_fail->add(std::make_unique<BTAction>([](Blackboard&, float) { return BTStatus::Success; }));
    par_all_fail->add(std::make_unique<BTAction>([](Blackboard&, float) { return BTStatus::Failure; }));
    NF_CHECK(par_all_fail->tick(bb, 0.1f) == BTStatus::Failure);

    // 2. RequireOne
    auto par_one = std::make_unique<BTParallel>(BTParallelPolicy::RequireOne);
    par_one->add(std::make_unique<BTAction>([](Blackboard&, float) { return BTStatus::Failure; }));
    par_one->add(std::make_unique<BTAction>([](Blackboard&, float) { return BTStatus::Success; }));
    NF_CHECK(par_one->tick(bb, 0.1f) == BTStatus::Success);

    auto par_one_fail = std::make_unique<BTParallel>(BTParallelPolicy::RequireOne);
    par_one_fail->add(std::make_unique<BTAction>([](Blackboard&, float) { return BTStatus::Failure; }));
    par_one_fail->add(std::make_unique<BTAction>([](Blackboard&, float) { return BTStatus::Failure; }));
    NF_CHECK(par_one_fail->tick(bb, 0.1f) == BTStatus::Failure);
}

NF_TEST(bt_decorators_cooldown_and_retry) {
    Blackboard bb;

    // Cooldown
    auto action = std::make_unique<BTAction>([](Blackboard&, float) { return BTStatus::Success; });
    BTCooldown cooldown_node(std::move(action), 1.0f);

    // First attempt succeeds
    NF_CHECK(cooldown_node.tick(bb, 0.1f) == BTStatus::Success);

    // Second attempt at 0.5s fails (still in cooldown)
    NF_CHECK(cooldown_node.tick(bb, 0.5f) == BTStatus::Failure);

    // Third attempt at 0.6s (total 1.1s elapsed since success) succeeds!
    NF_CHECK(cooldown_node.tick(bb, 0.6f) == BTStatus::Success);

    // Retry
    int attempts = 0;
    auto fail_twice = std::make_unique<BTAction>([&attempts](Blackboard&, float) {
        attempts++;
        return (attempts >= 3) ? BTStatus::Success : BTStatus::Failure;
    });

    BTRetry retry_node(std::move(fail_twice), 4);
    NF_CHECK(retry_node.tick(bb, 0.1f) == BTStatus::Running);
    NF_CHECK(retry_node.tick(bb, 0.1f) == BTStatus::Running);
    NF_CHECK(retry_node.tick(bb, 0.1f) == BTStatus::Success);
    NF_CHECK_EQ(attempts, 3);
}

NF_TEST(bt_decorators_timelimit_and_force) {
    Blackboard bb;

    // TimeLimit
    auto endless = std::make_unique<BTAction>([](Blackboard&, float) { return BTStatus::Running; });
    BTTimeLimit limit_node(std::move(endless), 1.0f);

    NF_CHECK(limit_node.tick(bb, 0.4f) == BTStatus::Running);
    NF_CHECK(limit_node.tick(bb, 0.4f) == BTStatus::Running);
    // Exceeds 1.0s (0.4 + 0.4 + 0.3 = 1.1s) -> Fails!
    NF_CHECK(limit_node.tick(bb, 0.3f) == BTStatus::Failure);

    // Force Success & Failure
    BTForceSuccess force_succ(std::make_unique<BTAction>([](Blackboard&, float) { return BTStatus::Failure; }));
    NF_CHECK(force_succ.tick(bb, 0.1f) == BTStatus::Success);

    BTForceFailure force_fail(std::make_unique<BTAction>([](Blackboard&, float) { return BTStatus::Success; }));
    NF_CHECK(force_fail.tick(bb, 0.1f) == BTStatus::Failure);
}

NF_TEST(bt_builder_fluent_authoring) {
    Blackboard bb;
    bb.set_number("health", 15.0);
    bb.set_flag("enemy_visible", true);

    bool took_cover = false;
    bool attacked = false;

    BTBuilder builder;
    builder.selector()
        .sequence()
            .condition([](const Blackboard& b) { return b.get_number("health") < 20.0; })
            .action([&took_cover](Blackboard&, float) {
                took_cover = true;
                return BTStatus::Success;
            })
        .end()
        .sequence()
            .condition([](const Blackboard& b) { return b.get_flag("enemy_visible"); })
            .action([&attacked](Blackboard&, float) {
                attacked = true;
                return BTStatus::Success;
            })
        .end()
    .end();

    auto tree = builder.build();
    NF_CHECK(tree != nullptr);

    BTStatus s = tree->tick(bb, 0.1f);
    NF_CHECK(s == BTStatus::Success);
    NF_CHECK(took_cover);
    NF_CHECK(!attacked); // Lower-priority attack branch should not run
}

NF_TEST(eqs_circle_generation_and_scoring) {
    Vec3 ai_pos(0, 0, 0);
    Vec3 enemy_pos(10, 0, 0);

    auto items = EQSQuery::generate_circle(ai_pos, 8.0f, 2.0f, 2.0f);
    NF_CHECK(!items.empty());

    // Filter items too close to enemy (e.g. closer than 4m)
    auto result = EQSQuery::run(items, {
        [&enemy_pos](std::vector<EQSItem>& pts) {
            EQSQuery::filter_range(pts, enemy_pos, 4.0f, 100.0f);
        },
        [&enemy_pos](std::vector<EQSItem>& pts) {
            // Preferred distance to enemy: 6.0m
            EQSQuery::score_distance(pts, enemy_pos, 6.0f, 1.0f);
        }
    });

    NF_CHECK(result.found);
    NF_CHECK(result.best_score > 0.0f);

    float dist_to_enemy = (result.best_position - enemy_pos).length();
    NF_CHECK(dist_to_enemy >= 4.0f);
    NF_CHECK(std::abs(dist_to_enemy - 6.0f) < 2.5f);
}

NF_TEST(eqs_grid_and_direction_scoring) {
    Vec3 center(0, 0, 0);
    auto items = EQSQuery::generate_grid(center, 5.0f, 1.0f);
    NF_CHECK_EQ(items.size(), 121u); // 11x11 grid (-5 to +5)

    // Score items in front (+Z axis) and prefer distance ~4m
    auto result = EQSQuery::run(items, {
        [&center](std::vector<EQSItem>& pts) {
            EQSQuery::score_direction(pts, center, Vec3(0, 0, 1), 1.0f, 2.0f);
            EQSQuery::score_distance(pts, center, 4.0f, 1.0f);
        }
    });

    NF_CHECK(result.found);
    // Best position should be forward along +Z axis around z ~ 4
    NF_CHECK(result.best_position.z >= 3.0f);
    NF_CHECK_NEAR(result.best_position.x, 0.0f, 0.001f);
}
