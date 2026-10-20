#pragma once

// NF/Runtime/KeyboardInput.hpp — the default keyboard, mouse and gamepad input
// for a windowed runtime run.
//
// A game window that never installs an input source means a scene with a
// PlayerController renders beautifully and never moves — every gameplay module
// that polls sees a dead device. This source polls the real devices so the
// standalone player behaves like the editor's play mode.
//
// Since 0.4.0 it owns the POINTER. A third-person camera the player cannot
// turn is a camera they watch, and turning it needs three things a keyboard
// cannot give: a displacement per frame, a wheel, and a cursor that does not
// run out of screen. So the source reads the cursor itself instead of waiting
// for window messages — a clipped cursor still has a position, and a recentred
// one still reports the movement that led to the edge.
//
// Also since 0.4.0 the ACTION VOCABULARY is data (`InputBindings`), not a
// ladder of `if (action == ...)`. The defaults are exactly the ladder that
// used to be here, so nothing that worked before changed; a game that wants
// jump on the mouse installs its own table or ships a binding file.

#include <NF/Gameplay/GameplayModule.hpp>
#include <NF/Runtime/InputBindings.hpp>

namespace nf::runtime {

class KeyboardInputSource final : public nf::gameplay::IInputSource {
public:
    KeyboardInputSource() = default;
    ~KeyboardInputSource() override;

    KeyboardInputSource(const KeyboardInputSource&) = delete;
    KeyboardInputSource& operator=(const KeyboardInputSource&) = delete;

    [[nodiscard]] bool action_pressed(std::string_view action) const override;
    [[nodiscard]] float action_axis(std::string_view action) const override;

    /// The native window (HWND on Windows) whose client area is captured.
    /// Without it the source still reads keys and sticks, but there is no
    /// pointer: nothing to clip, nothing to centre.
    void set_window(void* native_window);

    /// Replaces the action table. The built-in defaults are used until this
    /// is called, so a caller that never rebinds sees the engine's own
    /// vocabulary.
    void set_bindings(InputBindings bindings);
    [[nodiscard]] const InputBindings& bindings() const { return m_bindings; }

    /// Refreshes the pointer and the pad once per frame. Called by the
    /// runtime before anything reads input, so every reader in a frame sees
    /// the same delta.
    void begin_frame() override;

    [[nodiscard]] bool look_delta(float& dx, float& dy) const override;
    [[nodiscard]] float wheel_delta() const override;
    void set_pointer_capture(bool captured) override;

    /// The deadzoned pad axis at `index`, in the platform's GamepadAxis
    /// order. Public because the device callbacks the binding evaluator calls
    /// are free functions rather than members.
    [[nodiscard]] float pad_axis_value(u16 index) const;

private:
    /// The live device, in the shape the binding evaluator reads.
    [[nodiscard]] DeviceState device_state() const;

    void* m_window = nullptr;

    InputBindings m_bindings = InputBindings::defaults();

    /// True while the cursor is hidden and clipped to the window.
    bool m_captured = false;
    /// Set when the player asked for the pointer back (Esc). While it is set,
    /// the runtime's per-frame "capture, please" is ignored, so a game cannot
    /// take the pointer away again the instant it is given back.
    bool m_capture_suppressed = false;
    /// The left button's state last frame, for the click that resumes play.
    bool m_left_was_down = false;
    /// The screen-space centre of the client area, where a captured cursor is
    /// held. Recomputed on capture, so a resized window still centres.
    int m_center_x = 0;
    int m_center_y = 0;
    /// The cursor's screen position last frame, for the uncaptured delta.
    int m_last_x = 0;
    int m_last_y = 0;
    bool m_have_last = false;

    /// This frame's readings. Written by begin_frame(), read (never consumed)
    /// by the getters, so two readers in one frame agree.
    float m_dx = 0.0f;
    float m_dy = 0.0f;
    float m_wheel = 0.0f;

    /// The pad's six axes after the deadzone, refreshed per frame. Indexed by
    /// the platform's GamepadAxis value, so the binding table's code IS the
    /// index — no translation layer to get wrong.
    float m_pad_axes[6] = {};
    bool m_pad_connected = false;
};

} // namespace nf::runtime
