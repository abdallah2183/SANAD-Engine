// AITests/state machine — states, guarded transitions, occupancy brackets.
//
// Every case is pure CPU and drives the machine through a Blackboard, which is
// the same seam the behavior tree exposes: a transition is asserted by flipping
// a flag, not by instantiating a world.

#include <NF/AI/StateMachine.hpp>
#include <NF/Test/TestFramework.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::ai;

namespace {

/// A state that records every callback it gets, in order, into a shared log.
/// The log is the assertion: enter/exit/tick bracketing is the machine's whole
// contract, and a callback that fires out of order is invisible any other way.
class TrackedState : public SMState {
public:
    TrackedState(std::string name, std::vector<std::string>* log)
        : m_name(std::move(name)), m_log(log) {}

    void enter(Blackboard&) override { m_log->push_back(m_name + ":enter"); }
    void exit(Blackboard&) override { m_log->push_back(m_name + ":exit"); }
    SMStatus tick(Blackboard& bb, float) override {
        m_log->push_back(m_name + ":tick");
        return static_cast<SMStatus>(static_cast<int>(bb.get_number(m_name + ".status")));
    }

private:
    std::string m_name;
    std::vector<std::string>* m_log;
};

std::unique_ptr<SMState> tracked(const std::string& name, std::vector<std::string>* log) {
    return std::make_unique<TrackedState>(name, log);
}

} // namespace

NF_TEST(sm_states_get_insertion_order_ids) {
    std::vector<std::string> log;
    StateMachine sm;
    const u32 idle = sm.add_state(tracked("idle", &log));
    const u32 patrol = sm.add_state(tracked("patrol", &log));
    const u32 alert = sm.add_state(tracked("alert", &log));
    NF_CHECK(idle == 0u);
    NF_CHECK(patrol == 1u);
    NF_CHECK(alert == 2u);
    NF_CHECK(sm.states().size() == 3);
}

NF_TEST(sm_ticks_the_start_state_and_reports_its_status) {
    std::vector<std::string> log;
    StateMachine sm;
    const u32 idle = sm.add_state(tracked("idle", &log));
    (void)idle;
    sm.set_start(0);

    Blackboard bb;
    bb.set_number("idle.status", static_cast<double>(static_cast<int>(SMStatus::Success)));
    NF_CHECK(sm.tick(bb, 0.1f) == SMStatus::Success);
    NF_CHECK(log.size() == 1);
    NF_CHECK(log.front() == "idle:tick");
}

NF_TEST(sm_transition_fires_exit_then_enter) {
    std::vector<std::string> log;
    StateMachine sm;
    sm.add_state(tracked("idle", &log));
    sm.add_state(tracked("alert", &log));
    sm.set_start(0);
    sm.add_transition(0, 1, [](const Blackboard& bb) { return bb.get_flag("alerted"); });

    Blackboard bb;
    sm.tick(bb, 0.1f);
    NF_CHECK(log.size() == 1); // guard not set: nothing moved

    bb.set_flag("alerted", true);
    sm.tick(bb, 0.1f);
    // idle:tick (the transition tick re-ticks the state it is leaving, since a
    // guard is evaluated *after* the tick), idle:exit, alert:enter — in that
    // order, and no alert:tick until the next tick. The new state never ticks
    // on the tick that entered it.
    NF_CHECK(log.size() == 4);
    NF_CHECK(log[0] == "idle:tick"); // first tick, guard not yet set
    NF_CHECK(log[1] == "idle:tick"); // the tick that then transitions
    NF_CHECK(log[2] == "idle:exit");
    NF_CHECK(log[3] == "alert:enter");
    NF_CHECK(sm.current() == 1u);

    sm.tick(bb, 0.1f);
    NF_CHECK(log.back() == "alert:tick");
}

NF_TEST(sm_first_matching_guard_wins) {
    std::vector<std::string> log;
    StateMachine sm;
    sm.add_state(tracked("idle", &log));
    sm.add_state(tracked("alert", &log));
    sm.add_state(tracked("flee", &log));
    sm.set_start(0);

    // Both guards hold on the same flag. Authoring order decides: alert is
    // earlier, so alert wins — never flee, never "both".
    sm.add_transition(0, 1, [](const Blackboard& bb) { return bb.get_flag("danger"); });
    sm.add_transition(0, 2, [](const Blackboard& bb) { return bb.get_flag("danger"); });

    Blackboard bb;
    bb.set_flag("danger", true);
    sm.tick(bb, 0.1f);
    NF_CHECK(sm.current() == 1u);
    NF_CHECK(sm.transition_count() == 1u);
    NF_CHECK(log.size() == 3); // idle:tick, idle:exit, alert:enter — no flee
}

NF_TEST(sm_transitions_from_other_states_are_ignored) {
    std::vector<std::string> log;
    StateMachine sm;
    sm.add_state(tracked("idle", &log));
    sm.add_state(tracked("alert", &log));
    sm.set_start(0);
    // A rule keyed on idle, evaluated while idle is the current state, is dead.
    sm.add_transition(1, 0, [](const Blackboard&) { return true; });

    Blackboard bb;
    sm.tick(bb, 0.1f);
    NF_CHECK(sm.current() == 0u);
    NF_CHECK(sm.transition_count() == 0u);
}

