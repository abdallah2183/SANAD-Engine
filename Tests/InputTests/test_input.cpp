// InputTests — gameplay-facing action layer (design doc §50, §51, §114).
//
// The mapper is fed a synthetic InputState instead of a real window, so the
// tests are deterministic and need no device.

#include <NF/Input/InputAction.hpp>
#include <NF/Input/InputContext.hpp>
#include <NF/Input/InputMapper.hpp>
#include <NF/Input/InputNames.hpp>
#include <NF/Platform/InputSystem.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <string>

using namespace nf::input;
using nf::GamepadAxis;
using nf::GamepadButton;
using nf::InputState;
using nf::KeyCode;
using nf::MouseButton;
using nf::Vec2;
using nf::f32;
using nf::usize;

namespace {

/// Builds a mapper with a classic gameplay context: a 2D move action driven by
/// WASD, a bool jump on Space, and a 1D throttle on W/shift-less keys.
InputMapper make_gameplay_mapper() {
    InputMapper mapper;
    InputContext& gameplay = mapper.create_context("gameplay", 0);

    InputAction move("Move", ActionType::Axis2D);
    move.add_key(KeyCode::W, AxisComponent::Y, 1.0f);
    move.add_key(KeyCode::S, AxisComponent::Y, -1.0f);
    move.add_key(KeyCode::A, AxisComponent::X, -1.0f);
    move.add_key(KeyCode::D, AxisComponent::X, 1.0f);
    gameplay.add_action(std::move(move));

    InputAction jump("Jump", ActionType::Bool);
    jump.add_key(KeyCode::Space);
    jump.add_gamepad_button(GamepadButton::A);
    gameplay.add_action(std::move(jump));

    InputAction throttle("Throttle", ActionType::Axis1D);
    throttle.add_key(KeyCode::Up, AxisComponent::None, 1.0f);
    throttle.add_key(KeyCode::Down, AxisComponent::None, -1.0f);
    gameplay.add_action(std::move(throttle));

    return mapper;
}

void press(InputState& state, KeyCode key) {
    state.keys[static_cast<usize>(key)] = true;
}

void release(InputState& state, KeyCode key) {
    state.keys[static_cast<usize>(key)] = false;
}

} // namespace

// --- keyboard composes a 2D direction (§325: GetVector("Move")) -------------

NF_TEST(move_idle_when_nothing_held) {
    InputMapper mapper = make_gameplay_mapper();
    InputState state;
    mapper.update(state);
    const Vec2 v = mapper.get_vector("Move");
    NF_CHECK(v.x == 0.0f);
    NF_CHECK(v.y == 0.0f);
    NF_CHECK(!mapper.is_pressed("Jump"));
}

NF_TEST(move_reads_wasD_as_full_direction) {
    InputMapper mapper = make_gameplay_mapper();
    InputState state;
    press(state, KeyCode::D);
    press(state, KeyCode::W);
    mapper.update(state);

    const Vec2 v = mapper.get_vector("Move");
    NF_CHECK_NEAR(v.x, 1.0f, 1e-6f);
    NF_CHECK_NEAR(v.y, 1.0f, 1e-6f);
}

NF_TEST(opposing_keys_cancel_to_zero) {
    InputMapper mapper = make_gameplay_mapper();
    InputState state;
    press(state, KeyCode::A);
    press(state, KeyCode::D);
    mapper.update(state);
    const Vec2 v = mapper.get_vector("Move");
    NF_CHECK_NEAR(v.x, 0.0f, 1e-6f);
}

NF_TEST(axis1d_clamps_to_unit_range) {
    InputMapper mapper = make_gameplay_mapper();
    InputState state;
    press(state, KeyCode::Up);
    mapper.update(state);
    NF_CHECK_NEAR(mapper.get_axis("Throttle"), 1.0f, 1e-6f);
    release(state, KeyCode::Up);
    press(state, KeyCode::Down);
    mapper.update(state);
    NF_CHECK_NEAR(mapper.get_axis("Throttle"), -1.0f, 1e-6f);
}

// --- bool actions and edge detection --------------------------------------

