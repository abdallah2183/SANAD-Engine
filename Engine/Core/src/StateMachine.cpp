#include <NF/Core/StateMachine.hpp>
#include <algorithm>

namespace nf {

StateId HierarchicalStateMachine::add_state(StateId id, const std::string& name, StateId parent_id) {
    StateNode node;
    node.id = id;
    node.name = name;
    node.parent_id = parent_id;
    m_states[id] = std::move(node);
    return id;
}

bool HierarchicalStateMachine::set_callbacks(StateId id, ActionFn on_enter, UpdateFn on_update, ActionFn on_exit) {
    auto it = m_states.find(id);
    if (it == m_states.end()) return false;
    it->second.on_enter = std::move(on_enter);
    it->second.on_update = std::move(on_update);
    it->second.on_exit = std::move(on_exit);
    return true;
}

void HierarchicalStateMachine::add_transition(StateId from, StateId to, EventId event, GuardFn guard, ActionFn action) {
    Transition t;
    t.from = from;
    t.to = to;
    t.event = event;
    t.guard = std::move(guard);
    t.action = std::move(action);
    m_transitions.push_back(std::move(t));
}

std::vector<StateId> HierarchicalStateMachine::get_state_path(StateId leaf) const {
    std::vector<StateId> path;
    StateId curr = leaf;
    while (curr != kInvalidStateId) {
        path.push_back(curr);
        auto it = m_states.find(curr);
        if (it != m_states.end()) {
            curr = it->second.parent_id;
        } else {
            break;
        }
    }
    std::reverse(path.begin(), path.end()); // Root to leaf
    return path;
}

bool HierarchicalStateMachine::start(StateId initial_state) {
    auto it = m_states.find(initial_state);
    if (it == m_states.end()) return false;

    m_current_state = initial_state;
    m_previous_state = kInvalidStateId;

    // Enter root-to-leaf hierarchy
    auto path = get_state_path(initial_state);
    for (StateId s : path) {
        if (m_states[s].on_enter) {
            m_states[s].on_enter();
        }
    }
    return true;
}

bool HierarchicalStateMachine::dispatch_event(EventId event) {
    if (m_current_state == kInvalidStateId) return false;

    // Check transitions from current state or any active ancestor
    StateId curr = m_current_state;
    while (curr != kInvalidStateId) {
        for (const auto& t : m_transitions) {
            if (t.from == curr && (t.event == kAnyEvent || t.event == event)) {
                if (!t.guard || t.guard()) {
                    if (t.action) {
                        t.action();
                    }
                    return transition_to(t.to);
                }
            }
        }
        auto it = m_states.find(curr);
        curr = (it != m_states.end()) ? it->second.parent_id : kInvalidStateId;
    }
    return false;
}

bool HierarchicalStateMachine::transition_to(StateId target) {
    auto it = m_states.find(target);
    if (it == m_states.end()) return false;
    if (m_current_state == target) return true;

    auto old_path = get_state_path(m_current_state);
    auto new_path = get_state_path(target);

    // Find Lowest Common Ancestor (LCA)
    size_t lca_len = 0;
    while (lca_len < old_path.size() && lca_len < new_path.size() && old_path[lca_len] == new_path[lca_len]) {
        ++lca_len;
    }

    // Exit old branch from leaf up to LCA
    for (size_t i = old_path.size(); i > lca_len; --i) {
        StateId s = old_path[i - 1];
        if (m_states[s].on_exit) {
            m_states[s].on_exit();
        }
    }

    m_previous_state = m_current_state;
    m_current_state = target;

    // Enter new branch from LCA down to leaf
    for (size_t i = lca_len; i < new_path.size(); ++i) {
        StateId s = new_path[i];
        if (m_states[s].on_enter) {
            m_states[s].on_enter();
        }
    }

    return true;
}

bool HierarchicalStateMachine::restore_previous() {
    if (m_previous_state == kInvalidStateId) return false;
    return transition_to(m_previous_state);
}

void HierarchicalStateMachine::update(float dt) {
    if (m_current_state == kInvalidStateId) return;

    auto path = get_state_path(m_current_state);
    for (StateId s : path) {
        if (m_states[s].on_update) {
            m_states[s].on_update(dt);
        }
    }
}

bool HierarchicalStateMachine::is_in_state(StateId id) const {
    if (m_current_state == kInvalidStateId) return false;

    StateId curr = m_current_state;
    while (curr != kInvalidStateId) {
        if (curr == id) return true;
        auto it = m_states.find(curr);
        curr = (it != m_states.end()) ? it->second.parent_id : kInvalidStateId;
    }
    return false;
}

void HierarchicalStateMachine::clear() {
    m_states.clear();
    m_transitions.clear();
    m_current_state = kInvalidStateId;
    m_previous_state = kInvalidStateId;
}

} // namespace nf