NF_TEST(sm_self_transition_does_not_re_enter) {
    std::vector<std::string> log;
    StateMachine sm;
    sm.add_state(tracked("idle", &log));
    sm.set_start(0);
    // A guard that is always true, pointed back at the state it came from. If
    // this re-entered, idle:enter/idle:exit would appear on every tick.
    sm.add_transition(0, 0, [](const Blackboard&) { return true; });

    Blackboard bb;
    sm.tick(bb, 0.1f);
    sm.tick(bb, 0.1f);
    NF_CHECK(sm.transition_count() == 0u);
    bool saw_enter = false;
    for (const std::string& e : log) {
        if (e == "idle:enter") saw_enter = true;
    }
    NF_CHECK(!saw_enter);
}

NF_TEST(sm_one_transition_per_tick_even_when_the_target_also_wants_to_leave) {
    std::vector<std::string> log;
    StateMachine sm;
    sm.add_state(tracked("idle", &log));
    sm.add_state(tracked("alert", &log));
    sm.add_state(tracked("flee", &log));
    sm.set_start(0);
    sm.add_transition(0, 1, [](const Blackboard&) { return true; });
    sm.add_transition(1, 2, [](const Blackboard&) { return true; });

    Blackboard bb;
    sm.tick(bb, 0.1f);
    // Both guards hold, but a tick resolves one hop. Flee is reachable on the
    // next tick — that is what makes a chain of states a chain and not a jump.
    NF_CHECK(sm.current() == 1u);
    NF_CHECK(sm.transition_count() == 1u);
    sm.tick(bb, 0.1f);
    NF_CHECK(sm.current() == 2u);
    NF_CHECK(sm.transition_count() == 2u);
}

NF_TEST(sm_force_state_runs_the_bracket) {
    std::vector<std::string> log;
    StateMachine sm;
    sm.add_state(tracked("idle", &log));
    sm.add_state(tracked("stunned", &log));
    sm.set_start(0);

    Blackboard bb;
    sm.tick(bb, 0.1f);
    log.clear();

    NF_CHECK(sm.force_state(1, bb));
    NF_CHECK(sm.current() == 1u);
    NF_CHECK(sm.transition_count() == 1u); // a forced move counts as a transition
    NF_CHECK(log.size() == 2);
    NF_CHECK(log[0] == "idle:exit");
    NF_CHECK(log[1] == "stunned:enter");

    // Forcing where it already is is a no-op, not a re-enter.
    NF_CHECK(!sm.force_state(1, bb));
    NF_CHECK(log.size() == 2);
    NF_CHECK(sm.transition_count() == 1u);

    // Forcing at a state id that was never added is rejected, not crashed on.
    NF_CHECK(!sm.force_state(5, bb));
    NF_CHECK(sm.current() == 1u);
}

NF_TEST(sm_guard_may_read_what_the_tick_just_wrote) {
    StateMachine sm;
    std::vector<std::string> log;
    sm.add_state(tracked("attack", &log));
    sm.add_state(tracked("idle", &log));
    sm.set_start(0);
    // The guard reads a number the *attack tick* sets, so the transition fires
    // on the same tick the condition became true rather than a tick late.
    sm.add_transition(0, 1, [](const Blackboard& bb) {
        return bb.get_number("attacks.left") <= 0.0;
    });

    Blackboard bb;
    bb.set_number("attacks.left", 1.0);
    sm.tick(bb, 0.1f);
    NF_CHECK(sm.current() == 0u);
    bb.set_number("attacks.left", 0.0);
    sm.tick(bb, 0.1f);
    NF_CHECK(sm.current() == 1u);
}

NF_TEST(sm_reset_exits_and_returns_to_not_started) {
    std::vector<std::string> log;
    StateMachine sm;
    sm.add_state(tracked("idle", &log));
    sm.add_state(tracked("channeling", &log));
    sm.set_start(0);
    sm.add_transition(0, 1, [](const Blackboard&) { return true; });

    Blackboard bb;
    sm.tick(bb, 0.1f); // idle -> channeling
    NF_CHECK(sm.current() == 1u);
    log.clear();

    sm.reset(bb);
    NF_CHECK(sm.current() == StateMachine::kNotStarted);
    NF_CHECK(log.size() == 1);
    NF_CHECK(log.front() == "channeling:exit"); // cleanup owed and paid

    // The graph survives: the machine can be started again.
    sm.set_start(0);
    sm.tick(bb, 0.1f);
    NF_CHECK(sm.current() == 1u);
}

NF_TEST(sm_not_started_machine_ticks_nothing) {
    std::vector<std::string> log;
    StateMachine sm;
    sm.add_state(tracked("idle", &log));

    Blackboard bb;
    NF_CHECK(sm.tick(bb, 0.1f) == SMStatus::Running);
    NF_CHECK(log.empty()); // no set_start: the NPC stands there
    NF_CHECK(sm.current() == StateMachine::kNotStarted);
}

