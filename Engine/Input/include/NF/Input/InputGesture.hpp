#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Platform/InputSystem.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::input {

/// Type of input transition event
enum class InputEventType : u8 {
    Pressed = 0,
    Released,
    Repeated
};

/// A timestamped input event stored in the buffer
struct BufferedInputEvent {
    std::string action_name;
    KeyCode key = KeyCode::Unknown;
    GamepadButton pad_button = GamepadButton::Count;
    MouseButton mouse_button = MouseButton::Count;
    InputEventType type = InputEventType::Pressed;
    f32 timestamp = 0.0f; // engine time in seconds
};

/// Input Buffer maintaining a sliding window of recent input events
class InputBuffer {
public:
    explicit InputBuffer(f32 buffer_duration = 2.0f, size_t max_capacity = 128);

    void record_event(const BufferedInputEvent& event);
    void update(f32 dt);
    void clear();

    [[nodiscard]] f32 current_time() const noexcept { return m_current_time; }
    [[nodiscard]] size_t event_count() const noexcept { return m_events.size(); }
    [[nodiscard]] const std::deque<BufferedInputEvent>& events() const noexcept { return m_events; }

    /// Returns all events that occurred within the last `duration_seconds`
    [[nodiscard]] std::vector<BufferedInputEvent> recent_events(f32 duration_seconds) const;

    /// Checks if a specific action was pressed within `within_seconds`
    [[nodiscard]] bool was_pressed(const std::string& action_name, f32 within_seconds) const;

    /// Checks if a specific action was released within `within_seconds`
    [[nodiscard]] bool was_released(const std::string& action_name, f32 within_seconds) const;

private:
    f32 m_buffer_duration = 2.0f;
    size_t m_max_capacity = 128;
    f32 m_current_time = 0.0f;
    std::deque<BufferedInputEvent> m_events;
};

/// Definition and state for a chord (multiple simultaneous inputs)
struct InputChordDef {
    std::string name;
    std::vector<std::string> required_actions;
    f32 window_seconds = 0.05f; // Maximum time difference between presses
    std::function<void()> callback;
};

/// Definition and state for a motion combo or sequence
struct InputComboDef {
    std::string name;
    std::vector<std::string> sequence; // e.g. {"Down", "DownRight", "Right", "Punch"}
    f32 max_step_gap = 0.35f;          // Maximum allowed time between consecutive steps
    f32 max_total_duration = 1.2f;     // Maximum duration from first step to final step
    bool consume_inputs = true;        // Whether matching clears matched events
    std::function<void(const std::string& name)> callback;
};

/// Definition for a double-tap gesture (e.g. Forward, Forward for Dash)
struct DoubleTapDef {
    std::string action_name;
    f32 tap_window = 0.25f; // Maximum time between first release and second press
    std::function<void()> callback;
    f32 last_press_time = -100.0f;
    f32 last_release_time = -100.0f;
};

/// Definition for a charge gesture (e.g. Hold Back for 0.8s)
struct ChargeDef {
    std::string action_name;
    f32 required_charge_time = 0.8f;
    std::function<void()> callback;
    f32 press_time = -100.0f;
    bool is_held = false;
    bool has_triggered = false;
};

/// High-level Input Gesture and Combo Manager
class InputGestureManager {
public:
    InputGestureManager();
    ~InputGestureManager() = default;

    /// Register a simultaneous chord
    void register_chord(std::string name, std::vector<std::string> required_actions,
                        f32 window_seconds = 0.05f, std::function<void()> cb = nullptr);

    /// Register a sequence combo (fighting game motion or cheat code)
    void register_combo(std::string name, std::vector<std::string> sequence,
                        f32 max_step_gap = 0.35f, f32 max_total_duration = 1.2f,
                        std::function<void(const std::string& name)> cb = nullptr);

    /// Register a double-tap gesture
    void register_double_tap(const std::string& action_name, f32 tap_window = 0.25f,
                             std::function<void()> cb = nullptr);

    /// Register a charge gesture
    void register_charge(const std::string& action_name, f32 required_charge_time = 0.8f,
                         std::function<void()> cb = nullptr);

    /// Feed input event by action name
    void feed_action_event(const std::string& action_name, InputEventType type);

    /// Feed key event directly
    void feed_key_event(KeyCode key, InputEventType type, const std::string& action_alias = "");

    /// Feed gamepad button event directly
    void feed_gamepad_event(GamepadButton button, InputEventType type, const std::string& action_alias = "");

    /// Update tick: advances time, updates charges, evaluates gestures
    void update(f32 dt);

    /// Query if a combo was triggered during the current frame
    [[nodiscard]] bool is_combo_triggered(const std::string& combo_name) const;

    /// Query if a chord was triggered during the current frame
    [[nodiscard]] bool is_chord_triggered(const std::string& chord_name) const;

    /// Query if a double-tap was triggered during the current frame
    [[nodiscard]] bool is_double_tap_triggered(const std::string& action_name) const;

    /// Query if an action is currently charged
    [[nodiscard]] bool is_charged(const std::string& action_name) const;

    /// Query held duration for an action
    [[nodiscard]] f32 get_held_duration(const std::string& action_name) const;

    /// Access the underlying input buffer
    InputBuffer& buffer() noexcept { return m_buffer; }
    [[nodiscard]] const InputBuffer& buffer() const noexcept { return m_buffer; }

    /// Clear all registered gestures and reset buffer
    void clear();

private:
    void evaluate_chords();
    void evaluate_combos();

    InputBuffer m_buffer;
    std::vector<InputChordDef> m_chords;
    std::vector<InputComboDef> m_combos;
    std::unordered_map<std::string, DoubleTapDef> m_double_taps;
    std::unordered_map<std::string, ChargeDef> m_charges;

    std::vector<std::string> m_triggered_combos_this_frame;
    std::vector<std::string> m_triggered_chords_this_frame;
    std::vector<std::string> m_triggered_double_taps_this_frame;
};

} // namespace nf::input
