// AITests — behavior trees: composites, decorators, leaves, blackboard.

#include <NF/AI/BehaviorTree.hpp>
#include <NF/Test/TestFramework.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::ai;

namespace {

std::unique_ptr<BTNode> ok() {
    return std::make_unique<BTAction>(
        [](Blackboard&, float) { return BTStatus::Success; });
}
std::unique_ptr<BTNode> fail() {
    return std::make_unique<BTAction>(
        [](Blackboard&, float) { return BTStatus::Failure; });
}
std::unique_ptr<BTNode> running() {
    return std::make_unique<BTAction>(
        [](Blackboard&, float) { return BTStatus::Running; });
}

} // namespace

NF_TEST(bt_blackboard_roundtrip) {
    Blackboard bb;
    bb.set_number("hp", 75.5);
    NF_CHECK_NEAR(bb.get_number("hp"), 75.5, 1e-9);
    NF_CHECK_NEAR(bb.get_number("missing", 42.0), 42.0, 1e-9);
    bb.set_flag("alert", true);
    NF_CHECK(bb.get_flag("alert"));
    NF_CHECK(!bb.get_flag("missing"));
    bb.set_text("name", "goblin");
    NF_CHECK(bb.get_text("name") == "goblin");
    NF_CHECK(bb.get_text("missing", "dflt") == "dflt");
    bb.clear();
    NF_CHECK_NEAR(bb.get_number("hp"), 0.0, 1e-9);
}

NF_TEST(bt_sequence_fails_fast_in_order) {
    std::vector<std::string> order;
    auto seq = std::make_unique<BTSequence>();
    seq->add(std::make_unique<BTAction>([&](Blackboard&, float) {
        order.push_back("a");
        return BTStatus::Success;
    }));
    seq->add(std::make_unique<BTAction>([&](Blackboard&, float) {
        order.push_back("b");
        return BTStatus::Failure;
    }));
    seq->add(std::make_unique<BTAction>([&](Blackboard&, float) {
        order.push_back("c");
        return BTStatus::Success;
    }));
    BehaviorTree tree(std::move(seq));
    Blackboard bb;
    NF_CHECK(tree.tick(bb, 0.016f) == BTStatus::Failure);
    NF_CHECK(order.size() == 2); // c never ran
    NF_CHECK(order[0] == "a" && order[1] == "b");
}

NF_TEST(bt_selector_falls_back_to_first_success) {
    auto sel = std::make_unique<BTSelector>();
    sel->add(fail());
    sel->add(fail());
    sel->add(ok());
    BehaviorTree tree(std::move(sel));
    Blackboard bb;
    NF_CHECK(tree.tick(bb, 0.016f) == BTStatus::Success);

    auto sel2 = std::make_unique<BTSelector>();
    sel2->add(fail());
    sel2->add(fail());
    BehaviorTree tree2(std::move(sel2));
    NF_CHECK(tree2.tick(bb, 0.016f) == BTStatus::Failure);
}

NF_TEST(bt_running_propagates_and_preempts) {
    // Selector: [sequence: condition then running-action] vs fallback.
    // While the condition holds, the running branch owns the tick.
    auto seq = std::make_unique<BTSequence>();
    seq->add(std::make_unique<BTCondition>(
        [](const Blackboard& bb) { return bb.get_flag("go"); }));
    seq->add(running());
    auto sel = std::make_unique<BTSelector>();
    sel->add(std::move(seq));
    sel->add(ok());
    BehaviorTree tree(std::move(sel));
    Blackboard bb;
    bb.set_flag("go", true);
    NF_CHECK(tree.tick(bb, 0.016f) == BTStatus::Running);
    NF_CHECK(tree.tick(bb, 0.016f) == BTStatus::Running);
    bb.set_flag("go", false); // condition drops: falls back to ok()
    NF_CHECK(tree.tick(bb, 0.016f) == BTStatus::Success);
}

NF_TEST(bt_inverter_flips) {
    Blackboard bb;
    BehaviorTree t1(std::make_unique<BTInverter>(ok()));
    NF_CHECK(t1.tick(bb, 0.016f) == BTStatus::Failure);
    BehaviorTree t2(std::make_unique<BTInverter>(fail()));
    NF_CHECK(t2.tick(bb, 0.016f) == BTStatus::Success);
    BehaviorTree t3(std::make_unique<BTInverter>(running()));
    NF_CHECK(t3.tick(bb, 0.016f) == BTStatus::Running);
}

NF_TEST(bt_wait_times_out_over_ticks) {
    BehaviorTree tree(std::make_unique<BTWait>(1.0f));
    Blackboard bb;
    NF_CHECK(tree.tick(bb, 0.4f) == BTStatus::Running);
    NF_CHECK(tree.tick(bb, 0.4f) == BTStatus::Running);
    NF_CHECK(tree.tick(bb, 0.4f) == BTStatus::Success);
    NF_CHECK(tree.tick(bb, 0.4f) == BTStatus::Success); // stays done
    tree.reset();
    NF_CHECK(tree.tick(bb, 0.4f) == BTStatus::Running); // restarts
}

NF_TEST(bt_repeat_counts_successes) {
    int calls = 0;
    auto act = std::make_unique<BTAction>([&](Blackboard&, float) {
        ++calls;
        return BTStatus::Success;
    });
    BehaviorTree tree(std::make_unique<BTRepeat>(std::move(act), 3));
    Blackboard bb;
    // Instant children finish in a single tick.
    NF_CHECK(tree.tick(bb, 0.016f) == BTStatus::Success);
    NF_CHECK(calls == 3);
    // A second tick re-runs (repeat does not latch without reset... it
    // returns Success again since m_done stays >= times).
    NF_CHECK(tree.tick(bb, 0.016f) == BTStatus::Success);
    NF_CHECK(calls == 3); // latched: no re-runs until reset
    tree.reset();
    NF_CHECK(tree.tick(bb, 0.016f) == BTStatus::Success);
    NF_CHECK(calls == 6);
}

NF_TEST(bt_guard_patrol_pattern) {
    // The classic: selector { sequence[enemy_visible, attack], patrol }.
    Blackboard bb;
    bb.set_flag("enemy_visible", false);
    int attacked = 0, patrolled = 0;
    auto attack_seq = std::make_unique<BTSequence>();
    attack_seq->add(std::make_unique<BTCondition>(
        [](const Blackboard& b) { return b.get_flag("enemy_visible"); }));
    attack_seq->add(std::make_unique<BTAction>([&](Blackboard&, float) {
        ++attacked;
        return BTStatus::Success;
    }));
    auto root = std::make_unique<BTSelector>();
    root->add(std::move(attack_seq));
    root->add(std::make_unique<BTAction>([&](Blackboard&, float) {
        ++patrolled;
        return BTStatus::Success;
    }));
    BehaviorTree tree(std::move(root));
    NF_CHECK(tree.tick(bb, 0.016f) == BTStatus::Success);
    NF_CHECK(patrolled == 1 && attacked == 0);
    bb.set_flag("enemy_visible", true);
    NF_CHECK(tree.tick(bb, 0.016f) == BTStatus::Success);
    NF_CHECK(attacked == 1 && patrolled == 1);
}
