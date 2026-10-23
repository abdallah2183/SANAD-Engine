// NF/Runtime/InputBindings.cpp — the binding table and the evaluator.

#include <NF/Runtime/InputBindings.hpp>

#include <NF/Platform/InputSystem.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace nf::runtime {

namespace {

// The numbers in the name tables below are the platform's enums. Asserting
// them here means a change to GamepadButton or GamepadAxis breaks the build
// instead of silently binding "jump" to the wrong button.
static_assert(static_cast<u16>(nf::GamepadButton::A) == 0, "pad button table is stale");
static_assert(static_cast<u16>(nf::GamepadButton::Y) == 3, "pad button table is stale");
static_assert(static_cast<u16>(nf::GamepadButton::LeftBumper) == 4, "pad button table is stale");
static_assert(static_cast<u16>(nf::GamepadButton::DPadUp) == 11, "pad button table is stale");
static_assert(static_cast<u16>(nf::GamepadButton::DPadRight) == 14, "pad button table is stale");
static_assert(static_cast<u16>(nf::GamepadAxis::LeftX) == 0, "pad axis table is stale");
static_assert(static_cast<u16>(nf::GamepadAxis::RightY) == 3, "pad axis table is stale");
static_assert(static_cast<u16>(nf::GamepadAxis::LeftTrigger) == 4, "pad axis table is stale");

/// A magnitude below this is "not held": a stick at rest reports a few
/// thousandths, and an action bound to a stick must not read as pressed.
constexpr f32 kPressThreshold = 0.5f;

[[nodiscard]] bool equals_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

struct NamedCode {
    std::string_view name;
    u16 code;
};

// Virtual-key codes, spelled the way a person writes them in a file. The
// letters and digits are handled arithmetically, not listed.
constexpr std::array<NamedCode, 30> kKeyNames{{
    {"space", 0x20},
    {"esc", 0x1B},
    {"escape", 0x1B},
    {"enter", 0x0D},
    {"return", 0x0D},
    {"tab", 0x09},
    {"backspace", 0x08},
    {"shift", 0x10},
    {"ctrl", 0x11},
    {"control", 0x11},
    {"alt", 0x12},
    {"up", 0x26},
    {"down", 0x28},
    {"left", 0x25},
    {"right", 0x27},
    {"home", 0x24},
    {"end", 0x23},
    {"pageup", 0x21},
    {"pagedown", 0x22},
    {"insert", 0x2D},
    {"delete", 0x2E},
    {"f1", 0x70},
    {"f2", 0x71},
    {"f3", 0x72},
    {"f4", 0x73},
    {"f5", 0x74},
    {"f6", 0x75},
    {"f7", 0x76},
    {"f8", 0x77},
    {"f9", 0x78},
}};

constexpr std::array<NamedCode, 15> kPadButtons{{
    {"a", 0},
    {"b", 1},
    {"x", 2},
    {"y", 3},
    {"lb", 4},
    {"rb", 5},
    {"back", 6},
    {"start", 7},
    {"guide", 8},
    {"ls", 9},
    {"rs", 10},
    {"dpadup", 11},
    {"dpaddown", 12},
    {"dpadleft", 13},
    {"dpadright", 14},
}};

constexpr std::array<NamedCode, 6> kPadAxes{{
    {"leftx", 0},
    {"lefty", 1},
    {"rightx", 2},
    {"righty", 3},
    {"lt", 4},
    {"rt", 5},
}};

// The five mouse buttons a player can bind, by their Windows virtual-key
// codes. Not invented here: the input source already asks the platform layer
// for VK_LBUTTON to detect a click that takes the pointer back, so a binding
// and that click are the same question asked twice — and one table is what
// keeps them from disagreeing.
constexpr std::array<NamedCode, 5> kMouseButtons{{
    {"left", 0x01},   // VK_LBUTTON
    {"right", 0x02},  // VK_RBUTTON
    {"middle", 0x04}, // VK_MBUTTON
    {"x1", 0x05},     // VK_XBUTTON1
    {"x2", 0x06},     // VK_XBUTTON2
}};

[[nodiscard]] bool lookup(const NamedCode* table, size_t count, std::string_view name, u16& out) {
    for (size_t i = 0; i < count; ++i) {
        if (equals_ci(name, table[i].name)) {
            out = table[i].code;
            return true;
        }
    }
    return false;
}

/// One binding, with the sign the token asked for.
[[nodiscard]] InputBinding make_binding(BindingSource source, u16 code, f32 value) {
    return InputBinding{source, code, value};
}

} // namespace

// --- name tables -------------------------------------------------------------

