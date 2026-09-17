#pragma once

// NF/Input/InputContext.hpp — A named, prioritized set of actions (design doc §50).
//
// Contexts separate input by gameplay situation: "gameplay", "menu", "driving".
// A context may block lower-priority contexts while active, so opening a menu
// can suspend gameplay actions without unmapping them. Priority is a plain
// integer; higher wins. Blocking is one-way: the menu blocks gameplay, but
// gameplay never blocks the menu.
//
// Action names must be unique within a mapper; a duplicate across contexts is
// refused at registration so a query can never resolve ambiguously.

#include <NF/Core/Containers.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Input/InputAction.hpp>

#include <string>
#include <string_view>

namespace nf::input {

class InputContext {
public:
    InputContext() = default;
    InputContext(std::string name, i32 priority)
        : m_name(std::move(name)), m_priority(priority) {}

    const std::string& name() const { return m_name; }
    i32 priority() const { return m_priority; }

    void set_active(bool active) { m_active = active; }
    bool active() const { return m_active; }

    /// While active and blocking, every lower-priority context is suppressed.
    void set_blocking(bool blocks) { m_blocking = blocks; }
    bool blocking() const { return m_blocking; }

    void add_action(InputAction action);
    InputAction* find_action(std::string_view name);
    const InputAction* find_action(std::string_view name) const;

    const DynamicArray<InputAction>& actions() const { return m_actions; }
    usize action_count() const { return m_actions.size(); }

private:
    std::string m_name;
    i32 m_priority = 0;
    bool m_active = true;
    bool m_blocking = false;
    DynamicArray<InputAction> m_actions;
};

} // namespace nf::input
