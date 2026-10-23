#pragma once

// NF/Runtime/InputBindings.hpp — which physical control means which action.
//
// Until now the answer was hardcoded: `action_pressed` was a ladder of
// `if (action == "jump") return key_down(VK_SPACE)`, and a game that wanted
// jump on a mouse button had to replace the whole input source. That is fine
// for an engine demo and wrong for a game engine — rebinding is the first
// thing a player asks for and the first thing a shipped game needs.
//
// So the vocabulary moved out of the code and into data:
//
//     # Content/Input/bindings.nfinput
//     move_forward = key:W, key:Up
//     jump         = key:Space, pad:A
//     move_x       = -key:A, +key:D, axis:LeftX
//     attack       = mouse:Left, pad:X
//
// The file is optional. With no file the built-in defaults are used, and the
// defaults are exactly what the hardcoded ladder did — so a game that never
// writes a binding file behaves as it did, and the sample games keep working.
//
// The evaluation is a pure function of (bindings, action, device state). The
// device is a small table of function pointers rather than a class, so a test
// can press keys on a machine with no keyboard and read a pad on a machine
// with no pad — which is the only way this logic can be proven in CI.

#include <NF/Core/Types.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nf::runtime {

/// Where a binding reads from.
enum class BindingSource : u8 {
    Key = 0,     ///< A keyboard virtual-key code.
    PadButton,   ///< An XInput-style digital button.
    PadAxis,     ///< An XInput-style analogue axis.
    MouseButton, ///< A mouse button, by virtual-key code (VK_LBUTTON..VK_XBUTTON2).
};

/// One physical control bound to one action.
///
/// `value` is what the control CONTRIBUTES to the action's axis: +1 for a
/// positive digital binding, -1 for a negative one (the `-key:A` in
/// `move_x`), and the scale for an analogue axis. A button bound to an
/// action that is only ever queried as pressed ignores it.
struct InputBinding {
    BindingSource source = BindingSource::Key;
    u16 code = 0;
    f32 value = 1.0f;
};

/// Every action and what it is bound to.
class InputBindings {
public:
    /// The bindings the engine shipped with before this file existed.
    ///
    /// Written out rather than derived, because they ARE the compatibility
    /// promise: a game built against 0.4.0 must keep responding to WASD.
    static InputBindings defaults();

    /// Parse a binding file.
    ///
    /// Blank lines and `#` comments are skipped. Every other line is
    /// `action = token, token, ...`; a token is `key:NAME`, `pad:NAME`,
    /// `axis:NAME` or `mouse:NAME`, each optionally prefixed with `+` or `-`.
    /// Names are case-insensitive.
    ///
    /// A malformed file is not fatal: the bindings that parsed are kept and
    /// `error` describes the first line that did not. The caller decides
    /// whether a partial file is better than the defaults — it usually is
    /// not, which is why the runtime logs the error and keeps the defaults
    /// when one is reported.
    static InputBindings parse(std::string_view text, std::string* error);

    /// Replaces (or adds) the bindings for one action.
    void set(std::string_view action, std::vector<InputBinding> bindings);

    /// Copies every action of `other` over this table, replacing per action.
    ///
    /// This is how a binding file is applied: it starts from the defaults and
    /// the file overrides only the actions it names. A file that rebinds jump
    /// and nothing else must not silently delete `interact` — a control scheme
    /// with holes is the hardest kind of bug for a player to describe.
    void merge(const InputBindings& other);

    /// The bindings for `action`, or null when the action is unknown.
    [[nodiscard]] const std::vector<InputBinding>* find(std::string_view action) const;

    [[nodiscard]] bool empty() const { return m_actions.empty(); }
    [[nodiscard]] size_t action_count() const { return m_actions.size(); }
    /// The action names, in insertion order. For a rebinding UI that has to
    /// list them, and for the writer, which must emit them in a stable order.
    [[nodiscard]] std::vector<std::string> actions() const;

private:
    struct Entry {
        std::string action;
        std::vector<InputBinding> bindings;
    };
    std::vector<Entry> m_actions;
};

/// The live device, as the binding evaluator sees it.
///
/// Function pointers and an opaque context, not `std::function`: this is read
/// once per action per script per frame, and a heap allocation there is a
/// tax on every game for a feature most of them never rebind.
struct DeviceState {
    const void* context = nullptr;
    bool (*key_down)(const void*, u16) = nullptr;
    bool (*pad_down)(const void*, u16) = nullptr;
    f32 (*pad_axis)(const void*, u16) = nullptr;
    /// Mouse buttons, by virtual-key code. Null on a device with no mouse —
    /// a `mouse:Left` binding then simply never reads as pressed, which is the
    /// same contract the other three have.
    bool (*mouse_down)(const void*, u16) = nullptr;
};

/// Is any of the action's bindings active?
///
/// "Active" for an axis is a magnitude past a small threshold, so a resting
/// stick does not read as a held button.
[[nodiscard]] bool binding_pressed(const InputBindings& bindings, std::string_view action,
                                   const DeviceState& device);

/// The action's axis: the sum of its bindings' contributions, clamped.
///
/// Summing is what makes `-key:A, +key:D` a single axis and what makes a key
/// and a stick additive rather than one silently winning.
[[nodiscard]] f32 binding_axis(const InputBindings& bindings, std::string_view action,
                               const DeviceState& device);

// --- name tables ------------------------------------------------------------
//
// Public because the parser is not the only consumer: an editor that wants to
// show "W" instead of 0x57 needs the same names, and a test that types a
// binding needs to know what is legal.

[[nodiscard]] bool key_code_from_name(std::string_view name, u16& out);
[[nodiscard]] bool pad_button_from_name(std::string_view name, u16& out);
[[nodiscard]] bool pad_axis_from_name(std::string_view name, u16& out);
/// `Left`, `Right`, `Middle`, `X1`, `X2` — the five buttons a player can bind.
[[nodiscard]] bool mouse_button_from_name(std::string_view name, u16& out);

/// The inverse, for UI. Returns an empty view when the code is unknown.
[[nodiscard]] std::string_view key_name_from_code(u16 code);
[[nodiscard]] std::string_view mouse_button_name_from_code(u16 code);
[[nodiscard]] std::string_view pad_button_name_from_code(u16 code);
[[nodiscard]] std::string_view pad_axis_name_from_code(u16 code);

/// The binding table as a `bindings.nfinput` file.
///
/// The inverse of parse(), and the reason a rebinding screen can exist: the
/// editor writes what the player chose back to the same file the runtime
/// reads. Every action is emitted with its tokens in order, signs included
/// (`-key:A`), so parse(serialize(t)) == t for every table this engine can
/// build. An action with no bindings is emitted as `action =` — dropping it
/// would silently resurrect the default the player just cleared.
[[nodiscard]] std::string serialize_bindings(const InputBindings& bindings);

/// Every action in the table, in the order the actions were added. For a
/// rebinding UI that has to list them; the pair is (action, bindings).
[[nodiscard]] std::vector<std::pair<std::string, std::vector<InputBinding>>>
binding_rows(const InputBindings& bindings);

} // namespace nf::runtime