NF_TEST(sm_start_on_an_unknown_state_is_safe) {
    std::vector<std::string> log;
    StateMachine sm;
    sm.add_state(tracked("idle", &log));
    sm.set_start(4); // nothing was added at 4

    Blackboard bb;
    NF_CHECK(sm.tick(bb, 0.1f) == SMStatus::Running);
    NF_CHECK(sm.current() == 4u);
    NF_CHECK(log.empty()); // no dereference of a slot that does not exist
}

NF_TEST(sm_negative_dt_does_not_run_time_backwards) {
    std::vector<std::string> log;
    StateMachine sm;
    sm.add_state(tracked("idle", &log));
    sm.set_start(0);

    Blackboard bb;
    bb.set_number("idle.status", static_cast<double>(static_cast<int>(SMStatus::Failure)));
    NF_CHECK(sm.tick(bb, -100.0f) == SMStatus::Failure);
    NF_CHECK(log.size() == 1); // the tick still ran; only the clock was wrong
}

NF_TEST(sm_replays_bit_identically) {
    auto run = [] {
        std::vector<std::string> log;
        StateMachine sm;
        sm.add_state(tracked("idle", &log));
        sm.add_state(tracked("alert", &log));
        sm.add_state(tracked("attack", &log));
        sm.set_start(0);
        sm.add_transition(0, 1, [](const Blackboard& bb) { return bb.get_flag("see"); });
        sm.add_transition(1, 2, [](const Blackboard& bb) { return bb.get_flag("in_range"); });
        sm.add_transition(2, 0, [](const Blackboard& bb) { return !bb.get_flag("see"); });

        Blackboard bb;
        bb.set_flag("see", true);
        sm.tick(bb, 0.1f);
        bb.set_flag("in_range", true);
        sm.tick(bb, 0.1f);
        bb.set_flag("see", false);
        sm.tick(bb, 0.1f);
        sm.tick(bb, 0.1f);
        return std::pair(log, sm.transition_count());
    };

    const auto [first_log, first_count] = run();
    const auto [second_log, second_count] = run();
    NF_CHECK(first_count == second_count);
    NF_CHECK(first_count == 3u);
    NF_CHECK(first_log.size() == second_log.size());
    for (size_t i = 0; i < first_log.size(); ++i) {
        NF_CHECK(first_log[i] == second_log[i]); // same path, same callbacks
    }
}

NF_TEST(sm_transition_count_tracks_the_path_taken) {
    std::vector<std::string> log;
    StateMachine sm;
    sm.add_state(tracked("a", &log));
    sm.add_state(tracked("b", &log));
    sm.set_start(0);
    sm.add_transition(0, 1, [](const Blackboard& bb) { return bb.get_flag("forward"); });
    sm.add_transition(1, 0, [](const Blackboard& bb) { return bb.get_flag("back"); });

    Blackboard bb;
    NF_CHECK(sm.transition_count() == 0u);

    // A ping-pong: every tick hands over, so the count is exactly the hop count.
    for (int i = 0; i < 3; ++i) {
        bb.set_flag("forward", true);
        bb.set_flag("back", false);
        sm.tick(bb, 0.1f);
        NF_CHECK(sm.transition_count() == static_cast<u32>(2 * i + 1));

        bb.set_flag("forward", false);
        bb.set_flag("back", true);
        sm.tick(bb, 0.1f);
        NF_CHECK(sm.transition_count() == static_cast<u32>(2 * i + 2));
    }
}

NF_TEST(sm_state_may_wrap_a_behavior_tree) {
    // The two layers compose: a state whose tick runs a whole tree, forwarding
    // its status. This is why SMStatus and BTStatus share enumerators.
    std::vector<std::string> log;
    auto tree = std::make_unique<BTSequence>();
    tree->add(std::make_unique<BTAction>([&](Blackboard&, float) {
        log.push_back("bt:a");
        return BTStatus::Success;
    }));
    tree->add(std::make_unique<BTAction>([&](Blackboard& bb, float) {
        log.push_back("bt:b");
        return bb.get_flag("ready") ? BTStatus::Success : BTStatus::Running;
    }));

    class TreeState : public SMState {
    public:
        explicit TreeState(BehaviorTree t) : m_tree(std::move(t)) {}
        SMStatus tick(Blackboard& bb, float dt) override {
            return static_cast<SMStatus>(static_cast<int>(m_tree.tick(bb, dt)));
        }

    private:
        BehaviorTree m_tree;
    };

    StateMachine sm;
    sm.add_state(std::make_unique<TreeState>(BehaviorTree(std::move(tree))));
    sm.set_start(0);

    Blackboard bb;
    NF_CHECK(sm.tick(bb, 0.1f) == SMStatus::Running); // b not ready
    NF_CHECK(log.size() == 2);                        // reactive: re-ticked from the root
    bb.set_flag("ready", true);
    NF_CHECK(sm.tick(bb, 0.1f) == SMStatus::Success);
    NF_CHECK(log.size() == 4);
}