bool key_code_from_name(std::string_view name, u16& out) {
    const std::string_view t = trim(name);
    if (t.size() == 1) {
        const auto c = static_cast<unsigned char>(t.front());
        if (std::isalpha(c) != 0) {
            out = static_cast<u16>(std::toupper(c)); // 'A'..'Z' ARE their VK codes
            return true;
        }
        if (std::isdigit(c) != 0) {
            out = static_cast<u16>(c); // '0'..'9' likewise
            return true;
        }
    }
    return lookup(kKeyNames.data(), kKeyNames.size(), t, out);
}

bool pad_button_from_name(std::string_view name, u16& out) {
    return lookup(kPadButtons.data(), kPadButtons.size(), trim(name), out);
}

bool pad_axis_from_name(std::string_view name, u16& out) {
    return lookup(kPadAxes.data(), kPadAxes.size(), trim(name), out);
}

bool mouse_button_from_name(std::string_view name, u16& out) {
    return lookup(kMouseButtons.data(), kMouseButtons.size(), trim(name), out);
}

std::string_view key_name_from_code(u16 code) {
    if (code >= 'A' && code <= 'Z') {
        // The table is static and the letters are contiguous, so a stable
        // view into a literal is enough — no allocation on the UI path.
        static constexpr std::array<std::string_view, 26> letters{
            "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
            "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
        return letters[code - 'A'];
    }
    if (code >= '0' && code <= '9') {
        static constexpr std::array<std::string_view, 10> digits{"0", "1", "2", "3", "4",
                                                                 "5", "6", "7", "8", "9"};
        return digits[code - '0'];
    }
    for (const auto& n : kKeyNames) {
        if (n.code == code) {
            return n.name;
        }
    }
    return {};
}

std::string_view mouse_button_name_from_code(u16 code) {
    for (const auto& n : kMouseButtons) {
        if (n.code == code) {
            return n.name;
        }
    }
    return {};
}

std::string_view pad_button_name_from_code(u16 code) {
    for (const auto& n : kPadButtons) {
        if (n.code == code) {
            return n.name;
        }
    }
    return {};
}

std::string_view pad_axis_name_from_code(u16 code) {
    for (const auto& n : kPadAxes) {
        if (n.code == code) {
            return n.name;
        }
    }
    return {};
}

// --- the table ---------------------------------------------------------------

void InputBindings::set(std::string_view action, std::vector<InputBinding> bindings) {
    for (auto& e : m_actions) {
        if (e.action == action) {
            e.bindings = std::move(bindings);
            return;
        }
    }
    m_actions.push_back(Entry{std::string(action), std::move(bindings)});
}

std::vector<std::string> InputBindings::actions() const {
    std::vector<std::string> names;
    names.reserve(m_actions.size());
    for (const auto& e : m_actions) {
        names.push_back(e.action);
    }
    return names;
}

void InputBindings::merge(const InputBindings& other) {
    for (const auto& e : other.m_actions) {
        set(e.action, e.bindings);
    }
}

const std::vector<InputBinding>* InputBindings::find(std::string_view action) const {
    for (const auto& e : m_actions) {
        if (e.action == action) {
            return &e.bindings;
        }
    }
    return nullptr;
}

InputBindings InputBindings::defaults() {
    InputBindings b;
    // The three aliases each movement action has always answered to. They are
    // separate entries rather than one entry with three names, because the
    // file format is one action per line and an alias is just another line.
    const std::vector<InputBinding> forward{{BindingSource::Key, 'W', 1.0f},
                                            {BindingSource::Key, 0x26, 1.0f}}; // Up
    const std::vector<InputBinding> back{{BindingSource::Key, 'S', 1.0f},
                                         {BindingSource::Key, 0x28, 1.0f}}; // Down
    const std::vector<InputBinding> left{{BindingSource::Key, 'A', 1.0f},
                                         {BindingSource::Key, 0x25, 1.0f}}; // Left
    const std::vector<InputBinding> right{{BindingSource::Key, 'D', 1.0f},
                                          {BindingSource::Key, 0x27, 1.0f}}; // Right

    b.set("move_forward", forward);
    b.set("forward", forward);
    b.set("up", forward);
    b.set("move_back", back);
    b.set("back", back);
    b.set("down", back);
    b.set("move_left", left);
    b.set("left", left);
    b.set("move_right", right);
    b.set("right", right);

    b.set("jump", {{BindingSource::Key, 0x20, 1.0f},                     // Space
                   {BindingSource::PadButton, 0, 1.0f}});                // A
    b.set("sprint", {{BindingSource::Key, 0x10, 1.0f},                   // Shift
                     {BindingSource::PadButton, 4, 1.0f},                // LB
                     {BindingSource::PadAxis, 4, 1.0f}});                // LT
    b.set("interact", {{BindingSource::Key, 'E', 1.0f},
                       {BindingSource::PadButton, 2, 1.0f}});            // X

    // Aiming down sights. NOT part of the old ladder — there was no aiming to
    // bind — but it is additive: no existing action changes, and a scene that
    // never enables ADS simply never reads it. Right mouse and the right
    // trigger are where every third-person game puts it.
    b.set("aim", {{BindingSource::MouseButton, 0x02, 1.0f},              // RMB
                  {BindingSource::PadAxis, 5, 1.0f}});                   // RT

    // Legacy orbit axes, kept so a scene written for OrbitCameraModule still
    // reads something sensible.
    b.set("look", {{BindingSource::Key, 0x25, -1.0f}, {BindingSource::Key, 0x27, 1.0f}});
    b.set("zoom", {{BindingSource::Key, 0x26, -1.0f}, {BindingSource::Key, 0x28, 1.0f}});

    // The movement axes: two keys with opposite signs plus the left stick.
    // XInput reports +Y as UP and the engine's move_z is +1 for BACK, so the
    // stick's Y contribution is negated — the same correction the hand-written
    // ladder made, now expressed where it can be seen.
    b.set("move_x", {{BindingSource::Key, 'A', -1.0f},
                     {BindingSource::Key, 'D', 1.0f},
                     {BindingSource::Key, 0x25, -1.0f},
                     {BindingSource::Key, 0x27, 1.0f},
                     {BindingSource::PadAxis, 0, 1.0f}});                // LeftX
    b.set("move_z", {{BindingSource::Key, 'W', -1.0f},
                     {BindingSource::Key, 'S', 1.0f},
                     {BindingSource::Key, 0x26, -1.0f},
                     {BindingSource::Key, 0x28, 1.0f},
                     {BindingSource::PadAxis, 1, -1.0f}});               // LeftY
    b.set("look_x", {{BindingSource::Key, 0x25, -1.0f},
                     {BindingSource::Key, 0x27, 1.0f},
                     {BindingSource::PadAxis, 2, 1.0f}});                // RightX
    b.set("look_y", {{BindingSource::Key, 0x26, -1.0f},
                     {BindingSource::Key, 0x28, 1.0f},
                     {BindingSource::PadAxis, 3, -1.0f}});               // RightY
    return b;
}

InputBindings InputBindings::parse(std::string_view text, std::string* error) {
    InputBindings out;
    size_t line_no = 0;
    size_t pos = 0;
    auto report = [&](const std::string& what) {
        if (error != nullptr && error->empty()) {
            *error = "line " + std::to_string(line_no) + ": " + what;
        }
    };

    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos)
                                    ? text.substr(pos)
                                    : text.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        ++line_no;

        // A '#' starts a comment wherever it appears: a binding file is read
        // by people, and the reason for a rebind belongs next to the rebind.
        if (const size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            report("expected 'action = bindings'");
            continue;
        }
        const std::string_view action = trim(line.substr(0, eq));
        if (action.empty()) {
            report("missing action name");
            continue;
        }

        std::vector<InputBinding> bindings;
        std::string_view rest = line.substr(eq + 1);
        bool bad = false;
        while (!rest.empty() && !bad) {
            const size_t comma = rest.find(',');
            std::string_view token = (comma == std::string_view::npos)
                                         ? rest
                                         : rest.substr(0, comma);
            rest = (comma == std::string_view::npos) ? std::string_view{} : rest.substr(comma + 1);
            token = trim(token);
            if (token.empty()) {
                continue;
            }

            f32 sign = 1.0f;
            if (token.front() == '+') {
                token.remove_prefix(1);
            } else if (token.front() == '-') {
                sign = -1.0f;
                token.remove_prefix(1);
            }

            const size_t colon = token.find(':');
            if (colon == std::string_view::npos) {
                report("expected key:NAME, pad:NAME or axis:NAME");
                bad = true;
                break;
            }
            const std::string_view kind = trim(token.substr(0, colon));
            const std::string_view name = trim(token.substr(colon + 1));
            u16 code = 0;
            if (equals_ci(kind, "key")) {
                if (!key_code_from_name(name, code)) {
                    report("unknown key '" + std::string(name) + "'");
                    bad = true;
                    break;
                }
                bindings.push_back(make_binding(BindingSource::Key, code, sign));
            } else if (equals_ci(kind, "pad")) {
                if (!pad_button_from_name(name, code)) {
                    report("unknown pad button '" + std::string(name) + "'");
                    bad = true;
                    break;
                }
                bindings.push_back(make_binding(BindingSource::PadButton, code, sign));
            } else if (equals_ci(kind, "axis")) {
                if (!pad_axis_from_name(name, code)) {
                    report("unknown pad axis '" + std::string(name) + "'");
                    bad = true;
                    break;
                }
                bindings.push_back(make_binding(BindingSource::PadAxis, code, sign));
            } else if (equals_ci(kind, "mouse")) {
                if (!mouse_button_from_name(name, code)) {
                    report("unknown mouse button '" + std::string(name) + "'");
                    bad = true;
                    break;
                }
                bindings.push_back(make_binding(BindingSource::MouseButton, code, sign));
            } else {
                report("unknown binding kind '" + std::string(kind) + "'");
                bad = true;
                break;
            }
        }
        if (!bad) {
            out.set(action, std::move(bindings));
        }
    }
    return out;
}

