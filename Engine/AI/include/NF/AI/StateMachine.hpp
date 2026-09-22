#pragma once

// NF/AI/StateMachine.hpp — finite state machines (design doc Section 45,
// "AI Core: state machines").
//
// Behavior trees are the engine's default decision layer, but not every NPC
// wants one. A patrol drone that cycles Idle -> Patrol -> Alert -> Attack is a
// handful of states and transitions, and expressing it as a tree buries the
// shape of the thing under composites. This module is that shape: states own
// their enter/exit/tick, transitions are guarded predicates on a Blackboard.
//
// The two layers are deliberately the same shape:
//   - both read and write a Blackboard, never world state directly, so a test
//     needs no scene, no physics and no device;
//   - both report BTStatus/SMStatus, so a game can nest them — one state's tick
//     may run a whole BehaviorTree, and the tree's leaves may flip the flags a
//     transition guards on.
//
// Semantics (documented, not accidental):
//   - a tick runs the active state's tick, then evaluates that state's
//     transitions in insertion order; the first guard that holds wins, the rest
//     are ignored. Ordering is the whole contract — a tie between two guards is
//     resolved by authoring order, never by hash or pointer;
//   - enter/exit bracket the occupancy: exit runs after tick, before the new
//     state's enter, so a state always sees its own enter before its tick;
//   - a transition to the state already active is skipped, not re-entered. A
//     state that must restart writes a flag the guard can key on instead;
//   - ids are vector indices and states are added once, at authoring time.
//     Removing a state would shift every id above it and silently rewire
//     existing transitions, so the API does not offer it — a dead state is
//     simply one with no incoming transitions;
//   - no RNG, no wall clock. `dt` is the only time input, and a negative dt is
//     clamped to zero.
//
// Units: dt is seconds.

#include <NF/AI/BehaviorTree.hpp> // Blackboard
#include <NF/Core/Types.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace nf::ai {

/// What a state's tick returned. Deliberately the same enumerators as BTStatus
/// so a state may forward a nested tree's result unchanged.
enum class SMStatus : u8 { Success = 0, Failure = 1, Running = 2 };

/// One node of the machine. Callbacks default to no-ops, so a state can be as
/// thin as a marker (enter sets a flag) or as thick as a full action driver.
/// A state must not transition the machine from enter/exit: the transition pass
/// runs after tick, and moving the machine during the occupancy bracket would
/// leave the bookkeeping inconsistent. Set a flag; let a guard pick it up.
class SMState {
public:
    virtual ~SMState() = default;

    /// Once, on entering. A state entered by force_state() and by a guard fires
    /// the same callback — there is no "was this scripted?" channel, because a
    /// stun and a natural alert both mean "stop what you were doing".
    virtual void enter(Blackboard& bb) { (void)bb; }

    /// Once, on leaving. Runs even when the machine is reset while in it.
    virtual void exit(Blackboard& bb) { (void)bb; }

    /// Every tick while active, before transitions are considered. The return
    /// value is reported to the caller but does not itself move the machine —
    /// see the note about the single transition channel.
    virtual SMStatus tick(Blackboard& bb, float dt) {
        (void)bb;
        (void)dt;
        return SMStatus::Running;
    }
};

/// "When in `from` and `guard` holds, go to `to`." Guards are predicates over
/// the Blackboard rather than methods on the state so that the machine's wiring
/// is data: the same state composes differently under two guards, and a test
/// asserts a transition without instantiating a world.
struct SMTransition {
    u32 from = 0;
    u32 to = 0;
    std::function<bool(const Blackboard&)> guard;
};

class StateMachine {
public:
    StateMachine() = default;

    /// Appends a state and returns its id. Ids are insertion order and are never
    /// recycled, so a transition authored against id 3 always means "the fourth
    /// state added".
    u32 add_state(std::unique_ptr<SMState> state);

    /// Appends a transition and returns its index among transitions from the
    /// same state. That index is the tie-break order, so author the guard you
    /// want to win first.
    u32 add_transition(u32 from, u32 to, std::function<bool(const Blackboard&)> guard);

    /// Must be called before the first tick. A machine with no states cannot
    /// tick, and starting on a state other than 0 is a scripted setup choice.
    void set_start(u32 state);

    /// Runs the active state and then resolves one transition. The status
    /// returned is the active state's; a transition that fires this tick does
    /// not change what is reported. Negative dt clamps to zero.
    SMStatus tick(Blackboard& bb, float dt);

    /// Moves to `state` unconditionally, running exit and enter. This is the
    /// channel for things that happen *to* an NPC rather than by it: a stun, a
    /// possession, a scripted cue. Returns whether the machine actually moved.
    bool force_state(u32 state, Blackboard& bb);

    /// The active state's id, or the sentinel before the first tick.
    u32 current() const { return m_current; }

    /// The active state's status from its most recent tick.
    SMStatus last_status() const { return m_last_status; }

    /// How many transitions have fired since construction. A replay asserts this
    /// alongside every blackboard value — it is the shortest proof that two runs
    /// took the same path, and the cheapest place to catch a divergence.
    u32 transition_count() const { return m_transition_count; }

    /// Runs the exit of the active state (if any) and returns to the
    /// "not started" sentinel, without touching the states or transitions. For
    /// a respawn: the same machine, a fresh occupancy.
    void reset(Blackboard& bb);

    /// Read-only access for editors and debuggers. Order is insertion order.
    const std::vector<std::unique_ptr<SMState>>& states() const { return m_states; }
    const std::vector<SMTransition>& transitions() const { return m_transitions; }

    /// Before the first tick and after reset(). A machine in this state ticks
    /// nothing and reports Running, so a game that forgot set_start degrades to
    /// "NPC stands there" rather than crashing.
    static inline constexpr u32 kNotStarted = 0xFFFFFFFFu;

private:
    void goto_state(u32 next, Blackboard& bb);

    std::vector<std::unique_ptr<SMState>> m_states;
    std::vector<SMTransition> m_transitions;
    u32 m_current = kNotStarted;
    SMStatus m_last_status = SMStatus::Running;
    u32 m_transition_count = 0;
};

} // namespace nf::ai