NF_TEST(bool_action_and_just_pressed_edge) {
    InputMapper mapper = make_gameplay_mapper();
    InputState state;

    mapper.update(state);
    NF_CHECK(!mapper.is_pressed("Jump"));
    NF_CHECK(!mapper.just_pressed("Jump"));
    mapper.end_frame();

    press(state, KeyCode::Space);
    mapper.update(state);
    NF_CHECK(mapper.is_pressed("Jump"));
    NF_CHECK(mapper.just_pressed("Jump"));  // rose this frame
    mapper.end_frame();

    // Hold: still pressed, but no fresh edge.
    mapper.update(state);
    NF_CHECK(mapper.is_pressed("Jump"));
    NF_CHECK(!mapper.just_pressed("Jump"));
    mapper.end_frame();

    release(state, KeyCode::Space);
    mapper.update(state);
    NF_CHECK(!mapper.is_pressed("Jump"));
    NF_CHECK(mapper.just_released("Jump"));
}

NF_TEST(gamepad_button_drives_bool_action) {
    InputMapper mapper = make_gameplay_mapper();
    InputState state;
    state.gamepad_buttons[static_cast<usize>(GamepadButton::A)] = true;
    mapper.update(state);
    NF_CHECK(mapper.is_pressed("Jump"));
    NF_CHECK(mapper.just_pressed("Jump"));
}

// --- gamepad axes: deadzone and rescale (§51) ------------------------------

NF_TEST(stick_axis_below_deadzone_is_zero) {
    InputMapper mapper;
    InputContext& ctx = mapper.create_context("gameplay", 0);
    InputAction look("Look", ActionType::Axis2D);
    look.add_gamepad_axis(GamepadAxis::LeftX, AxisComponent::X);
    look.add_gamepad_axis(GamepadAxis::LeftY, AxisComponent::Y);
    ctx.add_action(std::move(look));

    GamepadSettings pad;
    pad.stick_deadzone = 0.2f;
    mapper.set_gamepad_settings(pad);

    InputState state;
    state.gamepad_axes[static_cast<usize>(GamepadAxis::LeftX)] = 0.10f;
    state.gamepad_axes[static_cast<usize>(GamepadAxis::LeftY)] = 0.10f;
    mapper.update(state);
    NF_CHECK(mapper.get_vector("Look").x == 0.0f); // magnitude 0.141 < 0.2
}

NF_TEST(stick_axis_above_deadzone_is_rescaled) {
    InputMapper mapper;
    InputContext& ctx = mapper.create_context("gameplay", 0);
    InputAction look("Look", ActionType::Axis2D);
    look.add_gamepad_axis(GamepadAxis::LeftX, AxisComponent::X);
    ctx.add_action(std::move(look));
    ctx.add_action(InputAction{"unused", ActionType::Bool}); // keeps structure
    mapper.create_context("empty", 1);

    GamepadSettings pad;
    pad.stick_deadzone = 0.25f;
    mapper.set_gamepad_settings(pad);

    InputState state;
    state.gamepad_axes[static_cast<usize>(GamepadAxis::LeftX)] = 1.0f;
    state.gamepad_axes[static_cast<usize>(GamepadAxis::LeftY)] = 0.0f;
    mapper.update(state);
    // Magnitude 1.0 is the top of the surviving [deadzone, 1] band, so the
    // rescale must land it exactly on 1.0 — a full-lock stick commands the
    // full action value. (Dividing by `mag` instead of `mag * (1 - deadzone)`
    // would cap this at 0.75 and make full steer unreachable.)
    NF_CHECK_NEAR(mapper.get_vector("Look").x, 1.0f, 1e-5f);

    // Half deflection is rescaled linearly: (0.5 - 0.25) / (1 - 0.25) = 1/3.
    state.gamepad_axes[static_cast<usize>(GamepadAxis::LeftX)] = 0.5f;
    mapper.update(state);
    NF_CHECK_NEAR(mapper.get_vector("Look").x, 1.0f / 3.0f, 1e-5f);
}

