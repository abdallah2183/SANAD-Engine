#pragma once

// NF/Input/InputNames.hpp — Enum <-> string tables for input enums.
//
// Two consumers: binding serialization (InputMapper::to_string/load_string) and
// the editor's rebinding UI, which has to show "GamepadAxis::LeftX" to a human
// and parse what they type back. Centralizing the tables means a new key code
// is spelled the same way everywhere.

#include <NF/Core/Types.hpp>
#include <NF/Input/InputAction.hpp>
#include <NF/Platform/InputSystem.hpp>

#include <string_view>

namespace nf::input {

// --- to string -----------------------------------------------------------
std::string_view to_string(KeyCode key);
std::string_view to_string(MouseButton button);
std::string_view to_string(GamepadButton button);
std::string_view to_string(GamepadAxis axis);
std::string_view to_string(ActionType type);
std::string_view to_string(AxisComponent component);
std::string_view to_string(BindingSource source);

// --- from string ---------------------------------------------------------
// Return false when the token matches nothing (caller decides warn vs. abort).
bool parse_key(std::string_view token, KeyCode& out);
bool parse_mouse_button(std::string_view token, MouseButton& out);
bool parse_gamepad_button(std::string_view token, GamepadButton& out);
bool parse_gamepad_axis(std::string_view token, GamepadAxis& out);
bool parse_action_type(std::string_view token, ActionType& out);
bool parse_axis_component(std::string_view token, AxisComponent& out);
bool parse_binding_source(std::string_view token, BindingSource& out);

} // namespace nf::input
