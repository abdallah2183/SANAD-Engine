// UITests — runtime game UI (Game-Ready G3): retained widgets + display list,
// HUD, menus, dialogue presentation, and the main→play→pause→game-over flow.
//
// Headless: no window, no GPU, no ImGui. The display list is the render
// contract, so geometry is asserted on the baked pixel snapshot.
//
// Arabic red line: the G3 menu/HUD strings come from ui::tr() keys; the cases
// below re-resolve them after set_language(Arabic) and restore English.

#include <NF/Test/TestFramework.hpp>
#include <NF/UI/GameUI.hpp>
#include <NF/UI/Localization.hpp>
#include <NF/UI/Widgets.hpp>

#include <string>
#include <vector>

using namespace nf;
using namespace nf::ui;

namespace {

// set_language() flips a process-global; every case that touches it restores
// the previous value even if an assertion throws.
struct LanguageGuard {
    Language prev = current_language();
    ~LanguageGuard() { set_language(prev); }
};

bool has_text(const DisplayList& dl, const std::string& needle) {
    for (const DlText& t : dl.texts) {
        if (t.text.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Widgets + display list
// ---------------------------------------------------------------------------

NF_TEST(ui_canvas_find_and_snapshot_geometry) {
    Canvas c;
    Label l;
    l.id = "l1";
    l.rect = UiRect{0.5f, 0.5f, 0.25f, 0.1f};
    l.text = "hello";
    l.align = TextAlign::Left;
    c.add_label(l);

    NF_CHECK(c.find_label("l1") != nullptr);
    NF_CHECK(c.find_label("nope") == nullptr);

    const DisplayList dl = c.snapshot(1280.0f, 720.0f);
    NF_CHECK(dl.texts.size() == 1);
    NF_CHECK(dl.texts[0].text == "hello");
    NF_CHECK_NEAR(dl.texts[0].x, 0.5f * 1280.0f, 1e-3f);
    NF_CHECK_NEAR(dl.texts[0].y, 0.5f * 720.0f, 1e-3f);

    // Center/right alignment pivots on the rect's width in pixels.
    Label& ref = *c.find_label("l1");
    ref.align = TextAlign::Center;
    DisplayList dc = c.snapshot(1280.0f, 720.0f);
    NF_CHECK_NEAR(dc.texts[0].x, 640.0f + 320.0f * 0.5f, 1e-3f);
    ref.align = TextAlign::Right;
    DisplayList dr = c.snapshot(1280.0f, 720.0f);
    NF_CHECK_NEAR(dr.texts[0].x, 640.0f + 320.0f, 1e-3f);

    // A hidden widget contributes nothing; a non-positive viewport is empty.
    ref.visible = false;
    NF_CHECK(c.snapshot(1280.0f, 720.0f).texts.empty());
    NF_CHECK(c.snapshot(0.0f, 720.0f).texts.empty());
    NF_CHECK(c.snapshot(1280.0f, -1.0f).texts.empty());
}

NF_TEST(ui_progress_bar_fraction_is_clamped_in_snapshot) {
    Canvas c;
    ProgressBar b;
    b.id = "hp";
    b.rect = UiRect{0.0f, 0.0f, 0.5f, 0.1f};
    b.label.clear();
    c.add_bar(b);

    ProgressBar& bar = *c.find_bar("hp");
    bar.fraction = 0.5f;
    DisplayList dl = c.snapshot(1000.0f, 500.0f);
    NF_CHECK(dl.rects.size() == 2); // background + fill
    NF_CHECK_NEAR(dl.rects[0].rect.w, 500.0f, 1e-3f);
    NF_CHECK_NEAR(dl.rects[1].rect.w, 250.0f, 1e-3f);

    // Over/under-flow clamps; a zero fraction draws no fill rect.
    bar.fraction = 5.0f;
    dl = c.snapshot(1000.0f, 500.0f);
    NF_CHECK_NEAR(dl.rects[1].rect.w, 500.0f, 1e-3f);
    bar.fraction = 0.0f;
    dl = c.snapshot(1000.0f, 500.0f);
    NF_CHECK(dl.rects.size() == 1);
}

NF_TEST(ui_menu_list_move_wraps_and_skips_disabled) {
    MenuList m;
    m.id = "m";
    m.items = {
        {"a", "A", true},
        {"b", "B", false},
        {"c", "C", true},
    };
    m.reset_selection();
    NF_CHECK(m.selected == 0);
    NF_CHECK(m.move(1)); // 0 -> 1 disabled -> 2
    NF_CHECK(m.selected == 2);
    NF_CHECK(m.move(1)); // 2 -> wraps to 0
    NF_CHECK(m.selected == 0);
    NF_CHECK(m.move(-1)); // 0 -> wraps to 2
    NF_CHECK(m.selected == 2);
    NF_CHECK(m.activate() != nullptr && m.activate()->id == "c");

    // reset_selection lands on the first ENABLED item.
    MenuList m2;
    m2.items = {{"x", "X", false}, {"y", "Y", true}};
    m2.reset_selection();
    NF_CHECK(m2.selected == 1);

    // All-disabled: move gives up unchanged, activate refuses.
    MenuList m3;
    m3.items = {{"p", "P", false}, {"q", "Q", false}};
    m3.reset_selection();
    NF_CHECK(!m3.move(1));
    NF_CHECK(m3.activate() == nullptr);
    NF_CHECK(m3.selected_item() != nullptr); // still reports the row, just not activatable

    // Empty list is inert.
    MenuList m4;
    NF_CHECK(!m4.move(1));
    NF_CHECK(m4.activate() == nullptr);
}

NF_TEST(ui_reticle_and_slider_snapshot) {
    Canvas c;
    Reticle r;
    r.id = "ret";
    c.add_reticle(r);
    DisplayList dl = c.snapshot(1000.0f, 1000.0f);
    NF_CHECK(dl.lines.size() == 4); // four arms
    NF_CHECK(dl.rects.size() == 1); // centre dot
    NF_CHECK_NEAR(dl.rects[0].rect.x, 499.0f, 1e-3f);

    c.find_reticle("ret")->visible = false;
    NF_CHECK(c.snapshot(1000.0f, 1000.0f).lines.empty());
    c.find_reticle("ret")->visible = true; // back on for the slider case below

    Slider s;
    s.id = "master";
    s.label = "Master";
    s.value = 0.5f;
    s.rect = UiRect{0.1f, 0.1f, 0.5f, 0.05f};
    c.add_slider(s);
    DisplayList ds = c.snapshot(1000.0f, 1000.0f);
    NF_CHECK(has_text(ds, "Master: 50%"));
    NF_CHECK(ds.rects.size() == 3); // reticle dot + slider track + knob
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------

NF_TEST(ui_hud_health_fraction_is_state_not_label) {
    Hud hud;
    NF_CHECK_NEAR(hud.health_fraction(), 1.0f, 1e-6f); // default full bar
    hud.set_health(50.0f, 100.0f);
    NF_CHECK_NEAR(hud.health_fraction(), 0.5f, 1e-6f);
    hud.set_health(250.0f, 100.0f); // over-heal clamps
    NF_CHECK_NEAR(hud.health_fraction(), 1.0f, 1e-6f);
    hud.set_health(-10.0f, 100.0f); // negative clamps to 0
    NF_CHECK_NEAR(hud.health_fraction(), 0.0f, 1e-6f);
    hud.set_health(5.0f, 0.0f); // max <= 0 is 0, never a division blow-up
    NF_CHECK_NEAR(hud.health_fraction(), 0.0f, 1e-6f);

    // The fraction survives a language flip: it is stored state, never parsed
    // back out of the (possibly Arabic) label text.
    LanguageGuard guard;
    set_language(Language::Arabic);
    hud.set_health(30.0f, 100.0f);
    hud.refresh_labels();
    NF_CHECK_NEAR(hud.health_fraction(), 0.3f, 1e-6f);
    const DisplayList dl = hud.canvas().snapshot(1280.0f, 720.0f);
    NF_CHECK(has_text(dl, tr("health")));
}

NF_TEST(ui_hud_ammo_readout_and_message_timer) {
    Hud hud;
    NF_CHECK(hud.ammo_magazine() == 0 && hud.ammo_reserve() == 0);
    hud.set_ammo(30, 90);
    NF_CHECK(hud.ammo_magazine() == 30 && hud.ammo_reserve() == 90);
    const Label* ammo = hud.canvas().find_label("hud_ammo");
    NF_CHECK(ammo != nullptr);
    NF_CHECK(ammo->text.find("30 / 90") != std::string::npos);

    // Message line: shown, then hidden by its own timer; the stored text stays.
    const Label* msg = hud.canvas().find_label("hud_message");
    NF_CHECK(msg != nullptr && !msg->visible);
    hud.show_message("reload!", 1.0f);
    NF_CHECK(hud.message() == "reload!");
    NF_CHECK(hud.canvas().find_label("hud_message")->visible);
    hud.update(0.0f);  // dt <= 0 is ignored
    hud.update(-4.0f); // negative dt is ignored
    NF_CHECK(hud.canvas().find_label("hud_message")->visible);
    hud.update(0.6f);
    NF_CHECK(hud.canvas().find_label("hud_message")->visible);
    hud.update(0.6f);
    NF_CHECK(!hud.canvas().find_label("hud_message")->visible);
    NF_CHECK(hud.message() == "reload!"); // state kept, only the line hides

    // Zero-second message hides on the very next tick.
    hud.show_message("now", 0.0f);
    hud.update(0.016f);
    NF_CHECK(!hud.canvas().find_label("hud_message")->visible);
}

NF_TEST(ui_hud_reticle_toggle) {
    Hud hud;
    NF_CHECK(hud.reticle_visible());
    hud.set_reticle_visible(false);
    NF_CHECK(!hud.reticle_visible());
    hud.set_reticle_visible(true);
    NF_CHECK(hud.reticle_visible());
    NF_CHECK(hud.canvas().snapshot(1280.0f, 720.0f).lines.size() == 4);
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------

NF_TEST(ui_menu_default_ctor_is_inert_and_typed_menu_works) {
    Menu empty;
    NF_CHECK(empty.item_count() == 0);
    NF_CHECK(empty.selected() == nullptr);
    NF_CHECK(empty.activate() == nullptr);
    NF_CHECK(!empty.move(1));
    empty.set_items({{"a", "A", true}}); // no canvas menu to write into: no-op
    NF_CHECK(empty.item_count() == 0);

    Menu m{"main", "Main"};
    NF_CHECK(m.title() == "Main");
    m.set_items({{"a", "A", true}, {"b", "B", true}});
    NF_CHECK(m.item_count() == 2);
    NF_CHECK(m.item(0) != nullptr && m.item(0)->id == "a");
    NF_CHECK(m.item(9) == nullptr);
    NF_CHECK(m.selected() != nullptr && m.selected()->id == "a");
    NF_CHECK(m.move(1));
    NF_CHECK(m.selected() != nullptr && m.selected()->id == "b");
    const MenuItem* it = m.activate();
    NF_CHECK(it != nullptr && it->id == "b");
    m.set_title("Renamed");
    NF_CHECK(m.title() == "Renamed");
}

// ---------------------------------------------------------------------------
// Dialogue presentation
// ---------------------------------------------------------------------------

namespace {

gameplay::DialogueTree make_dialogue_tree() {
    gameplay::DialogueTree tree;
    tree.add_node({"start", "Guide", "Welcome, traveller.", "hub", {}});
    gameplay::DialogueNode hub;
    hub.id = "hub";
    hub.speaker = "Guide";
    hub.text = "What do you want to know?";
    hub.choices = {
        {"ask_gate", "Ask about the gate", "gate", "", "met_guide"},
        {"ask_king", "Ask about the king", "king", "knows_king", ""},
    };
    tree.add_node(hub);
    tree.add_node({"gate", "Guide", "The gate is north.", "", {}});
    tree.add_node({"king", "Guide", "The king is dead.", "", {}});
    return tree;
}

} // namespace

NF_TEST(ui_dialogue_presenter_gates_choices_and_grants_tags) {
    gameplay::DialogueTree tree = make_dialogue_tree();
    gameplay::TagContainer tags;
    DialoguePresenter dp;

    NF_CHECK(!dp.active());
    NF_CHECK(dp.start(tree, "start", tags));
    NF_CHECK(dp.active());
    NF_CHECK(dp.speaker() == "Guide");
    NF_CHECK(dp.text() == "Welcome, traveller.");
    NF_CHECK(dp.choice_count() == 0);

    // A choiceless node advances with Confirm.
    NF_CHECK(dp.confirm());
    NF_CHECK(dp.active());
    NF_CHECK(dp.text() == "What do you want to know?");
    // ask_king is gated behind "knows_king", which the player does not hold.
    NF_CHECK(dp.choice_count() == 1);

    // move_selection wraps over the single available choice.
    NF_CHECK(dp.move_selection(1));
    NF_CHECK(dp.selected_choice() == 0);

    NF_CHECK(dp.confirm()); // picks ask_gate -> grants met_guide, jumps to gate
    NF_CHECK(dp.active());
    NF_CHECK(dp.text() == "The gate is north.");
    NF_CHECK(tags.has_exact("met_guide"));
    NF_CHECK(dp.choice_count() == 0);

    NF_CHECK(!dp.confirm()); // gate has no next: the run ends
    NF_CHECK(!dp.active());
    NF_CHECK(dp.canvas().labels.empty());

    // With the gate tag held, the king option is now selectable.
    tags.add("knows_king");
    NF_CHECK(dp.start(tree, "hub", tags));
    NF_CHECK(dp.choice_count() == 2);
    NF_CHECK(dp.move_selection(1) && dp.selected_choice() == 1);
    NF_CHECK(dp.confirm());
    NF_CHECK(dp.text() == "The king is dead.");
}

NF_TEST(ui_dialogue_presenter_missing_start_stays_closed) {
    gameplay::DialogueTree tree = make_dialogue_tree();
    gameplay::TagContainer tags;
    DialoguePresenter dp;
    NF_CHECK(!dp.start(tree, "no_such_node", tags));
    NF_CHECK(!dp.active());
    NF_CHECK(dp.choice_count() == 0);
    NF_CHECK(dp.speaker().empty());
    NF_CHECK(!dp.confirm()); // inert, never a crash
    NF_CHECK(!dp.move_selection(1));
}

NF_TEST(ui_dialogue_presenter_close_and_restart) {
    gameplay::DialogueTree tree = make_dialogue_tree();
    gameplay::TagContainer tags;
    DialoguePresenter dp;
    NF_CHECK(dp.start(tree, "start", tags));
    dp.close();
    NF_CHECK(!dp.active());
    NF_CHECK(dp.text().empty());
    NF_CHECK(dp.start(tree, "hub", tags)); // a fresh run works after close
    NF_CHECK(dp.active());
    NF_CHECK(dp.text() == "What do you want to know?");
}

// ---------------------------------------------------------------------------
// GameFlow — the acceptance path
// ---------------------------------------------------------------------------

NF_TEST(ui_flow_main_play_pause_game_over) {
    GameFlow flow;
    NF_CHECK(flow.screen() == Screen::MainMenu);
    NF_CHECK(std::string(screen_name(flow.screen())) == "main_menu");
    NF_CHECK(flow.main_menu().item_count() == 3);

    int started = 0, resumed = 0, restarted = 0;
    flow.on_start_game = [&] { ++started; };
    flow.on_resume = [&] { ++resumed; };
    flow.on_restart = [&] { ++restarted; };

    // main -> play
    flow.handle(Action::Confirm);
    NF_CHECK(flow.screen() == Screen::Playing);
    NF_CHECK(started == 1);
    NF_CHECK(flow.active_canvas() == &flow.hud().canvas());

    // play -> pause -> play
    flow.handle(Action::Back);
    NF_CHECK(flow.screen() == Screen::Paused);
    NF_CHECK(flow.active_canvas() == &flow.pause_menu().canvas());
    flow.handle(Action::Back); // Esc resumes
    NF_CHECK(flow.screen() == Screen::Playing);
    NF_CHECK(resumed == 1);

    // pause via the menu's "resume" row
    flow.handle(Action::Back);
    NF_CHECK(flow.screen() == Screen::Paused);
    flow.handle(Action::Confirm);
    NF_CHECK(flow.screen() == Screen::Playing);
    NF_CHECK(resumed == 2);

    // play -> game over
    flow.notify_game_over();
    NF_CHECK(flow.screen() == Screen::GameOver);
    NF_CHECK(flow.active_canvas() == &flow.game_over_menu().canvas());

    // game over -> restart
    flow.handle(Action::Confirm);
    NF_CHECK(flow.screen() == Screen::Playing);
    NF_CHECK(restarted == 1);

    // game over -> quit
    flow.notify_game_over();
    int quits = 0;
    flow.on_quit = [&] { ++quits; };
    flow.game_over_menu().move(1); // restart -> quit
    flow.handle(Action::Confirm);
    NF_CHECK(quits == 1);

    // Action::None never moves anything.
    const Screen before = flow.screen();
    flow.handle(Action::None);
    NF_CHECK(flow.screen() == before);
}

NF_TEST(ui_flow_pause_menu_rows) {
    GameFlow flow;
    flow.handle(Action::Confirm); // play
    flow.handle(Action::Back);    // pause

    int resumed = 0, restarted = 0;
    flow.on_resume = [&] { ++resumed; };
    flow.on_restart = [&] { ++restarted; };

    // rows: resume, settings, restart, quit
    NF_CHECK(flow.pause_menu().item_count() == 4);
    flow.pause_menu().move(2); // -> restart
    flow.handle(Action::Confirm);
    NF_CHECK(flow.screen() == Screen::Playing);
    NF_CHECK(restarted == 1);

    // settings opened from pause returns to pause, not to the main menu.
    flow.handle(Action::Back); // pause
    flow.pause_menu().move(-1); // selection is still on "restart" (2) -> 1 = settings
    flow.handle(Action::Confirm);
    NF_CHECK(flow.screen() == Screen::Settings);
    flow.handle(Action::Back);
    NF_CHECK(flow.screen() == Screen::Paused);
    NF_CHECK(resumed == 0);
}

NF_TEST(ui_flow_settings_sliders_drive_volume_contract) {
    GameFlow flow;
    int changed = 0;
    flow.on_settings_changed = [&] { ++changed; };

    // main -> settings
    flow.main_menu().move(1); // -> settings row
    flow.handle(Action::Confirm);
    NF_CHECK(flow.screen() == Screen::Settings);
    NF_CHECK(flow.settings_menu().item_count() == 5); // 4 sliders + back

    // Row 0 is "master"; the slider starts at the SettingsData value (1.0).
    const Slider* master = flow.settings_menu().canvas().find_slider("master");
    NF_CHECK(master != nullptr);
    NF_CHECK_NEAR(master->value, 1.0f, 1e-6f);

    flow.handle(Action::Left); // 1.0 - 0.05
    NF_CHECK_NEAR(flow.settings().master_volume, 0.95f, 1e-6f);
    NF_CHECK(changed >= 1);
    flow.handle(Action::Right); // back to 1.0
    NF_CHECK_NEAR(flow.settings().master_volume, 1.0f, 1e-6f);

    // Row 1 is "music".
    flow.handle(Action::Down);
    flow.handle(Action::Left);
    NF_CHECK_NEAR(flow.settings().music_volume, 0.95f, 1e-6f);

    // Row 2 is "sfx".
    flow.handle(Action::Down);
    flow.handle(Action::Left);
    NF_CHECK_NEAR(flow.settings().sfx_volume, 0.95f, 1e-6f);

    // Row 3 is "sensitivity" — gameplay, not an audio bus, so no bus callback.
    const int changed_before_sens = changed;
    flow.handle(Action::Down);
    flow.handle(Action::Left);
    NF_CHECK_NEAR(flow.settings().mouse_sensitivity, 0.95f, 1e-6f);
    NF_CHECK(changed == changed_before_sens);

    // Row 4 is "back": Confirm on it leaves settings (and does not touch a bus).
    const float master_before = flow.settings().master_volume;
    flow.handle(Action::Down);
    flow.handle(Action::Confirm);
    NF_CHECK(flow.screen() == Screen::MainMenu);
    NF_CHECK_NEAR(flow.settings().master_volume, master_before, 1e-6f);

    // The bus table itself clamps and rejects unknown names.
    NF_CHECK(flow.settings().set_volume("music", 2.0f));
    NF_CHECK_NEAR(flow.settings().volume("music"), 1.0f, 1e-6f);
    NF_CHECK(flow.settings().set_volume("music", -3.0f));
    NF_CHECK_NEAR(flow.settings().volume("music"), 0.0f, 1e-6f);
    NF_CHECK(!flow.settings().set_volume("nope", 0.5f));
    NF_CHECK_NEAR(flow.settings().volume("nope"), -1.0f, 1e-6f);
    NF_CHECK_NEAR(flow.settings().volume("master"), 1.0f, 1e-6f);
}

NF_TEST(ui_flow_dialogue_captures_input_while_playing) {
    GameFlow flow;
    flow.handle(Action::Confirm); // play

    gameplay::DialogueTree tree = make_dialogue_tree();
    gameplay::TagContainer tags;
    NF_CHECK(flow.dialogue().start(tree, "hub", tags));
    NF_CHECK(flow.dialogue().active());

    // Back closes the dialogue instead of pausing — dialogue owns the input.
    flow.handle(Action::Down);
    NF_CHECK(flow.screen() == Screen::Playing);
    flow.handle(Action::Back);
    NF_CHECK(!flow.dialogue().active());
    NF_CHECK(flow.screen() == Screen::Playing);

    // Now Back pauses again.
    flow.handle(Action::Back);
    NF_CHECK(flow.screen() == Screen::Paused);
}

NF_TEST(ui_flow_active_canvas_matches_every_screen) {
    GameFlow flow;
    NF_CHECK(flow.active_canvas() == &flow.main_menu().canvas());
    flow.handle(Action::Confirm);
    NF_CHECK(flow.active_canvas() == &flow.hud().canvas());
    flow.handle(Action::Back);
    NF_CHECK(flow.active_canvas() == &flow.pause_menu().canvas());
    flow.handle(Action::Back);
    flow.notify_game_over();
    NF_CHECK(flow.active_canvas() == &flow.game_over_menu().canvas());
    flow.notify_game_over(); // idempotent
    NF_CHECK(flow.screen() == Screen::GameOver);
}

// ---------------------------------------------------------------------------
// Localization (Arabic red line for the G3 keys)
// ---------------------------------------------------------------------------

NF_TEST(ui_flow_labels_re_resolve_after_language_change) {
    LanguageGuard guard;

    set_language(Language::English);
    GameFlow flow;
    NF_CHECK(flow.main_menu().item(0) != nullptr);
    NF_CHECK(flow.main_menu().item(0)->label == tr("new_game"));
    NF_CHECK(flow.main_menu().item(0)->label == "New Game");

    set_language(Language::Arabic);
    flow.refresh_labels();
    NF_CHECK(flow.main_menu().title() == tr("main_menu"));
    NF_CHECK(flow.main_menu().item(0)->label == "لعبة جديدة");
    NF_CHECK(flow.main_menu().item(0)->label != "New Game");
    NF_CHECK(flow.pause_menu().item(0)->label == "استئناف");
    NF_CHECK(flow.game_over_menu().item(0)->label == "إعادة المحاولة");

    // Sliders keep their values and only their labels re-resolve.
    const Slider* master = flow.settings_menu().canvas().find_slider("master");
    NF_CHECK(master != nullptr);
    NF_CHECK(master->label == "الصوت العام");

    // HUD labels re-resolve too, and the state stays put.
    flow.hud().set_ammo(7, 21);
    flow.hud().refresh_labels();
    const Label* ammo = flow.hud().canvas().find_label("hud_ammo");
    NF_CHECK(ammo != nullptr);
    NF_CHECK(ammo->text.find("7 / 21") != std::string::npos);
    NF_CHECK(ammo->text.find("الذخيرة") != std::string::npos);
}

NF_TEST(ui_g3_localization_keys_present_in_both_languages) {
    LanguageGuard guard;
    const char* keys[] = {"main_menu", "new_game", "resume",  "restart",  "quit",
                          "game_over", "health",   "ammo",    "back",     "master_volume",
                          "music_volume", "sfx_volume", "sensitivity", "dialogue_continue",
                          "paused", "settings"};
    for (const char* key : keys) {
        set_language(Language::English);
        const std::string en = tr(key);
        NF_CHECK(!en.empty());
        NF_CHECK(en != key); // a miss would echo the key back
        set_language(Language::Arabic);
        const std::string ar = tr(key);
        NF_CHECK(!ar.empty());
        NF_CHECK(ar != key);
        // Arabic values must actually be Arabic (a Latin copy is a miss).
        NF_CHECK(ar != en);
        bool has_arabic = false;
        for (char c : ar) {
            if (static_cast<unsigned char>(c) >= 0xD8) { // Arabic UTF-8 lead byte
                has_arabic = true;
                break;
            }
        }
        NF_CHECK(has_arabic);
    }
    // No duplicate key may shadow a G3 row.
    NF_CHECK(tr_duplicate_count() == 0);
}

// ---------------------------------------------------------------------------
// Action name vocabulary (shared by Lua + C#)
// ---------------------------------------------------------------------------

NF_TEST(ui_action_name_round_trip) {
    NF_CHECK(std::string(action_name(Action::None)) == "none");
    NF_CHECK(std::string(action_name(Action::Confirm)) == "confirm");
    NF_CHECK(action_from_name("confirm") == Action::Confirm);
    NF_CHECK(action_from_name("CONFIRM") == Action::Confirm); // case-insensitive
    NF_CHECK(action_from_name("Back") == Action::Back);
    NF_CHECK(action_from_name("up") == Action::Up);
    NF_CHECK(action_from_name("down") == Action::Down);
    NF_CHECK(action_from_name("left") == Action::Left);
    NF_CHECK(action_from_name("right") == Action::Right);
    NF_CHECK(action_from_name("nonsense") == Action::None); // fail to None, never throw
    NF_CHECK(action_from_name("") == Action::None);
    NF_CHECK(action_from_name(nullptr) == Action::None);
    const Action all[] = {Action::None, Action::Up,    Action::Down, Action::Left,
                          Action::Right, Action::Confirm, Action::Back};
    for (Action a : all) {
        NF_CHECK(action_from_name(action_name(a)) == a);
    }
}
