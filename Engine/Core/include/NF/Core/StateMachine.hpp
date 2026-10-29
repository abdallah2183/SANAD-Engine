#pragma once

#include <NF/Core/Types.hpp>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <memory>

namespace nf {

using StateId = uint32_t;
using EventId = uint32_t;

static constexpr StateId kInvalidStateId = 0xFFFFFFFFu;
static constexpr EventId kAnyEvent = 0xFFFFFFFFu;

/// Generic, type-safe Hierarchical State Machine (HFSM / StateChart).
/// Supports nested parent/child states, guarded transitions, and state history.
class HierarchicalStateMachine {
public:
    using GuardFn = std::function<bool()>;
    using ActionFn = std::function<void()>;
    using UpdateFn = std::function<void(float dt)>;

    struct StateNode {
        StateId id = kInvalidStateId;
        std::string name;
        StateId parent_id = kInvalidStateId;

        ActionFn on_enter;
        UpdateFn on_update;
        ActionFn on_exit;
    };

    struct Transition {
        StateId from = kInvalidStateId;
        StateId to = kInvalidStateId;
        EventId event = kAnyEvent;
        GuardFn guard;
        ActionFn action;
    };

    HierarchicalStateMachine() = default;

    /// Registers a state. If parent_id is specified, the state is treated as a sub-state.
    StateId add_state(StateId id, const std::string& name, StateId parent_id = kInvalidStateId);

    /// Sets lifecycle callbacks for a registered state.
    bool set_callbacks(StateId id, ActionFn on_enter, UpdateFn on_update = nullptr, ActionFn on_exit = nullptr);

    /// Adds a transition between two states triggered by an event, with an optional guard.
    void add_transition(StateId from, StateId to, EventId event = kAnyEvent, GuardFn guard = nullptr, ActionFn action = nullptr);

    /// Initializes and starts the state machine at the specified initial state.
    bool start(StateId initial_state);

    /// Dispatches an event, evaluating outgoing transitions from current active state.
    bool dispatch_event(EventId event);

    /// Forces a direct transition to the target state without checking guards.
    bool transition_to(StateId target);

    /// Restores the previously active state (history transition).
    bool restore_previous();

    /// Updates the currently active state and its active parents.
    void update(float dt);

    /// Gets the current leaf active state ID.
    StateId current_state() const { return m_current_state; }

    /// Gets the previously active state ID.
    StateId previous_state() const { return m_previous_state; }

    /// Returns true if the state machine is currently in `id` or any of its descendants.
    bool is_in_state(StateId id) const;

    /// Clears all states and transitions.
    void clear();

private:
    std::vector<StateId> get_state_path(StateId leaf) const;

    StateId m_current_state = kInvalidStateId;
    StateId m_previous_state = kInvalidStateId;

    std::unordered_map<StateId, StateNode> m_states;
    std::vector<Transition> m_transitions;
};

} // namespace nf