NF_TEST(trigger_deadzone_rescales_full_pull) {
    GamepadSettings pad;
    pad.trigger_deadzone = 0.5f;
    // Half pull on a 0.5 deadzone sits exactly at 1.0 after rescale.
    NF_CHECK_NEAR(InputAction::process_axis(GamepadAxis::LeftTrigger, 0.5f, 0.0f, pad),
                  0.0f, 1e-6f);
    NF_CHECK_NEAR(InputAction::process_axis(GamepadAxis::LeftTrigger, 1.0f, 0.0f, pad),
                  1.0f, 1e-6f);
    NF_CHECK_NEAR(InputAction::process_axis(GamepadAxis::LeftTrigger, 0.75f, 0.0f, pad),
                  0.5f, 1e-5f);
}

NF_TEST(keyboard_and_gamepad_compose_into_one_action) {
    InputMapper mapper;
    InputContext& ctx = mapper.create_context("gameplay", 0);
    InputAction move("Move", ActionType::Axis2D);
    move.add_key(KeyCode::D, AxisComponent::X, 1.0f);
    move.add_gamepad_axis(GamepadAxis::LeftX, AxisComponent::X);
    ctx.add_action(std::move(move));

    GamepadSettings pad; // default deadzone
    pad.stick_deadzone = 0.0f;
    mapper.set_gamepad_settings(pad);

    InputState state;
    state.gamepad_axes[static_cast<usize>(GamepadAxis::LeftX)] = 0.4f;
    press(state, KeyCode::D);
    mapper.update(state);
    // Clamped: 0.4 (stick) + 1.0 (key) = 1.4 -> clamped to 1.0.
    NF_CHECK_NEAR(mapper.get_vector("Move").x, 1.0f, 1e-6f);
}

// --- contexts and priority blocking (§50) ---------------------------------

NF_TEST(blocking_menu_context_suppresses_gameplay) {
    InputMapper mapper;
    InputContext& gameplay = mapper.create_context("gameplay", 0);
    InputAction jump("Jump", ActionType::Bool);
    jump.add_key(KeyCode::Space);
    gameplay.add_action(std::move(jump));

    InputContext& menu = mapper.create_context("menu", 10, /*blocking=*/true);
    InputAction confirm("Confirm", ActionType::Bool);
    confirm.add_key(KeyCode::Enter);
    menu.add_action(std::move(confirm));

    InputState state;
    press(state, KeyCode::Space); // gameplay input held...

    // Menu open: gameplay is suppressed, menu still works.
    mapper.update(state);
    NF_CHECK(!mapper.is_pressed("Jump"));
    NF_CHECK(!mapper.just_pressed("Jump"));
    press(state, KeyCode::Enter);
    mapper.update(state);
    NF_CHECK(mapper.is_pressed("Confirm"));

    // Menu closed: the held Space now counts again.
    mapper.set_context_active("menu", false);
    mapper.update(state);
    NF_CHECK(mapper.is_pressed("Jump"));
    NF_CHECK(mapper.just_pressed("Jump")); // suppressed press is not eaten
}

NF_TEST(inactive_context_actions_read_zero) {
    InputMapper mapper;
    InputContext& gameplay = mapper.create_context("gameplay", 0);
    InputAction jump("Jump", ActionType::Bool);
    jump.add_key(KeyCode::Space);
    gameplay.add_action(std::move(jump));

    InputState state;
    press(state, KeyCode::Space);
    mapper.set_context_active("gameplay", false);
    mapper.update(state);
    NF_CHECK(!mapper.is_pressed("Jump"));
    NF_CHECK(mapper.get_axis("Jump") == 0.0f);
}

NF_TEST(unknown_action_queries_are_safe) {
    InputMapper mapper = make_gameplay_mapper();
    InputState state;
    mapper.update(state);
    NF_CHECK(!mapper.is_pressed("DoesNotExist"));
    NF_CHECK(!mapper.just_pressed("DoesNotExist"));
    NF_CHECK(!mapper.just_released("DoesNotExist"));
    NF_CHECK(mapper.get_axis("DoesNotExist") == 0.0f);
    NF_CHECK(mapper.get_vector("DoesNotExist").x == 0.0f);
}

// --- determinism (§114): identical state, identical values -----------------

