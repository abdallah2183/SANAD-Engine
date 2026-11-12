// Text + number input behaviour: focus, editing, commit/cancel, validation,
// focus-loss and scene-reset safety. Headless: UiKeys drive everything.
#include <NF/Test/TestFramework.hpp>
#include <NF/UI/ScriptCanvas.hpp>
#include <string>
using namespace nf::ui;

namespace {
UiPointer inside(float x = 0.2f, float y = 0.15f, bool down = false) {
    return {x, y, down, true};
}
UiKeys char_key(const std::string& utf8_cp) {
    UiKeys k;
    k.text = {utf8_cp};
    return k;
}
UiKeys enter_key() { UiKeys k; k.enter = true; return k; }
UiKeys escape_key() { UiKeys k; k.escape = true; return k; }
} // namespace

NF_TEST(text_input_types_arabic_and_commits_on_enter) {
    ScriptCanvas c;
    std::string value = "Hi";
    const auto frame = [&](UiPointer p, UiKeys k = {}) {
        c.begin_frame(p, k);
        auto r = c.text_input("name", value, {.1f,.1f,.4f,.1f}, true, 32);
        if (r.committed) value = r.value;
        return r;
    };
    // Tab focuses (needs a previous frame with the widget).
    frame(inside());
    UiKeys tab{}; tab.tab = true;
    frame(inside(), tab);
    NF_CHECK(c.is_text_editing());
    // Type an Arabic codepoint (س = U+0633, UTF-8 D8 B3).
    const std::string seh = "\xD8\xB3";
    frame(inside(), char_key(seh));
    NF_CHECK(c.editing_draft() == "Hi" + seh);
    // Backspace must remove the whole codepoint, not half of it.
    UiKeys bs{}; bs.backspace = true;
    frame(inside(), bs);
    NF_CHECK(c.editing_draft() == "Hi");
    NF_CHECK(c.editing_draft().size() == 2);
    // Type again then commit with Enter.
    frame(inside(), char_key(seh));
    auto r = frame(inside(), enter_key());
    NF_CHECK(r.committed);
    NF_CHECK(value == "Hi" + seh);
    NF_CHECK(!c.is_text_editing());
}

NF_TEST(text_input_home_end_arrows_and_escape_cancels) {
    ScriptCanvas c;
    std::string value = "abc";
    const auto frame = [&](UiPointer p, UiKeys k = {}) {
        c.begin_frame(p, k);
        auto r = c.text_input("f", value, {.1f,.1f,.4f,.1f}, true, 32);
        if (r.committed) value = r.value;
        return r;
    };
    frame(inside());
    UiKeys tab{}; tab.tab = true;
    frame(inside(), tab);
    UiKeys home{}; home.home = true;
    frame(inside(), home);
    frame(inside(), char_key("X"));
    NF_CHECK(c.editing_draft() == "Xabc");
    UiKeys end{}; end.end = true;
    frame(inside(), end);
    frame(inside(), char_key("Y"));
    NF_CHECK(c.editing_draft() == "XabcY");
    auto r = frame(inside(), escape_key());
    NF_CHECK(r.cancelled);
    NF_CHECK(value == "abc"); // cancelled draft never commits
    NF_CHECK(!c.is_text_editing());
}

NF_TEST(text_input_max_length_caps_codepoints) {
    ScriptCanvas c;
    std::string value = "";
    const auto frame = [&](UiKeys k) {
        c.begin_frame(inside(), k);
        auto r = c.text_input("f", value, {.1f,.1f,.4f,.1f}, true, 2);
        if (r.committed) value = r.value;
        return r;
    };
    frame({});
    UiKeys tab{}; tab.tab = true;
    frame(tab);
    frame(char_key("a"));
    frame(char_key("b"));
    frame(char_key("c")); // over cap: ignored
    NF_CHECK(c.editing_draft() == "ab");
}

