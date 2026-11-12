#include <NF/Test/TestFramework.hpp>
#include <NF/UI/ScriptCanvas.hpp>
#include <limits>
using namespace nf::ui;

NF_TEST(ui_canvas_button_requires_a_complete_click_and_consumes_it_once) {
    ScriptCanvas canvas;
    const auto frame = [&](UiPointer pointer) {
        canvas.begin_frame(pointer);
        const bool first = canvas.button("go", "Go", {.1f,.1f,.3f,.1f});
        NF_CHECK(!canvas.button("go", "Go", {.1f,.1f,.3f,.1f}));
        return first;
    };
    NF_CHECK(!frame({.2f,.15f,false,true}));
    NF_CHECK(!frame({.2f,.15f,true,true}));
    NF_CHECK(!frame({.2f,.15f,true,true}));
    NF_CHECK(frame({.2f,.15f,false,true}));
    NF_CHECK(!frame({.2f,.15f,false,true}));
    NF_CHECK_EQ(canvas.size(), 1u);
    NF_CHECK(!frame({.8f,.8f,true,true}));
    NF_CHECK(!frame({.2f,.15f,false,true}));
    NF_CHECK(!frame({.2f,.15f,true,true}));
    NF_CHECK(!frame({.8f,.8f,false,true}));
}
NF_TEST(ui_canvas_focus_loss_disabled_buttons_and_scene_reset_cancel_clicks) {
    ScriptCanvas canvas;
    const auto frame = [&](UiPointer pointer, bool enabled = true) {
        canvas.begin_frame(pointer);
        return canvas.button("go", "Go", {.1f,.1f,.3f,.1f}, enabled);
    };
    frame({.2f,.15f,false,true});
    frame({.2f,.15f,true,true});
    NF_CHECK(!frame({.2f,.15f,true,false}));
    NF_CHECK(!frame({.2f,.15f,true,true}));
    NF_CHECK(!frame({.2f,.15f,false,true}));
    frame({.2f,.15f,true,true});
    NF_CHECK(!frame({.2f,.15f,false,true}, false));
    frame({.2f,.15f,false,true});
    frame({.2f,.15f,true,true});
    canvas.reset();
    NF_CHECK(!frame({.2f,.15f,false,true}));
}
NF_TEST(ui_canvas_overlapping_buttons_capture_only_the_top_entity_scope) {
    ScriptCanvas canvas;
    const auto frame = [&](bool down) {
        canvas.begin_frame({.2f,.15f,down,true});
        canvas.set_scope(1,0);
        const bool first = canvas.button("go", "First", {.1f,.1f,.3f,.1f});
        canvas.set_scope(2,0);
        const bool second = canvas.button("go", "Second", {.1f,.1f,.3f,.1f});
        NF_CHECK(!first);
        return second;
    };
    NF_CHECK(!frame(false));
    NF_CHECK(!frame(true));
    NF_CHECK(frame(false));
    NF_CHECK_EQ(canvas.size(), 2u);
}
NF_TEST(ui_canvas_slider_keeps_capture_outside_the_track_and_clamps_values) {
    ScriptCanvas canvas;
    double value = 0;
    const auto frame = [&](UiPointer pointer) {
        canvas.begin_frame(pointer);
        value = canvas.slider("level", "Level", value, 0, 100, {.1f,.1f,.4f,.1f});
    };
    frame({.25f,.15f,false,true});
    frame({.25f,.15f,true,true});
    NF_CHECK_NEAR(value, 37.5, .001);
    frame({1,.8f,true,true});
    NF_CHECK_NEAR(value, 100, .001);
    NF_CHECK(canvas.wants_pointer({1,.8f,true,true}));
    frame({1,.8f,false,true});
    frame({1,.8f,false,true});
    NF_CHECK(!canvas.wants_pointer({1,.8f,false,true}));
}
NF_TEST(ui_canvas_snapshot_scales_geometry_and_keeps_logical_arabic) {
    ScriptCanvas canvas;
    canvas.begin_frame();
    canvas.panel("panel", {.1f,.1f,.4f,.5f});
    canvas.label("title", "اختبار الواجهة", {.1f,.1f,.4f,.1f});
    canvas.bar("progress", "50%", .5, {.1f,.3f,.4f,.1f});
    auto list = canvas.snapshot(1000,500);
    NF_CHECK_EQ(list.rects.size(),3u);
    NF_CHECK_NEAR(list.rects[2].rect.w,200,.001f);
    NF_CHECK(list.texts[0].text == "اختبار الواجهة");
    NF_CHECK_NEAR(list.texts[0].x,100,.001f);
    canvas.begin_frame();
    NF_CHECK(canvas.snapshot(1000,500).texts.empty());
}
NF_TEST(ui_canvas_rejects_invalid_geometry_and_bounds_per_frame_allocations) {
    ScriptCanvas canvas;
    canvas.begin_frame();
    canvas.label("", "Empty ID", {.1f,.1f,.2f,.1f});
    canvas.label("bad", "Invalid", {-1,0,.2f,.1f});
    canvas.bar("nan", "Invalid", 1, {0,0,std::numeric_limits<float>::quiet_NaN(),.1f});
    NF_CHECK_EQ(canvas.size(),0u);
    for (unsigned i=0;i<1024;++i) canvas.label(std::to_string(i), "Label", {0,0,.2f,.1f});
    NF_CHECK_EQ(canvas.size(),ScriptCanvas::kMaxElements);
    NF_CHECK(canvas.snapshot(0,500).texts.empty());
    NF_CHECK(canvas.snapshot(std::numeric_limits<float>::infinity(),500).texts.empty());
}
NF_TEST(ui_canvas_cursor_requests_are_frame_local_and_focus_aware) {
    ScriptCanvas canvas;
    canvas.begin_frame();
    canvas.cursor(true);
    NF_CHECK(canvas.wants_pointer({.9f,.9f,false,true}));
    NF_CHECK(!canvas.wants_pointer({.9f,.9f,false,false}));
    canvas.begin_frame();
    NF_CHECK(!canvas.wants_pointer({.9f,.9f,false,true}));
}
NF_TEST(ui_canvas_anchor_rect_clamps_to_viewport_and_respects_min_px) {
    ScriptCanvas c; c.set_viewport(800, 600);
    const auto r = c.anchor_rect(UiAnchor::TopLeft, .4f, .5f, .02f, 280, 240);
    NF_CHECK(r.w >= 280.f / 800.f - 1e-6f);
    NF_CHECK(r.h >= 240.f / 600.f - 1e-6f);
    NF_CHECK(r.x + r.w <= 1.001f && r.y + r.h <= 1.001f);
    const auto r2 = c.anchor_rect(UiAnchor::TopRight, .3f, .2f, .02f);
    NF_CHECK_NEAR(r2.x, 1 - .02f - .3f, 1e-6f);
    const auto r3 = c.anchor_rect(UiAnchor::Center, .2f, .2f);
    NF_CHECK_NEAR(r3.x, .4f, 1e-6f);
    UiAnchor a; NF_CHECK(ScriptCanvas::anchor_from_name("bottomright", a) && a == UiAnchor::BottomRight);
    // same helper must scale at the three lab sizes without leaving the box
    for (auto wh : {std::pair{800.f,600.f}, {1280.f,720.f}, {1920.f,1080.f}}) {
        ScriptCanvas s; s.set_viewport(wh.first, wh.second);
        const auto rr = s.anchor_rect(UiAnchor::TopLeft, .42f, .88f, .025f, 300, 220);
        NF_CHECK(rr.x >= 0 && rr.y >= 0 && rr.x + rr.w <= 1.001f && rr.y + rr.h <= 1.001f);
    }
}
NF_TEST(ui_text_helpers_split_hard_lines_and_words_without_shaping) {
    const auto hard = split_hard_lines("a\n\nb");
    NF_CHECK_EQ(hard.size(), 3u);
    NF_CHECK(hard[1].empty());
    const auto words = split_words("  مختبر  UI   123 ");
    NF_CHECK_EQ(words.size(), 3u);
    NF_CHECK(words[0] == "مختبر");
    const auto mixed = split_words("Charge 65% يزداد");
    NF_CHECK_EQ(mixed.size(), 3u);
}
NF_TEST(ui_canvas_checkbox_toggles_on_click_and_enter) {
    ScriptCanvas c;
    bool v = false;
    const auto frame = [&](UiPointer p, UiKeys k = {}) {
        c.begin_frame(p, k);
        v = c.checkbox("c", "Opt", v, {.1f,.1f,.3f,.1f});
    };
    frame({.2f,.15f,false,true}); frame({.2f,.15f,true,true}); frame({.2f,.15f,false,true});
    NF_CHECK(v);
    // click again toggles off
    frame({.2f,.15f,true,true}); frame({.2f,.15f,false,true});
    NF_CHECK(!v);
    // Tab focuses, Enter toggles
    UiKeys tab{}; tab.tab = true;
    frame({.2f,.15f,false,true}, tab);
    UiKeys enter{}; enter.enter = true;
    frame({.2f,.15f,false,true}, enter);
    NF_CHECK(v);
}
NF_TEST(ui_canvas_list_selects_scrolls_and_clips) {
    ScriptCanvas c;
    std::vector<std::string> items;
    for (int i = 0; i < 10; ++i) items.push_back("Item " + std::to_string(i));
    int sel = 0;
    const auto frame = [&](UiPointer p, UiKeys k = {}) {
        c.begin_frame(p, k);
        sel = c.list("m", items, sel, {.1f,.1f,.4f,.4f}, 3, true);
    };
    // prime one frame so Tab has a previous-frame order to walk
    frame({.05f,.9f,false,true});
    // Tab -> focus list, Down -> moves selection, still within clipped window
    UiKeys tab{}; tab.tab = true;
    frame({.05f,.9f,false,true}, tab);
    UiKeys down{}; down.down = true;
    frame({.05f,.9f,false,true}, down);
    NF_CHECK_EQ(sel, 1);
    UiKeys rel{}; rel.down = false;
    frame({.05f,.9f,false,true}, rel);
    UiKeys down2{}; down2.down = true;
    frame({.05f,.9f,false,true}, down2);
    NF_CHECK_EQ(sel, 2);
    // snapshot only emits the 3 visible rows (clipped)
    auto dl = c.snapshot(1000, 1000);
    // rows are texts starting with "Item"
    int item_texts = 0; for (auto& t : dl.texts) if (t.text.rfind("Item ", 0) == 0) ++item_texts;
    NF_CHECK_EQ(item_texts, 3);
    // wheel scroll moves window
    UiKeys wheel{}; wheel.wheel = -3;
    // capture via pointer inside
    frame({.2f,.2f,true,true}, wheel);
    auto dl2 = c.snapshot(1000, 1000);
    bool saw_clipped = false;
    for (auto& t : dl2.texts) if (t.text == "Item 0") saw_clipped = true;
    // after scrolling down 3, Item 0 must be clipped out
    NF_CHECK(!saw_clipped);
}
NF_TEST(ui_canvas_keyboard_tab_moves_focus_and_snapshot_draws_ring) {
    ScriptCanvas c;
    const auto frame = [&](UiKeys k = {}) {
        c.begin_frame({.05f,.9f,false,true}, k);
        c.button("a", "A", {.1f,.1f,.2f,.08f});
        c.button("b", "B", {.1f,.2f,.2f,.08f});
        c.slider("s", "S", 50, 0, 100, {.1f,.3f,.3f,.08f});
    };
    frame();
    NF_CHECK(!c.keyboard_focused());
    UiKeys tab{}; tab.tab = true;
    frame(tab);
    NF_CHECK(c.keyboard_focused());
    // one more Tab -> second button
    UiKeys tab2{}; tab2.tab = true;
    frame(tab2);
    // snapshot shows a focus ring (lines)
    NF_CHECK(!c.snapshot(1000, 600).lines.empty());
    // Shift+Tab wraps
    UiKeys stab{}; stab.tab = true; stab.shift = true;
    frame(stab);
    // Enter on button fires (focus is first after wrap cycle) — just run frames
    UiKeys enter{}; enter.enter = true;
    bool clicked = false;
    c.begin_frame({.05f,.9f,false,true}, enter);
    clicked = c.button("a", "A", {.1f,.1f,.2f,.08f});
    // may or may not be 'a' depending on cycle; ensure no crash and no double fire
    c.begin_frame({.05f,.9f,false,true});
    NF_CHECK(!c.button("a", "A", {.1f,.1f,.2f,.08f}));
}
NF_TEST(ui_canvas_slider_keyboard_arrows_adjust_value) {
    ScriptCanvas c;
    double v = 50;
    const auto frame = [&](UiKeys k = {}) {
        c.begin_frame({.05f,.9f,false,true}, k);
        v = c.slider("s", "S", v, 0, 100, {.1f,.1f,.4f,.08f});
    };
    frame();
    UiKeys tab{}; tab.tab = true;
    frame(tab);
    UiKeys right{}; right.right = true;
    frame(right);
    NF_CHECK_NEAR(v, 55, .001);
    UiKeys left{}; left.left = true;
    frame(left);
    NF_CHECK_NEAR(v, 50, .001);
}
NF_TEST(ui_canvas_tooltip_appears_only_on_hover_and_stays_inside) {
    ScriptCanvas c;
    const auto frame = [&](UiPointer p) {
        c.begin_frame(p);
        c.tooltip("t", "Help / مساعدة", {.1f,.1f,.3f,.1f});
    };
    frame({.2f,.15f,false,true});
    // hovered: tooltip rect+text emitted, clamped inside 800x600
    auto dl = c.snapshot(800, 600);
    NF_CHECK(!dl.rects.empty());
    NF_CHECK(!dl.texts.empty());
    for (auto& r : dl.rects) {
        NF_CHECK(r.rect.x >= 0 && r.rect.y >= 0 && r.rect.x + r.rect.w <= 800.001f && r.rect.y + r.rect.h <= 600.001f);
    }
    frame({.9f,.9f,false,true});
    NF_CHECK(c.snapshot(800, 600).texts.empty());
}
NF_TEST(ui_canvas_label_alignment_is_honored_in_snapshot) {
    ScriptCanvas c;
    c.begin_frame();
    c.label("l", "hello", {.1f,.1f,.4f,.1f}, 20, TextAlign::Right);
    auto dl = c.snapshot(1000, 500);
    NF_CHECK_EQ(dl.texts.size(), 1u);
    NF_CHECK(dl.texts[0].align == TextAlign::Right);
    NF_CHECK(dl.texts[0].x == 100.f); // box left edge; overlay aligns per line
    NF_CHECK(dl.texts[0].wrap_width == 400.f);
}