NF_TEST(repeat_updates_are_identical) {
    InputMapper a = make_gameplay_mapper();
    InputMapper b = make_gameplay_mapper();
    InputState state;
    press(state, KeyCode::W);
    press(state, KeyCode::D);

    for (int i = 0; i < 5; ++i) {
        a.update(state);
        b.update(state);
        a.end_frame();
        b.end_frame();
    }
    NF_CHECK_NEAR(a.get_vector("Move").x, b.get_vector("Move").x, 0.0f);
    NF_CHECK_NEAR(a.get_vector("Move").y, b.get_vector("Move").y, 0.0f);
    NF_CHECK(a.is_pressed("Jump") == b.is_pressed("Jump"));
}

// --- serialization round-trip (rebinding persistence, §138) ----------------

NF_TEST(bindings_round_trip_through_text) {
    InputMapper original = make_gameplay_mapper();
    const std::string text = original.to_string();

    InputMapper restored;
    NF_CHECK(restored.load_string(text));
    NF_CHECK(restored.context_count() == original.context_count());

    // Same bindings must react identically to the same device state.
    InputState state;
    press(state, KeyCode::Space);
    press(state, KeyCode::W);
    original.update(state);
    restored.update(state);

    NF_CHECK(original.is_pressed("Jump") == restored.is_pressed("Jump"));
    NF_CHECK_NEAR(original.get_vector("Move").y, restored.get_vector("Move").y, 1e-6f);
    NF_CHECK(restored.find_context("gameplay") != nullptr);
    NF_CHECK(restored.find_context("gameplay")->action_count() == 3);
}

NF_TEST(load_string_rejects_wrong_version) {
    InputMapper mapper;
    NF_CHECK(!mapper.load_string("v 999\n"));
    NF_CHECK(mapper.context_count() == 0);
}

NF_TEST(load_string_skips_garbage_and_keeps_valid_lines) {
    InputMapper mapper;
    const std::string text =
        "v 1\n"
        "# a comment line\n"
        "c gameplay 0 0\n"
        "garbage token line that is malformed\n"
        "a gameplay Jump Bool\n"
        "b gameplay Jump Key Space None 1.000000\n"
        "b gameplay Jump Key BogusKey None 1.000000\n"; // bad enum: skipped
    NF_CHECK(mapper.load_string(text) == false);       // malformed lines present
    // The valid action and its good binding survived.
    const InputContext* ctx = mapper.find_context("gameplay");
    NF_CHECK(ctx != nullptr);
    const InputAction* jump = ctx->find_action("Jump");
    NF_CHECK(jump != nullptr);
    NF_CHECK(jump->bindings().size() == 1);
}

// --- enum name tables (editor rebinding UI) --------------------------------

NF_TEST(enum_names_round_trip) {
    KeyCode k = KeyCode::Unknown;
    NF_CHECK(parse_key("Space", k));
    NF_CHECK(k == KeyCode::Space);
    NF_CHECK(to_string(KeyCode::Space) == "Space");

    GamepadAxis a = GamepadAxis::Count;
    NF_CHECK(parse_gamepad_axis("RightTrigger", a));
    NF_CHECK(a == GamepadAxis::RightTrigger);
    NF_CHECK(to_string(GamepadAxis::RightTrigger) == "RightTrigger");

    ActionType t = ActionType::Bool;
    NF_CHECK(parse_action_type("Axis2D", t));
    NF_CHECK(t == ActionType::Axis2D);

    AxisComponent c = AxisComponent::None;
    NF_CHECK(parse_axis_component("Y", c));
    NF_CHECK(c == AxisComponent::Y);

    NF_CHECK(!parse_key("NotAKey", k));
    NF_CHECK(!parse_gamepad_axis("LeftX2", a));
}

NF_TEST(context_stable_references_across_growth) {
    InputMapper mapper;
    InputContext& first = mapper.create_context("first", 0);
    first.add_action(InputAction{"A", ActionType::Bool});
    for (int i = 0; i < 8; ++i) {
        mapper.create_context("ctx" + std::to_string(i), i);
    }
    // The reference handed out earlier must still resolve after growth.
    NF_CHECK(first.find_action("A") != nullptr);
    NF_CHECK(first.name() == "first");

    InputState state;
    mapper.update(state);
    NF_CHECK(first.find_action("A") != nullptr); // still valid post-refresh
}

