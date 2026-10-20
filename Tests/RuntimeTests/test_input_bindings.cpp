// Tests/RuntimeTests/test_input_bindings.cpp — which control means which action.
//
// Before 0.4.0 the answer was a ladder of `if (action == "jump") return
// key_down(VK_SPACE)` inside the input source, which meant a game could not
// move jump to the mouse without replacing the source. The vocabulary is now a
// table, and these cases pin the two things that can go wrong with a table:
//
//   1. the DEFAULTS must reproduce the ladder exactly, or every existing game
//      changes behaviour the day this shipped;
//   2. the EVALUATOR must be a pure function of (table, action, device), which
//      is what lets a machine with no keyboard and no pad prove it.
//
// The fake device below is the whole point: `binding_axis("move_x")` is
// evaluated against a table and a bag of pressed keys, with no Win32 call and
// no XInput slot anywhere in the test.

#include <NF/Test/TestFramework.hpp>
#include <NF/Runtime/InputBindings.hpp>

#include <cmath>
#include <set>
#include <string>

using namespace nf;
using namespace nf::runtime;

namespace {

constexpr float kEps = 1e-4f;

/// A device made of whatever the test says is held.
struct FakeDevice {
    std::set<u16> keys;
    std::set<u16> buttons;
    float axes[6] = {};

    void press(u16 vk) { keys.insert(vk); }
    void release(u16 vk) { keys.erase(vk); }
    void hold_pad(u16 button) { buttons.insert(button); }
    void set_axis(u16 axis, float value) {
        if (axis < 6) {
            axes[axis] = value;
        }
    }

    [[nodiscard]] DeviceState state() const {
        DeviceState s;
        s.context = this;
        s.key_down = [](const void* c, u16 code) {
            return static_cast<const FakeDevice*>(c)->keys.count(code) > 0;
        };
        s.pad_down = [](const void* c, u16 code) {
            return static_cast<const FakeDevice*>(c)->buttons.count(code) > 0;
        };
        s.pad_axis = [](const void* c, u16 code) {
            return code < 6 ? static_cast<const FakeDevice*>(c)->axes[code] : 0.0f;
        };
        return s;
    }
};

} // namespace

// --- the defaults are the old ladder ----------------------------------------

NF_TEST(default_bindings_are_the_ladder_that_used_to_be_hardcoded) {
    const InputBindings b = InputBindings::defaults();

    // Every action the engine ever answered, including the aliases.
    for (const char* action : {"move_forward", "forward", "up", "move_back", "back", "down",
                               "move_left", "left", "move_right", "right", "jump", "sprint",
                               "interact", "look", "zoom", "move_x", "move_z", "look_x",
                               "look_y"}) {
        NF_CHECK(b.find(action) != nullptr);
    }

    // Jump: Space and pad A.
    FakeDevice d;
    d.press(0x20);
    NF_CHECK(binding_pressed(b, "jump", d.state()));
    d.release(0x20);
    NF_CHECK(!binding_pressed(b, "jump", d.state()));
    d.hold_pad(0); // A
    NF_CHECK(binding_pressed(b, "jump", d.state()));

    // Sprint: Shift, LB, or the left trigger past half.
    FakeDevice s;
    s.press(0x10);
    NF_CHECK(binding_pressed(b, "sprint", s.state()));
    s.release(0x10);
    s.hold_pad(4);
    NF_CHECK(binding_pressed(b, "sprint", s.state()));
    s.buttons.clear();
    s.set_axis(4, 0.8f);
    NF_CHECK(binding_pressed(b, "sprint", s.state()));

    // Interact: E and pad X.
    FakeDevice e;
    e.press('E');
    NF_CHECK(binding_pressed(b, "interact", e.state()));
}

NF_TEST(default_axes_keep_their_signs_and_their_clamp) {
    const InputBindings b = InputBindings::defaults();

    // D is +x, A is -x.
    FakeDevice right;
    right.press('D');
    NF_CHECK_NEAR(binding_axis(b, "move_x", right.state()), 1.0f, kEps);
    FakeDevice left;
    left.press('A');
    NF_CHECK_NEAR(binding_axis(b, "move_x", left.state()), -1.0f, kEps);

    // W is FORWARD, and forward is -z: the engine's move_z is +1 for back.
    FakeDevice forward;
    forward.press('W');
    NF_CHECK_NEAR(binding_axis(b, "move_z", forward.state()), -1.0f, kEps);
    FakeDevice back;
    back.press('S');
    NF_CHECK_NEAR(binding_axis(b, "move_z", back.state()), 1.0f, kEps);

    // Both at once cancel, rather than one winning by table order.
    FakeDevice both;
    both.press('A');
    both.press('D');
    NF_CHECK_NEAR(binding_axis(b, "move_x", both.state()), 0.0f, kEps);

    // XInput reports +Y as UP, and the engine wants +z for BACK, so the left
    // stick's Y contribution is negated. Pushing the stick forward (Y = +1)
    // must therefore read as -1.
    FakeDevice stick;
    stick.set_axis(1, 1.0f);
    NF_CHECK_NEAR(binding_axis(b, "move_z", stick.state()), -1.0f, kEps);

    // A key and a stick together do not exceed the [-1, 1] contract.
    FakeDevice mixed;
    mixed.press('D');
    mixed.set_axis(0, 1.0f);
    NF_CHECK_NEAR(binding_axis(b, "move_x", mixed.state()), 1.0f, kEps);
}

// --- parsing ----------------------------------------------------------------

