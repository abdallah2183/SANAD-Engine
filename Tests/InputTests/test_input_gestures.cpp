#include <NF/Test/TestFramework.hpp>
#include <NF/Input/InputGesture.hpp>

using namespace nf;
using namespace nf::input;

NF_TEST(input_buffer_recording_and_sliding_window) {
    InputBuffer buffer(1.0f, 64); // 1.0s retention window

    BufferedInputEvent ev1{"Jump", KeyCode::Space, GamepadButton::A, MouseButton::Count, InputEventType::Pressed, 0.0f};
    buffer.record_event(ev1);

    NF_CHECK_EQ(buffer.event_count(), 1u);
    NF_CHECK(buffer.was_pressed("Jump", 0.5f));
    NF_CHECK(!buffer.was_released("Jump", 0.5f));
    NF_CHECK(!buffer.was_pressed("Fire", 0.5f));

    // Advance 0.5s and record release
    buffer.update(0.5f);
    BufferedInputEvent ev2{"Jump", KeyCode::Space, GamepadButton::A, MouseButton::Count, InputEventType::Released, 0.0f};
    buffer.record_event(ev2);

    NF_CHECK_EQ(buffer.event_count(), 2u);
    NF_CHECK(buffer.was_released("Jump", 0.1f));

    // Advance 1.2s: first press at t=0.0 should be evicted (current time is 1.7s, cutoff is 0.7s)
    buffer.update(1.2f);
    NF_CHECK(!buffer.was_pressed("Jump", 2.0f)); // ev1 evicted
    NF_CHECK(!buffer.was_released("Jump", 2.0f)); // ev2 also older than 1.0s window
    NF_CHECK_EQ(buffer.event_count(), 0u);
}

NF_TEST(input_chord_simultaneous_press) {
    InputGestureManager manager;

    int chord_triggers = 0;
    manager.register_chord("EX_Attack", {"LightPunch", "HeavyPunch"}, 0.06f, [&]() {
        chord_triggers++;
    });

    // 1. Success case: both pressed in quick succession (within 0.06s)
    manager.feed_action_event("LightPunch", InputEventType::Pressed);
    manager.update(0.02f);
    manager.feed_action_event("HeavyPunch", InputEventType::Pressed);

    NF_CHECK_EQ(chord_triggers, 1);
    NF_CHECK(manager.is_chord_triggered("EX_Attack"));

    // 2. Next frame
    manager.update(0.1f);
    NF_CHECK(!manager.is_chord_triggered("EX_Attack"));

    // 3. Failure case: too slow
    manager.feed_action_event("LightPunch", InputEventType::Pressed);
    manager.update(0.20f); // 200ms gap > 60ms window
    manager.feed_action_event("HeavyPunch", InputEventType::Pressed);

    NF_CHECK_EQ(chord_triggers, 1); // Not incremented
}

NF_TEST(fighting_game_motion_combo) {
    InputGestureManager manager;

    std::string triggered_combo;
    manager.register_combo("Hadouken", {"Down", "DownRight", "Right", "Punch"}, 0.3f, 1.0f,
                           [&](const std::string& name) {
                               triggered_combo = name;
                           });

    // Feed sequence with 0.05s intervals
    manager.feed_action_event("Down", InputEventType::Pressed);
    manager.update(0.05f);
    manager.feed_action_event("DownRight", InputEventType::Pressed);
    manager.update(0.05f);
    manager.feed_action_event("Right", InputEventType::Pressed);
    manager.update(0.05f);
    manager.feed_action_event("Punch", InputEventType::Pressed);

    NF_CHECK_EQ(triggered_combo, "Hadouken");
    NF_CHECK(manager.is_combo_triggered("Hadouken"));

    // Inputs were consumed, so another update doesn't re-trigger
    manager.update(0.05f);
    NF_CHECK(!manager.is_combo_triggered("Hadouken"));
}

NF_TEST(double_tap_dash_detection) {
    InputGestureManager manager;

    int dash_count = 0;
    manager.register_double_tap("Forward", 0.25f, [&]() {
        dash_count++;
    });

    // Tap 1: Press and release
    manager.feed_action_event("Forward", InputEventType::Pressed);
    manager.update(0.05f);
    manager.feed_action_event("Forward", InputEventType::Released);
    manager.update(0.10f); // 100ms gap

    // Tap 2: Press within 250ms window
    manager.feed_action_event("Forward", InputEventType::Pressed);
    NF_CHECK_EQ(dash_count, 1);
    NF_CHECK(manager.is_double_tap_triggered("Forward"));

    // Next frame
    manager.update(0.05f);
    NF_CHECK(!manager.is_double_tap_triggered("Forward"));

    // Slow tap: gap > 250ms
    manager.feed_action_event("Forward", InputEventType::Released);
    manager.update(0.35f); // 350ms gap > 250ms
    manager.feed_action_event("Forward", InputEventType::Pressed);
    NF_CHECK_EQ(dash_count, 1); // Did not increment
}

NF_TEST(charge_input_hold_and_release) {
    InputGestureManager manager;

    int charge_completed = 0;
    manager.register_charge("HoldBack", 0.8f, [&]() {
        charge_completed++;
    });

    // Press and hold
    manager.feed_action_event("HoldBack", InputEventType::Pressed);
    NF_CHECK(!manager.is_charged("HoldBack"));

    // Advance 0.5s: not yet charged
    manager.update(0.5f);
    NF_CHECK(!manager.is_charged("HoldBack"));
    NF_CHECK_NEAR(manager.get_held_duration("HoldBack"), 0.5f, 1e-4f);
    NF_CHECK_EQ(charge_completed, 0);

    // Advance another 0.4s (total 0.9s >= 0.8s): charged!
    manager.update(0.4f);
    NF_CHECK(manager.is_charged("HoldBack"));
    NF_CHECK_EQ(charge_completed, 1);

    // Releasing resets the charge
    manager.feed_action_event("HoldBack", InputEventType::Released);
    NF_CHECK(!manager.is_charged("HoldBack"));
    NF_CHECK_NEAR(manager.get_held_duration("HoldBack"), 0.0f, 1e-4f);
}