// --- live sample path (XInput mask → InputSystem edges → action map) --------
// G1: headless proof that a real poll-shaped GamepadSample drives the same
// actions VehicleDemo reads, with no device and no GPU.

namespace {

/// Clears any prior singleton gamepad edges so tests do not leak into each other.
void reset_pad_edges(nf::InputSystem& sys) {
    sys.begin_frame();
    sys.end_frame();
    sys.begin_frame();
}

} // namespace

NF_TEST(xinput_mask_maps_to_engine_buttons_and_edges) {
    auto& sys = nf::InputSystem::instance();
    reset_pad_edges(sys);
    NF_CHECK(!sys.gamepad_connected());

    nf::GamepadSample press{};
    press.connected = true;
    press.buttons = nf::xinput_buttons::A | nf::xinput_buttons::DPadRight |
                    nf::xinput_buttons::RightShoulder;
    sys.apply_gamepad_sample(press);

    NF_CHECK(sys.gamepad_connected());
    NF_CHECK(sys.is_gamepad_down(nf::GamepadButton::A));
    NF_CHECK(sys.is_gamepad_pressed(nf::GamepadButton::A));
    NF_CHECK(sys.is_gamepad_down(nf::GamepadButton::DPadRight));
    NF_CHECK(sys.is_gamepad_pressed(nf::GamepadButton::RightBumper));
    NF_CHECK(!sys.is_gamepad_down(nf::GamepadButton::B));
    NF_CHECK(!sys.is_gamepad_pressed(nf::GamepadButton::B));

    // Hold: still down, edge gone after begin_frame.
    sys.end_frame();
    sys.begin_frame();
    sys.apply_gamepad_sample(press);
    NF_CHECK(sys.is_gamepad_down(nf::GamepadButton::A));
    NF_CHECK(!sys.is_gamepad_pressed(nf::GamepadButton::A));

    // Release: edge fires once.
    sys.end_frame();
    sys.begin_frame();
    nf::GamepadSample release{};
    release.connected = true;
    release.buttons = 0;
    sys.apply_gamepad_sample(release);
    NF_CHECK(!sys.is_gamepad_down(nf::GamepadButton::A));
    NF_CHECK(sys.is_gamepad_released(nf::GamepadButton::A));

    // Disconnect clears pad state and flags.
    sys.end_frame();
    sys.begin_frame();
    sys.apply_gamepad_sample(nf::GamepadSample{});
    NF_CHECK(!sys.gamepad_connected());
    NF_CHECK(!sys.is_gamepad_down(nf::GamepadButton::A));
}

NF_TEST(sample_sticks_and_triggers_normalize_to_unit_range) {
    auto& sys = nf::InputSystem::instance();
    reset_pad_edges(sys);

    nf::GamepadSample s{};
    s.connected = true;
    s.left_x = 32767;
    s.left_y = -32768;
    s.right_x = 16384;
    s.left_trigger = 255;
    s.right_trigger = 0;
    sys.apply_gamepad_sample(s);

    NF_CHECK_NEAR(sys.gamepad_axis(nf::GamepadAxis::LeftX), 1.0f, 1e-5f);
    NF_CHECK_NEAR(sys.gamepad_axis(nf::GamepadAxis::LeftY), -1.0f, 1e-5f);
    NF_CHECK_NEAR(sys.gamepad_axis(nf::GamepadAxis::RightX), 16384.0f / 32767.0f, 1e-5f);
    NF_CHECK_NEAR(sys.gamepad_axis(nf::GamepadAxis::LeftTrigger), 1.0f, 1e-5f);
    NF_CHECK_NEAR(sys.gamepad_axis(nf::GamepadAxis::RightTrigger), 0.0f, 1e-6f);

    // Clean singleton for any later test.
    sys.end_frame();
    sys.begin_frame();
    sys.apply_gamepad_sample(nf::GamepadSample{});
    sys.end_frame();
}