NF_TEST(a_binding_file_is_read_with_comments_signs_and_aliases) {
    const std::string text =
        "# Crate Hunter bindings\n"
        "\n"
        "jump = key:space, pad:a\n"
        "  move_forward  =  key:w , key:up   # both\n"
        "move_x = -key:A, +key:D, axis:LeftX\n"
        "sprint = axis:lt\n";

    std::string error;
    const InputBindings b = InputBindings::parse(text, &error);
    NF_CHECK(error.empty());
    NF_CHECK(b.action_count() == 4);

    FakeDevice space;
    space.press(0x20);
    NF_CHECK(binding_pressed(b, "jump", space.state()));

    FakeDevice w;
    w.press('W');
    NF_CHECK(binding_pressed(b, "move_forward", w.state()));

    FakeDevice d;
    d.press('D');
    NF_CHECK_NEAR(binding_axis(b, "move_x", d.state()), 1.0f, kEps);
    FakeDevice a;
    a.press('A');
    NF_CHECK_NEAR(binding_axis(b, "move_x", a.state()), -1.0f, kEps);

    // An action the file did not mention is not in the PARSED table: parse
    // returns what the file says and nothing else, so a test can tell the two
    // apart. The runtime overlays it on the defaults (see the merge case).
    FakeDevice shift;
    shift.press(0x10);
    NF_CHECK(b.find("interact") == nullptr);
}

NF_TEST(a_file_overlays_the_defaults_instead_of_replacing_them) {
    // The reason this is a merge: rebinding one key must not delete the rest
    // of the control scheme. A player who reports "jump stopped working"
    // because they edited the sprint line is the bug this prevents.
    std::string error;
    const InputBindings file = InputBindings::parse("jump = key:q\n", &error);
    NF_CHECK(error.empty());

    InputBindings live = InputBindings::defaults();
    live.merge(file);

    FakeDevice q;
    q.press('Q');
    NF_CHECK(binding_pressed(live, "jump", q.state()));
    // The old binding is GONE for that action — a rebind replaces, it does
    // not add a second key nobody asked for.
    FakeDevice space;
    space.press(0x20);
    NF_CHECK(!binding_pressed(live, "jump", space.state()));
    // ...and everything the file did not name still works.
    FakeDevice e;
    e.press('E');
    NF_CHECK(binding_pressed(live, "interact", e.state()));
    FakeDevice w;
    w.press('W');
    NF_CHECK_NEAR(binding_axis(live, "move_z", w.state()), -1.0f, kEps);
}

NF_TEST(a_broken_line_is_reported_and_not_applied) {
    std::string error;
    const InputBindings b = InputBindings::parse("jump = key:nosuchkey\n", &error);
    NF_CHECK(!error.empty());
    // The line failed, so the action has no bindings at all — it does not
    // silently fall back to the default, because a table that is half old and
    // half new is the one thing a player cannot debug.
    NF_CHECK(b.find("jump") == nullptr);

    std::string missing_eq;
    InputBindings::parse("jump key:space\n", &missing_eq);
    NF_CHECK(!missing_eq.empty());

    std::string bad_kind;
    InputBindings::parse("jump = trigger:space\n", &bad_kind);
    NF_CHECK(!bad_kind.empty());

    // The error names the line, because a binding file is edited by hand.
    std::string numbered;
    InputBindings::parse("# comment\n\njump = key:nope\n", &numbered);
    NF_CHECK(numbered.find("line 3") != std::string::npos);
}

NF_TEST(names_round_trip_through_the_tables) {
    u16 code = 0;
    NF_CHECK(key_code_from_name("w", code));
    NF_CHECK(code == 'W');
    NF_CHECK(key_code_from_name("SPACE", code));
    NF_CHECK(code == 0x20);
    NF_CHECK(key_code_from_name("Up", code));
    NF_CHECK(code == 0x26);
    NF_CHECK(!key_code_from_name("nosuchkey", code));

    NF_CHECK(pad_button_from_name("a", code));
    NF_CHECK(code == 0);
    NF_CHECK(pad_button_from_name("DPadLeft", code));
    NF_CHECK(code == 13);
    NF_CHECK(!pad_button_from_name("trigger", code));

    NF_CHECK(pad_axis_from_name("LeftX", code));
    NF_CHECK(code == 0);
    NF_CHECK(pad_axis_from_name("rt", code));
    NF_CHECK(code == 5);
    NF_CHECK(!pad_axis_from_name("LeftZ", code));

    // The inverse is what a rebinding UI prints.
    NF_CHECK(key_name_from_code('W') == "W");
    NF_CHECK(key_name_from_code(0x20) == "space");
    NF_CHECK(key_name_from_code(0xFFF) == std::string_view{});
}

NF_TEST(an_empty_table_answers_nothing) {
    const InputBindings empty;
    FakeDevice d;
    d.press('W');
    NF_CHECK(!binding_pressed(empty, "move_forward", d.state()));
    NF_CHECK_NEAR(binding_axis(empty, "move_x", d.state()), 0.0f, kEps);
    NF_CHECK(empty.empty());
}

NF_TEST(a_device_with_no_callbacks_answers_nothing) {
    // A headless run hands the evaluator a device with null callbacks. It must
    // report "not held" rather than crash — this is the shape of every
    // packaged game's first frame before a window exists.
    const InputBindings b = InputBindings::defaults();
    const DeviceState none;
    NF_CHECK(!binding_pressed(b, "jump", none));
    NF_CHECK_NEAR(binding_axis(b, "move_x", none), 0.0f, kEps);
}
