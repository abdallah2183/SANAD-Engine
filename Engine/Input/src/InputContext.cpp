// NF/Input/InputContext.cpp

#include <NF/Input/InputContext.hpp>

#include <NF/Core/Assert.hpp>

namespace nf::input {

void InputContext::add_action(InputAction action) {
    NF_ASSERT(!action.name().empty(), "InputContext::add_action: empty name");
    if (find_action(action.name()) != nullptr) {
        // A duplicate name inside one context would make queries ambiguous.
        NF_ASSERT(false, "InputContext::add_action: duplicate action name");
        return;
    }
    m_actions.push_back(std::move(action));
}

InputAction* InputContext::find_action(std::string_view name) {
    for (InputAction& action : m_actions) {
        if (action.name() == name) return &action;
    }
    return nullptr;
}

const InputAction* InputContext::find_action(std::string_view name) const {
    for (const InputAction& action : m_actions) {
        if (action.name() == name) return &action;
    }
    return nullptr;
}

} // namespace nf::input