// --- evaluation --------------------------------------------------------------

bool binding_pressed(const InputBindings& bindings, std::string_view action,
                     const DeviceState& device) {
    const std::vector<InputBinding>* list = bindings.find(action);
    if (list == nullptr) {
        return false;
    }
    for (const auto& b : *list) {
        switch (b.source) {
        case BindingSource::Key:
            if (device.key_down != nullptr && device.key_down(device.context, b.code)) {
                return true;
            }
            break;
        case BindingSource::PadButton:
            if (device.pad_down != nullptr && device.pad_down(device.context, b.code)) {
                return true;
            }
            break;
        case BindingSource::PadAxis:
            if (device.pad_axis != nullptr &&
                std::fabs(device.pad_axis(device.context, b.code) * b.value) >= kPressThreshold) {
                return true;
            }
            break;
        case BindingSource::MouseButton:
            if (device.mouse_down != nullptr && device.mouse_down(device.context, b.code)) {
                return true;
            }
            break;
        }
    }
    return false;
}

f32 binding_axis(const InputBindings& bindings, std::string_view action, const DeviceState& device) {
    const std::vector<InputBinding>* list = bindings.find(action);
    if (list == nullptr) {
        return 0.0f;
    }
    f32 sum = 0.0f;
    for (const auto& b : *list) {
        switch (b.source) {
        case BindingSource::Key:
            if (device.key_down != nullptr && device.key_down(device.context, b.code)) {
                sum += b.value;
            }
            break;
        case BindingSource::PadButton:
            if (device.pad_down != nullptr && device.pad_down(device.context, b.code)) {
                sum += b.value;
            }
            break;
        case BindingSource::PadAxis:
            if (device.pad_axis != nullptr) {
                sum += device.pad_axis(device.context, b.code) * b.value;
            }
            break;
        case BindingSource::MouseButton:
            if (device.mouse_down != nullptr && device.mouse_down(device.context, b.code)) {
                sum += b.value;
            }
            break;
        }
    }
    // Two keys pressed against each other cancel to zero, and a key plus a
    // stick at full deflection must still read as one axis, not 1.6.
    return std::clamp(sum, -1.0f, 1.0f);
}


