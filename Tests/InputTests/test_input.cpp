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
    // Magnitude 1.0: rescaled by (1 - 0.25)/1 = 0.75.
    NF_CHECK_NEAR(mapper.get_vector("Look").x, 0.75f, 1e-5f);
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