NF_TEST(text_input_focus_loss_and_reset_cancel_the_draft) {
    ScriptCanvas c;
    std::string value = "keep";
    const auto frame = [&](UiPointer p, UiKeys k = {}) {
        c.begin_frame(p, k);
        auto r = c.text_input("f", value, {.1f,.1f,.4f,.1f}, true, 32);
        if (r.committed) value = r.value;
        return r;
    };
    frame(inside());
    UiKeys tab{}; tab.tab = true;
    frame(inside(), tab);
    frame(inside(), char_key("!"));
    NF_CHECK(c.is_text_editing());
    // Losing window focus cancels (draft dropped, value untouched).
    c.begin_frame({.2f,.15f,false,false});
    auto r = c.text_input("f", value, {.1f,.1f,.4f,.1f}, true, 32);
    NF_CHECK(!r.committed);
    NF_CHECK(value == "keep");
    // Scene reset also clears editing state.
    frame(inside());
    frame(inside(), tab);
    NF_CHECK(c.is_text_editing());
    c.reset();
    NF_CHECK(!c.is_text_editing());
    NF_CHECK(!c.keyboard_focused());
}

NF_TEST(number_input_interim_and_invalid_keep_last_valid) {
    ScriptCanvas c;
    double value = 10.0;
    const auto frame = [&](UiKeys k = {}) {
        c.begin_frame(inside(), k);
        auto r = c.number_input("n", "Volume", value, 0, 100, {.1f,.1f,.4f,.1f}, true, 5, true);
        if (r.committed && r.valid) value = r.value;
        return r;
    };
    frame({});
    UiKeys tab{}; tab.tab = true;
    frame(tab);
    NF_CHECK(c.is_text_editing());
    // Select-all by deleting: draft goes empty (still editing, value kept).
    UiKeys home{}; home.home = true;
    frame(home);
    UiKeys del{}; del.delete_key = true;
    frame(del);
    frame(del);
    NF_CHECK(value == 10.0);
    // Type "200" (out of range) and try to commit: rejected, stays editing.
    frame(char_key("2"));
    frame(char_key("0"));
    frame(char_key("0"));
    auto r = frame(enter_key());
    NF_CHECK(!r.committed);
    NF_CHECK(!r.valid);
    NF_CHECK(!r.error.empty());
    NF_CHECK(value == 10.0);
    NF_CHECK(c.is_text_editing());
    // Escape discards.
    auto r2 = frame(escape_key());
    NF_CHECK(r2.cancelled);
    NF_CHECK(value == 10.0);
}

NF_TEST(number_input_rejects_nan_inf_and_overflow) {
    ScriptCanvas c;
    double value = 5.0;
    const auto frame = [&](UiKeys k = {}) {
        c.begin_frame(inside(), k);
        auto r = c.number_input("n", "N", value, 0, 1000, {.1f,.1f,.4f,.1f}, true, 1, false);
        if (r.committed && r.valid) value = r.value;
        return r;
    };
    frame({});
    UiKeys tab{}; tab.tab = true;
    frame(tab);
    // Clear draft, type "nan".
    UiKeys home{}; home.home = true;
    frame(home);
    UiKeys del{}; del.delete_key = true;
    frame(del);
    frame(char_key("n"));
    frame(char_key("a"));
    frame(char_key("n"));
    auto r = frame(enter_key());
    NF_CHECK(!r.valid);
    NF_CHECK(value == 5.0);
    // Overflow: type a huge number.
    frame(escape_key());
    frame({});
    frame(tab);
    frame(home);
    frame(del);
    frame(del);
    for (int i = 0; i < 30; ++i) frame(char_key("9"));
    auto r2 = frame(enter_key());
    NF_CHECK(!r2.valid);
    NF_CHECK(value == 5.0);
}

NF_TEST(number_input_valid_commit_and_step) {
    ScriptCanvas c;
    double value = 50.0;
    const auto frame = [&](UiKeys k = {}) {
        c.begin_frame(inside(), k);
        auto r = c.number_input("n", "N", value, 0, 100, {.1f,.1f,.4f,.1f}, true, 5, true);
        if (r.committed && r.valid) value = r.value;
        return r;
    };
    frame({});
    UiKeys tab{}; tab.tab = true;
    frame(tab);
    UiKeys up{}; up.up = true;
    auto r = frame(up);
    NF_CHECK(r.committed);
    NF_CHECK(value == 55.0);
    // Valid text entry: clear and type 42.
    UiKeys home{}; home.home = true;
    frame(home);
    UiKeys del{}; del.delete_key = true;
    frame(del);
    frame({});
    frame(del);
    frame({});
    frame(char_key("4"));
    frame(char_key("2"));
    auto r2 = frame(enter_key());
    NF_CHECK(r2.committed && r2.valid);
    NF_CHECK(value == 42.0);
}