NF_TEST(sample_drives_vehicle_action_map_end_to_end) {
    // Same shape as VehicleDemo: sample → InputSystem → mapper actions.
    auto& sys = nf::InputSystem::instance();
    reset_pad_edges(sys);

    InputMapper mapper;
    InputContext& drive = mapper.create_context("driving", 0);

    InputAction throttle("Throttle", ActionType::Axis1D);
    throttle.add_key(KeyCode::W);
    throttle.add_gamepad_axis(GamepadAxis::RightTrigger, AxisComponent::None, 1.0f);
    drive.add_action(std::move(throttle));

    InputAction steer("Steer", ActionType::Axis1D);
    steer.add_key(KeyCode::A, AxisComponent::None, -1.0f);
    steer.add_key(KeyCode::D, AxisComponent::None, 1.0f);
    steer.add_gamepad_axis(GamepadAxis::LeftX, AxisComponent::None, 1.0f);
    drive.add_action(std::move(steer));

    InputAction handbrake("Handbrake", ActionType::Bool);
    handbrake.add_key(KeyCode::Space);
    handbrake.add_gamepad_button(GamepadButton::A);
    drive.add_action(std::move(handbrake));

    InputAction reset("Reset", ActionType::Bool);
    reset.add_key(KeyCode::R);
    reset.add_gamepad_button(GamepadButton::Y);
    drive.add_action(std::move(reset));

    GamepadSettings pad; // defaults; stick deadzone 0.15
    mapper.set_gamepad_settings(pad);

    nf::GamepadSample s{};
    s.connected = true;
    s.buttons = nf::xinput_buttons::A | nf::xinput_buttons::Y;
    s.left_x = 32767;                 // full right steer
    s.right_trigger = 255;            // full throttle
    sys.apply_gamepad_sample(s);

    mapper.update(sys.state());
    NF_CHECK_NEAR(mapper.get_axis("Throttle"), 1.0f, 1e-5f);
    NF_CHECK_NEAR(mapper.get_axis("Steer"), 1.0f, 1e-5f);
    NF_CHECK(mapper.is_pressed("Handbrake"));
    NF_CHECK(mapper.just_pressed("Handbrake"));
    NF_CHECK(mapper.is_pressed("Reset"));
    NF_CHECK(mapper.just_pressed("Reset"));
    mapper.end_frame();

    // Hold: edges drop, values stay.
    sys.end_frame();
    sys.begin_frame();
    sys.apply_gamepad_sample(s);
    mapper.update(sys.state());
    NF_CHECK_NEAR(mapper.get_axis("Throttle"), 1.0f, 1e-5f);
    NF_CHECK(!mapper.just_pressed("Handbrake"));
    NF_CHECK(mapper.is_pressed("Handbrake"));
    mapper.end_frame();

    // Disconnect: handbrake releases, throttle/steer zero.
    sys.end_frame();
    sys.begin_frame();
    sys.apply_gamepad_sample(nf::GamepadSample{});
    mapper.update(sys.state());
    NF_CHECK(!mapper.is_pressed("Handbrake"));
    NF_CHECK(mapper.just_released("Handbrake"));
    NF_CHECK_NEAR(mapper.get_axis("Throttle"), 0.0f, 1e-6f);
    NF_CHECK_NEAR(mapper.get_axis("Steer"), 0.0f, 1e-6f);
    mapper.end_frame();

    // Leave singleton clean.
    sys.end_frame();
}

// --- action callbacks (held/pressed/released, G1) ---------------------------
// Same headless shape as the query tests: a fake pad sample drives the mapper,
// the callbacks record what a gameplay subscriber would have seen.

