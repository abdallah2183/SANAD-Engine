#include <NF/Test/TestFramework.hpp>
#include <NF/Core/StateMachine.hpp>

using namespace nf;

namespace {

enum TestStates : StateId {
    kStateRoot = 1,
    kStateGrounded = 2,
    kStateAirborne = 3,
    kStateIdle = 4,
    kStateRunning = 5,
    kStateJumping = 6
};

enum TestEvents : EventId {
    kEventMove = 10,
    kEventStop = 11,
    kEventJump = 12,
    kEventLand = 13
};

} // namespace

NF_TEST(state_machine_hierarchical_enter_and_exit) {
    HierarchicalStateMachine hsm;
    std::vector<std::string> log;

    // Root state
    hsm.add_state(kStateGrounded, "Grounded");
    hsm.set_callbacks(kStateGrounded,
        [&log]() { log.push_back("enter_grounded"); },
        nullptr,
        [&log]() { log.push_back("exit_grounded"); }
    );

    // Sub-states under Grounded
    hsm.add_state(kStateIdle, "Idle", kStateGrounded);
    hsm.set_callbacks(kStateIdle,
        [&log]() { log.push_back("enter_idle"); },
        nullptr,
        [&log]() { log.push_back("exit_idle"); }
    );

    hsm.add_state(kStateRunning, "Running", kStateGrounded);
    hsm.set_callbacks(kStateRunning,
        [&log]() { log.push_back("enter_running"); },
        nullptr,
        [&log]() { log.push_back("exit_running"); }
    );

    // Transitions
    hsm.add_transition(kStateIdle, kStateRunning, kEventMove);
    hsm.add_transition(kStateRunning, kStateIdle, kEventStop);

    // Start in Idle
    hsm.start(kStateIdle);
    NF_CHECK_EQ(hsm.current_state(), kStateIdle);
    NF_CHECK(hsm.is_in_state(kStateIdle));
    NF_CHECK(hsm.is_in_state(kStateGrounded));

    // Both parent and child should have entered in order
    NF_CHECK_EQ(log.size(), 2u);
    NF_CHECK(log[0] == "enter_grounded");
    NF_CHECK(log[1] == "enter_idle");

    // Transition Idle -> Running (sibling sub-states under same parent)
    log.clear();
    bool transitioned = hsm.dispatch_event(kEventMove);
    NF_CHECK(transitioned);
    NF_CHECK_EQ(hsm.current_state(), kStateRunning);
    NF_CHECK(hsm.is_in_state(kStateRunning));
    NF_CHECK(hsm.is_in_state(kStateGrounded));

    // Parent should NOT exit or re-enter! Only Idle exits, Running enters
    NF_CHECK_EQ(log.size(), 2u);
    NF_CHECK(log[0] == "exit_idle");
    NF_CHECK(log[1] == "enter_running");
}

NF_TEST(state_machine_guarded_transition_and_history) {
    HierarchicalStateMachine hsm;
    bool can_jump = false;

    hsm.add_state(kStateGrounded, "Grounded");
    hsm.add_state(kStateAirborne, "Airborne");

    hsm.add_transition(kStateGrounded, kStateAirborne, kEventJump, [&can_jump]() {
        return can_jump;
    });

    hsm.start(kStateGrounded);
    NF_CHECK_EQ(hsm.current_state(), kStateGrounded);

    // Should fail guard
    bool fired = hsm.dispatch_event(kEventJump);
    NF_CHECK(!fired);
    NF_CHECK_EQ(hsm.current_state(), kStateGrounded);

    // Enable guard and retry
    can_jump = true;
    fired = hsm.dispatch_event(kEventJump);
    NF_CHECK(fired);
    NF_CHECK_EQ(hsm.current_state(), kStateAirborne);
    NF_CHECK_EQ(hsm.previous_state(), kStateGrounded);

    // Restore previous state
    NF_CHECK(hsm.restore_previous());
    NF_CHECK_EQ(hsm.current_state(), kStateGrounded);
}
