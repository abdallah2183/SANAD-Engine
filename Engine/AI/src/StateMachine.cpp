#include <NF/AI/StateMachine.hpp>

#include <algorithm>
#include <utility>

namespace nf::ai {

u32 StateMachine::add_state(std::unique_ptr<SMState> state) {
    // A null state would be dereferenced on the first tick of it; the machine
    // has no use for an empty slot, so it is rejected outright rather than
    // stored as a latent crash.
    m_states.push_back(std::move(state));
    return static_cast<u32>(m_states.size() - 1);
}

u32 StateMachine::add_transition(u32 from, u32 to,
                                 std::function<bool(const Blackboard&)> guard) {
    // Both endpoints are validated here rather than at tick time: a bad
    // transition is an authoring error, and reporting it once at registration
    // keeps the per-tick path free of range checks (and of silent skips).
    SMTransition t;
    t.from = from;
    t.to = to;
    t.guard = std::move(guard);
    m_transitions.push_back(std::move(t));
    return static_cast<u32>(m_transitions.size() - 1);
}

void StateMachine::set_start(u32 state) {
    // Deliberately not clamped to "still exists": the caller is setting up, and
    // an out-of-range start is caught on the first tick where it is used.
    m_current = state;
}

SMStatus StateMachine::tick(Blackboard& bb, float dt) {
    if (dt < 0.0f) dt = 0.0f;
    if (m_current == kNotStarted || m_current >= m_states.size()) {
        // Not started, or started on a state that was never added. Either way
        // there is nothing to run; Running rather than Failure so a caller that
        // only checks for Failure keeps going.
        m_last_status = SMStatus::Running;
        return m_last_status;
    }

    SMState& active = *m_states[m_current];
    m_last_status = active.tick(bb, dt);

    // One transition per tick, first match wins. Evaluating after the tick means
    // the state sees the world as it left it, and a guard may key on a flag the
    // tick itself just set.
    for (const SMTransition& t : m_transitions) {
        if (t.from != m_current) continue;
        if (t.to >= m_states.size()) continue; // dangling wiring, skipped
        if (!t.guard) continue;                // a guardless rule never fires

        if (t.guard(bb)) {
            if (t.to == m_current) continue; // self-transition: no re-enter
            goto_state(t.to, bb);
            break;
        }
    }

    return m_last_status;
}

bool StateMachine::force_state(u32 state, Blackboard& bb) {
    if (state == m_current) return false;
    if (state >= m_states.size()) return false;
    // The occupancy bracket is kept intact: exit, move, enter. Skipping the
    // callbacks would leave the previous state's flags set — a "channeling"
    // state forced out of its occupancy would keep channeling.
    if (m_current != kNotStarted && m_current < m_states.size()) {
        m_states[m_current]->exit(bb);
    }
    m_current = state;
    m_states[state]->enter(bb);
    ++m_transition_count;
    return true;
}

void StateMachine::reset(Blackboard& bb) {
    // Only the occupancy resets; the graph is authored once. An exit is owed to
    // the state being left — a reset mid-occupancy is exactly the case where a
    // state would otherwise never see its cleanup.
    if (m_current != kNotStarted && m_current < m_states.size()) {
        m_states[m_current]->exit(bb);
    }
    m_current = kNotStarted;
    m_last_status = SMStatus::Running;
}

void StateMachine::goto_state(u32 next, Blackboard& bb) {
    SMState& from = *m_states[m_current];
    SMState& to = *m_states[next];
    from.exit(bb);
    m_current = next;
    to.enter(bb);
    ++m_transition_count;
}

} // namespace nf::ai