// --- writing the table back out ----------------------------------------------

namespace {

/// One token, e.g. `-key:A`. The sign is the only thing the format can carry
/// beyond the code, so a binding whose value is not +/-1 is written as its
/// sign: the file has no syntax for a scale, and inventing one here would
/// make this writer produce files the parser cannot read.
std::string token_for(const InputBinding& b) {
    std::string out;
    if (b.value < 0.0f) {
        out.push_back('-');
    }
    switch (b.source) {
    case BindingSource::Key:
        out += "key:";
        out += std::string(key_name_from_code(b.code));
        break;
    case BindingSource::PadButton:
        out += "pad:";
        out += std::string(pad_button_name_from_code(b.code));
        break;
    case BindingSource::PadAxis:
        out += "axis:";
        out += std::string(pad_axis_name_from_code(b.code));
        break;
    case BindingSource::MouseButton:
        out += "mouse:";
        out += std::string(mouse_button_name_from_code(b.code));
        break;
    }
    return out;
}

} // namespace

std::string serialize_bindings(const InputBindings& bindings) {
    std::string out;
    out += "# SANAD input bindings — written by the editor, safe to hand-edit.\n";
    out += "# action = token, token, ...   tokens: key:NAME, pad:NAME, axis:NAME, mouse:NAME\n";
    for (const auto& [action, list] : binding_rows(bindings)) {
        out += action;
        out += " =";
        for (size_t i = 0; i < list.size(); ++i) {
            out += (i == 0) ? " " : ", ";
            out += token_for(list[i]);
        }
        out += "\n";
    }
    return out;
}

std::vector<std::pair<std::string, std::vector<InputBinding>>>
binding_rows(const InputBindings& bindings) {
    std::vector<std::pair<std::string, std::vector<InputBinding>>> rows;
    rows.reserve(bindings.action_count());
    for (const auto& action : bindings.actions()) {
        rows.emplace_back(action, *bindings.find(action));
    }
    return rows;
}

} // namespace nf::runtime