NF_TEST(pad_button_fires_pressed_held_released_callbacks) {
    InputMapper mapper;
    InputContext& drive = mapper.create_context("driving", 0);
    InputAction handbrake("Handbrake", ActionType::Bool);
    handbrake.add_gamepad_button(GamepadButton::A);
    drive.add_action(std::move(handbrake));

    int pressed = 0, held = 0, released = 0;
    mapper.set_action_callback("Handbrake", ActionPhase::Pressed,
                               [&](const ActionValue&) { ++pressed; });
    mapper.set_action_callback("Handbrake", ActionPhase::Held,
                               [&](const ActionValue&) { ++held; });
    mapper.set_action_callback("Handbrake", ActionPhase::Released,
                               [&](const ActionValue&) { ++released; });

    InputState state;
    mapper.update(state); // nothing held: no callback at all
    NF_CHECK(pressed == 0 && held == 0 && released == 0);
    mapper.end_frame();

    state.gamepad_buttons[static_cast<usize>(GamepadButton::A)] = true;
    mapper.update(state); // press edge
    NF_CHECK(pressed == 1);
    NF_CHECK(held == 0); // Pressed frame is not also Held
    NF_CHECK(released == 0);
    mapper.end_frame();

    mapper.update(state); // hold
    NF_CHECK(pressed == 1);
    NF_CHECK(held == 1);
    mapper.end_frame();

    mapper.update(state); // still held
    NF_CHECK(held == 2);
    mapper.end_frame();

    state.gamepad_buttons[static_cast<usize>(GamepadButton::A)] = false;
    mapper.update(state); // release edge
    NF_CHECK(released == 1);
    NF_CHECK(held == 2); // no Held on the Released frame
    NF_CHECK(pressed == 1);
    mapper.end_frame();

    mapper.update(state); // idle again: no repeat release
    NF_CHECK(released == 1);
}

NF_TEST(pad_axis_crossing_threshold_fires_edge_callbacks) {
    InputMapper mapper;
    InputContext& drive = mapper.create_context("driving", 0);
    InputAction throttle("Throttle", ActionType::Axis1D);
    throttle.add_gamepad_axis(GamepadAxis::RightTrigger, AxisComponent::None, 1.0f);
    drive.add_action(std::move(throttle));

    // Deadzone off: this test is about edge timing and payload, and a zero
    // deadzone makes the callback value equal the raw trigger reading exactly.
    // (With the default 0.05 deadzone the payload is the rescaled value, so a
    // 0.9 pull reports 0.8947 — the deadzone math is pinned separately.)
    GamepadSettings pad;
    pad.trigger_deadzone = 0.0f;
    mapper.set_gamepad_settings(pad);

    int pressed = 0, held = 0, released = 0;
    f32 last_value = -1.0f;
    mapper.set_action_callback("Throttle", ActionPhase::Pressed,
                               [&](const ActionValue& v) { ++pressed; last_value = v.x; });
    mapper.set_action_callback("Throttle", ActionPhase::Held,
                               [&](const ActionValue&) { ++held; });
    mapper.set_action_callback("Throttle", ActionPhase::Released,
                               [&](const ActionValue&) { ++released; });

    InputState state;
    mapper.update(state); // trigger at 0: nothing
    NF_CHECK(pressed == 0 && held == 0 && released == 0);
    mapper.end_frame();

    // Half pull = exactly the threshold; pressed() is strictly greater, so no edge yet.
    state.gamepad_axes[static_cast<usize>(GamepadAxis::RightTrigger)] = 0.5f;
    mapper.update(state);
    NF_CHECK(pressed == 0);
    mapper.end_frame();

    state.gamepad_axes[static_cast<usize>(GamepadAxis::RightTrigger)] = 0.9f;
    mapper.update(state); // crosses 0.5: Pressed with the live value
    NF_CHECK(pressed == 1);
    NF_CHECK_NEAR(last_value, 0.9f, 1e-5f);
    mapper.end_frame();

    mapper.update(state); // sustained pull: Held
    NF_CHECK(held == 1);
    mapper.end_frame();

    state.gamepad_axes[static_cast<usize>(GamepadAxis::RightTrigger)] = 0.0f;
    mapper.update(state); // trigger released
    NF_CHECK(released == 1);
}

