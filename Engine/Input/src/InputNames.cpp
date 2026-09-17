// NF/Input/InputNames.cpp

#include <NF/Input/InputNames.hpp>

#include <cstring>

namespace nf::input {

namespace {

struct NameEntry {
    std::string_view name;
};

// Tables are indexed by enum value; every table ends with the sentinel `Count`
// so a stray value can never index out of bounds.

constexpr NameEntry key_names[] = {
    {"Unknown"},
    {"A"}, {"B"}, {"C"}, {"D"}, {"E"}, {"F"}, {"G"}, {"H"}, {"I"}, {"J"},
    {"K"}, {"L"}, {"M"}, {"N"}, {"O"}, {"P"}, {"Q"}, {"R"}, {"S"}, {"T"},
    {"U"}, {"V"}, {"W"}, {"X"}, {"Y"}, {"Z"},
    {"Num0"}, {"Num1"}, {"Num2"}, {"Num3"}, {"Num4"},
    {"Num5"}, {"Num6"}, {"Num7"}, {"Num8"}, {"Num9"},
    {"F1"}, {"F2"}, {"F3"}, {"F4"}, {"F5"}, {"F6"},
    {"F7"}, {"F8"}, {"F9"}, {"F10"}, {"F11"}, {"F12"},
    {"Up"}, {"Down"}, {"Left"}, {"Right"},
    {"Home"}, {"End"}, {"PageUp"}, {"PageDown"}, {"Insert"}, {"Delete"},
    {"LeftShift"}, {"RightShift"}, {"LeftCtrl"}, {"RightCtrl"},
    {"LeftAlt"}, {"RightAlt"}, {"LeftSuper"}, {"RightSuper"},
    {"Space"}, {"Enter"}, {"Escape"}, {"Tab"}, {"Backspace"},
    {"CapsLock"}, {"ScrollLock"}, {"PrintScreen"}, {"Pause"},
    {"LeftBracket"}, {"RightBracket"},
    {"Semicolon"}, {"Apostrophe"}, {"Comma"}, {"Period"},
    {"Slash"}, {"Backslash"}, {"Grave"},
    {"Numpad0"}, {"Numpad1"}, {"Numpad2"}, {"Numpad3"}, {"Numpad4"},
    {"Numpad5"}, {"Numpad6"}, {"Numpad7"}, {"Numpad8"}, {"Numpad9"},
    {"NumpadEnter"}, {"NumpadAdd"}, {"NumpadSubtract"},
    {"NumpadMultiply"}, {"NumpadDivide"}, {"NumpadDecimal"},
    {"Count"},
};

constexpr NameEntry mouse_names[] = {
    {"Left"}, {"Right"}, {"Middle"}, {"X1"}, {"X2"}, {"Count"},
};

constexpr NameEntry pad_button_names[] = {
    {"A"}, {"B"}, {"X"}, {"Y"},
    {"LeftBumper"}, {"RightBumper"},
    {"Back"}, {"Start"}, {"Guide"},
    {"LeftThumb"}, {"RightThumb"},
    {"DPadUp"}, {"DPadDown"}, {"DPadLeft"}, {"DPadRight"},
    {"Count"},
};

constexpr NameEntry pad_axis_names[] = {
    {"LeftX"}, {"LeftY"}, {"RightX"}, {"RightY"},
    {"LeftTrigger"}, {"RightTrigger"}, {"Count"},
};

constexpr NameEntry action_type_names[] = {
    {"Bool"}, {"Axis1D"}, {"Axis2D"},
};

constexpr NameEntry axis_component_names[] = {
    {"None"}, {"X"}, {"Y"},
};

constexpr NameEntry binding_source_names[] = {
    {"Key"}, {"Mouse"}, {"GamepadButton"}, {"GamepadAxis"},
};

template <usize N>
std::string_view table_name(const NameEntry (&table)[N], usize index) {
    if (index >= N) return "Unknown";
    return table[index].name;
}

template <usize N>
bool table_parse(const NameEntry (&table)[N], std::string_view token, usize& out) {
    for (usize i = 0; i < N; ++i) {
        if (token.size() == table[i].name.size() &&
            std::memcmp(token.data(), table[i].name.data(), token.size()) == 0) {
            out = i;
            return true;
        }
    }
    return false;
}

} // namespace

// --- to string -----------------------------------------------------------

std::string_view to_string(KeyCode key) {
    return table_name(key_names, static_cast<usize>(key));
}

std::string_view to_string(MouseButton button) {
    return table_name(mouse_names, static_cast<usize>(button));
}

std::string_view to_string(GamepadButton button) {
    return table_name(pad_button_names, static_cast<usize>(button));
}

std::string_view to_string(GamepadAxis axis) {
    return table_name(pad_axis_names, static_cast<usize>(axis));
}

std::string_view to_string(ActionType type) {
    return table_name(action_type_names, static_cast<usize>(type));
}

std::string_view to_string(AxisComponent component) {
    return table_name(axis_component_names, static_cast<usize>(component));
}

std::string_view to_string(BindingSource source) {
    return table_name(binding_source_names, static_cast<usize>(source));
}

// --- from string ---------------------------------------------------------

bool parse_key(std::string_view token, KeyCode& out) {
    usize index = 0;
    if (!table_parse(key_names, token, index)) return false;
    out = static_cast<KeyCode>(index);
    return true;
}

bool parse_mouse_button(std::string_view token, MouseButton& out) {
    usize index = 0;
    if (!table_parse(mouse_names, token, index)) return false;
    out = static_cast<MouseButton>(index);
    return true;
}

bool parse_gamepad_button(std::string_view token, GamepadButton& out) {
    usize index = 0;
    if (!table_parse(pad_button_names, token, index)) return false;
    out = static_cast<GamepadButton>(index);
    return true;
}

bool parse_gamepad_axis(std::string_view token, GamepadAxis& out) {
    usize index = 0;
    if (!table_parse(pad_axis_names, token, index)) return false;
    out = static_cast<GamepadAxis>(index);
    return true;
}

bool parse_action_type(std::string_view token, ActionType& out) {
    usize index = 0;
    if (!table_parse(action_type_names, token, index)) return false;
    out = static_cast<ActionType>(index);
    return true;
}

bool parse_axis_component(std::string_view token, AxisComponent& out) {
    usize index = 0;
    if (!table_parse(axis_component_names, token, index)) return false;
    out = static_cast<AxisComponent>(index);
    return true;
}

bool parse_binding_source(std::string_view token, BindingSource& out) {
    usize index = 0;
    if (!table_parse(binding_source_names, token, index)) return false;
    out = static_cast<BindingSource>(index);
    return true;
}

} // namespace nf::input