NF_TEST(blocking_context_releases_callbacks_of_suppressed_actions) {
    InputMapper mapper;
    InputContext& gameplay = mapper.create_context("gameplay", 0);
    InputAction jump("Jump", ActionType::Bool);
    jump.add_key(KeyCode::Space);
    jump.add_gamepad_button(GamepadButton::A);
    gameplay.add_action(std::move(jump));

    InputContext& menu = mapper.create_context("menu", 10, /*blocking=*/true);
    InputAction confirm("Confirm", ActionType::Bool);
    confirm.add_key(KeyCode::Enter);
    menu.add_action(std::move(confirm));

    int jump_pressed = 0, jump_released = 0;
    int confirm_pressed = 0;
    mapper.set_action_callback("Jump", ActionPhase::Pressed,
                               [&](const ActionValue&) { ++jump_pressed; });
    mapper.set_action_callback("Jump", ActionPhase::Released,
                               [&](const ActionValue&) { ++jump_released; });
    mapper.set_action_callback("Confirm", ActionPhase::Pressed,
                               [&](const ActionValue&) { ++confirm_pressed; });

    // Contexts default to active, and an active blocking context suppresses
    // everything below it from the first frame — so the menu must be closed
    // before the "menu opens" step below can mean anything.
    mapper.set_context_active("menu", false);

    InputState state;
    state.gamepad_buttons[static_cast<usize>(GamepadButton::A)] = true;
    mapper.update(state);
    NF_CHECK(jump_pressed == 1);
    mapper.end_frame();

    // Menu opens while A is held: the suppressed action reads as Released —
    // the same value just_released reports, so callbacks cannot disagree
    // with queries.
    mapper.set_context_active("menu", true);
    mapper.update(state);
    NF_CHECK(jump_released == 1);
    NF_CHECK(!mapper.just_pressed("Jump"));
    mapper.end_frame();

    press(state, KeyCode::Enter);
    mapper.update(state); // menu still works while gameplay is blocked
    NF_CHECK(confirm_pressed == 1);
}

NF_TEST(callback_for_unknown_action_never_fires) {
    InputMapper mapper;
    InputContext& ctx = mapper.create_context("gameplay", 0);
    ctx.add_action(InputAction{"Real", ActionType::Bool});

    bool fired = false;
    mapper.set_action_callback("Real", ActionPhase::Pressed,
                               [&](const ActionValue&) { fired = true; });
    mapper.set_action_callback("Ghost", ActionPhase::Pressed,
                               [&](const ActionValue&) { fired = true; });

    InputState state;
    press(state, KeyCode::Space); // nothing bound to either name
    mapper.update(state);
    NF_CHECK(!fired); // no crash, no spurious fire
}

// --- live hardware path (env-gated, NF_SKIP per protocol) -------------------
// Proves the real XInput poll — not just injected samples — drives the state.
// Without a pad attached this must count as SKIPPED, never as a pass.

NF_TEST(live_xinput_poll_reports_a_connected_pad) {
    auto& sys = nf::InputSystem::instance();
    reset_pad_edges(sys);

    sys.poll_gamepad(); // real XInput slots 0–3 (no-op if the DLL is absent)
    if (!sys.gamepad_connected()) {
        NF_SKIP("no XInput pad connected — live hardware check requires a pad");
    }

    // A pad that XInput reports as connected must present sane analog data,
    // whatever the sticks are physically doing this instant.
    for (usize i = 0; i < static_cast<usize>(GamepadAxis::LeftTrigger); ++i) {
        const f32 v = sys.gamepad_axis(static_cast<GamepadAxis>(i));
        NF_CHECK(v >= -1.001f && v <= 1.001f);
    }
    NF_CHECK(sys.gamepad_axis(GamepadAxis::LeftTrigger) >= 0.0f);
    NF_CHECK(sys.gamepad_axis(GamepadAxis::LeftTrigger) <= 1.001f);
    NF_CHECK(sys.gamepad_axis(GamepadAxis::RightTrigger) >= 0.0f);
    NF_CHECK(sys.gamepad_axis(GamepadAxis::RightTrigger) <= 1.001f);

    // A second poll must be stable (connected stays connected).
    sys.end_frame();
    sys.begin_frame();
    sys.poll_gamepad();
    NF_CHECK(sys.gamepad_connected());

    // Leave the singleton clean for any later suite in the same process.
    sys.apply_gamepad_sample(nf::GamepadSample{});
    sys.end_frame();
}
