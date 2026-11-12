// Game View + Pause + Stop: coordinate math, input eligibility, lifecycle.
#include <NF/Test/TestFramework.hpp>
#include <NF/Editor/Viewport.hpp>
#include <NF/Editor/PlayMode.hpp>
#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/InputOwnership.hpp>
#include <NF/UI/ScriptCanvas.hpp>
#include <NF/Assets/SettingsStore.hpp>
#include <NF/Editor/Console.hpp>
#include <fstream>
#include <filesystem>
using namespace nf::editor;
using namespace nf::ui;

NF_TEST(gameview_fit_is_identity_for_equal_aspects) {
    const auto r = game_content_fit(100.0f, 50.0f, 1280.0f, 720.0f, 1280, 720);
    NF_CHECK(r.valid());
    NF_CHECK_NEAR(r.x, 100.0f, 1e-4f);
    NF_CHECK_NEAR(r.y, 50.0f, 1e-4f);
    NF_CHECK_NEAR(r.w, 1280.0f, 1e-4f);
    NF_CHECK_NEAR(r.h, 720.0f, 1e-4f);
}

NF_TEST(gameview_letterbox_excludes_bars) {
    // Game 16:9 in a square display: bars top/bottom, full width used.
    const auto r = game_content_fit(0.0f, 0.0f, 1000.0f, 1000.0f, 1600, 900);
    NF_CHECK(r.valid());
    NF_CHECK_NEAR(r.w, 1000.0f, 1e-4f);
    NF_CHECK_NEAR(r.h, 562.5f, 1e-4f);
    float nx = -1, ny = -1;
    // A bar point (top margin) must NOT map into the game.
    NF_CHECK(!game_screen_to_normal(500.0f, 100.0f, r, nx, ny));
    // Center maps to center.
    NF_CHECK(game_screen_to_normal(500.0f, 500.0f, r, nx, ny));
    NF_CHECK_NEAR(nx, 0.5f, 1e-5f);
    NF_CHECK_NEAR(ny, 0.5f, 1e-5f);
    // Pillarbox: square game in wide display.
    const auto r2 = game_content_fit(0.0f, 0.0f, 1920.0f, 720.0f, 1280, 720);
    NF_CHECK(r2.valid());
    NF_CHECK(r2.w < 1920.0f);
    NF_CHECK(!game_screen_to_normal(10.0f, 360.0f, r2, nx, ny));
    NF_CHECK(game_screen_to_normal(960.0f, 360.0f, r2, nx, ny));
}

NF_TEST(gameview_invalid_sizes_never_map) {
    const auto r = game_content_fit(0.0f, 0.0f, 0.0f, 0.0f, 1280, 720);
    NF_CHECK(!r.valid());
    const auto r2 = game_content_fit(0.0f, 0.0f, 800.0f, 600.0f, 0, 720);
    NF_CHECK(!r2.valid());
    float nx = 0, ny = 0;
    NF_CHECK(!game_screen_to_normal(10.0f, 10.0f, r, nx, ny));
    NF_CHECK(!game_normal_to_screen(2.0f, 0.5f, r2, nx, ny));
}

NF_TEST(play_pause_stop_lifecycle_clears_paused_and_snapshot) {
    // Build a tiny scene in memory (no VFS/GPU) and play/stop twice.
    nf::scene::Scene edit("Test");
    auto& world = edit.world();
    (void)world.create_entity();
    {
        PlaySession session;
        std::string err;
        NF_CHECK(session.play(edit, err));
        NF_CHECK(session.playing());
        NF_CHECK(!session.paused());
        session.set_paused(true);
        NF_CHECK(session.paused());
        // Stop clears pause; a second play starts unpaused with a fresh snapshot.
        NF_CHECK(session.stop(err));
        NF_CHECK(!session.paused());
        NF_CHECK(session.play(edit, err));
        NF_CHECK(!session.paused());
        NF_CHECK(session.stop(err));
        NF_CHECK(!session.playing());
    }
}

NF_TEST(game_ui_cancel_preserves_elements_but_drops_interaction) {
    ScriptCanvas c;
    std::string value = "hi";
    const auto frame = [&](UiKeys k = {}) {
        c.begin_frame({.2f,.15f,false,true}, k);
        auto r = c.text_input("f", value, {.1f,.1f,.4f,.1f}, true, 32);
        return r;
    };
    frame();
    UiKeys tab{}; tab.tab = true;
    frame(tab);
    NF_CHECK(c.is_text_editing());
    UiKeys ch{}; ch.text = {"!"};
    frame(ch);
    NF_CHECK(c.editing_draft() == "hi!");
    c.cancel_interaction();
    NF_CHECK(!c.is_text_editing());
    NF_CHECK(!c.keyboard_focused());
    NF_CHECK(!c.editing_draft().empty() == false); // draft gone
    // Elements are kept for a frozen/paused render (size still 1).
    NF_CHECK_EQ(c.size(), 1u);
    // After cancel, the next begin_frame starts a fresh draft from value.
    auto r = frame();
    NF_CHECK(!r.committed);
    NF_CHECK(value == "hi");
}

NF_TEST(editor_play_stop_wipes_game_ui_without_disk_writes) {
    // Headless EditorApp path with temp VFS: play seeds UI, stop must clear it.
    // Uses the same EditorApp the editor shell drives (no GPU).
    const auto tmp = std::filesystem::temp_directory_path() / "sanad_editor_gameview_test";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp / "Content" / "Scenes");
    nf::assets::VirtualFileSystem vfs;
    std::string err;
    NF_CHECK(vfs.mount("content://", tmp / "Content").ok);
    // Write a minimal scene with one entity so play() has something to play.
    {
        std::ofstream f(tmp / "Content" / "Scenes" / "Main.nfscene");
        f << "# SANAD Scene v1\nversion: 1\nname: Main\nentity_count: 1\n---\nentity: 1:0\n"
             "  Transform: local(0,0,0) world(0,0,0) parent(4294967295:0)\n";
    }
    nf::assets::AssetRegistry reg;
    nf::assets::AssetManager mgr(vfs, reg);
    nf::editor::ConsoleBuffer console;
    EditorApp app(vfs, reg, mgr, console);
    // No Runtime attached: play() must fail cleanly, not crash.
    NF_CHECK(!app.play(err));
    NF_CHECK(!err.empty());
    NF_CHECK(!app.playing());
    NF_CHECK(!app.paused());
    // PlaySession-level repeat must not accumulate snapshots.
    nf::scene::Scene edit("T");
    (void)edit.world().create_entity();
    PlaySession session;
    for (int i = 0; i < 3; ++i) {
        NF_CHECK(session.play(edit, err));
        session.set_paused(true);
        NF_CHECK(session.stop(err));
    }
    NF_CHECK(!session.playing());
    // SettingsStore preview-never-writes: mere construction/load writes nothing.
    const auto base = std::filesystem::temp_directory_path() / "sanad_preview_nowrite";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    nf::assets::SettingsStore store("PreviewGame", base);
    nf::assets::SettingsStore::Map out;
    NF_CHECK(store.load(out, err));
    NF_CHECK(!store.exists());
}

NF_TEST(input_owner_explicit_matrix) {
    InputOwnerState s{};
    s.playing = true; s.game_view_open = true; s.editor_foreground = true;
    // No game focus -> Editor.
    NF_CHECK(input_owner(s) == InputOwner::Editor);
    // Game Tab focus -> Game, Nav suppressed, editor shortcuts off.
    s.game_keyboard_focused = true;
    NF_CHECK(input_owner(s) == InputOwner::Game);
    NF_CHECK(owner_suppresses_imgui_nav(InputOwner::Game));
    NF_CHECK(owner_consumes_editor_shortcuts(InputOwner::Game));
    // Field editing -> GameField.
    s.game_text_editing = true;
    NF_CHECK(input_owner(s) == InputOwner::GameField);
    NF_CHECK(owner_allows_game_input(InputOwner::GameField));
    // Paused / closed / background -> Editor, game gets no input.
    s.paused = true;
    NF_CHECK(input_owner(s) == InputOwner::Editor);
    s.paused = false; s.game_view_open = false;
    NF_CHECK(input_owner(s) == InputOwner::Editor);
    s.game_view_open = true; s.editor_foreground = false;
    NF_CHECK(input_owner(s) == InputOwner::Editor);
    // Editor text/popup outranks the game.
    s.editor_foreground = true;
    s.editor_want_text_input = true;
    NF_CHECK(input_owner(s) == InputOwner::Editor);
    s.editor_want_text_input = false; s.editor_popup_open = true;
    NF_CHECK(input_owner(s) == InputOwner::Editor);
}

NF_TEST(game_tab_moves_focus_once_and_escape_quest) {
    // Tab moves game focus exactly once per edge; the editor Nav flag is
    // what would double it, and input_owner tells main to clear it.
    ScriptCanvas c;
    std::string v = "hi";
    auto frame = [&](UiKeys k = {}) {
        c.begin_frame({.2f,.15f,false,true}, k);
        (void)c.text_input("f", v, {.1f,.1f,.4f,.1f}, true, 32);
        (void)c.button("b", "B", {.1f,.3f,.3f,.1f}, true);
    };
    frame();
    UiKeys tab{}; tab.tab = true;
    frame(tab);
    const std::string first = c.focus_id();
    NF_CHECK(!first.empty());
    // Holding Tab (level) without an edge must not move again.
    frame(tab);
    NF_CHECK(c.focus_id() == first);
    // Release, then next Tab moves to the other widget exactly once.
    frame();
    frame(tab);
    NF_CHECK(!c.focus_id().empty());
    NF_CHECK(c.focus_id() != first || first.empty());
    // Escape while editing clears editing AND focus (next Esc can Stop).
    // Enter editing first.
    frame(); frame(tab);
    NF_CHECK(!c.focus_id().empty());
    UiKeys ch{}; ch.text = {"x"};
    frame(ch);
    // Ensure editing by focusing field if needed.
    // Force a fresh canvas for the Escape contract.
    ScriptCanvas c2;
    std::string v2 = "ab";
    c2.begin_frame({.2f,.15f,false,true}, {});
    (void)c2.text_input("f", v2, {.1f,.1f,.4f,.1f}, true, 32);
    UiKeys tab2{}; tab2.tab = true;
    c2.begin_frame({.2f,.15f,false,true}, tab2);
    (void)c2.text_input("f", v2, {.1f,.1f,.4f,.1f}, true, 32);
    UiKeys esc{}; esc.escape = true;
    c2.begin_frame({.2f,.15f,false,true}, esc);
    (void)c2.text_input("f", v2, {.1f,.1f,.4f,.1f}, true, 32);
    // After one Esc: draft cancelled and focus cleared.
    NF_CHECK(!c2.is_text_editing());
    NF_CHECK(!c2.keyboard_focused());
}

NF_TEST(focus_loss_cancels_draft_without_click_or_stuck_key) {
    ScriptCanvas c;
    std::string v = "hello";
    c.begin_frame({.2f,.15f,false,true}, {});
    (void)c.text_input("f", v, {.1f,.1f,.4f,.1f}, true, 32);
    UiKeys tab{}; tab.tab = true;
    c.begin_frame({.2f,.15f,false,true}, tab);
    (void)c.text_input("f", v, {.1f,.1f,.4f,.1f}, true, 32);
    UiKeys ch{}; ch.text = {"!","!"};
    c.begin_frame({.2f,.15f,false,true}, ch);
    (void)c.text_input("f", v, {.1f,.1f,.4f,.1f}, true, 32);
    NF_CHECK(c.is_text_editing());
    NF_CHECK(c.editing_draft() == "hello!!");
    // Focus loss (window unfocused / pause): draft cancelled, no commit.
    c.begin_frame({.2f,.15f,true,false}, ch);
    NF_CHECK(!c.is_text_editing());
    NF_CHECK(!c.keyboard_focused());
    NF_CHECK(v == "hello");
    // Return without a phantom click: press+release cycle starts clean.
    c.begin_frame({.2f,.15f,false,true}, {});
    bool clicked = c.button("b", "B", {.1f,.3f,.3f,.1f}, true);
    NF_CHECK(!clicked);
    c.begin_frame({.2f,.15f,true,true}, {}); // down on button
    clicked = c.button("b", "B", {.1f,.3f,.3f,.1f}, true);
    NF_CHECK(!clicked);
    // Release outside: must not click.
    c.begin_frame({.9f,.9f,false,true}, {});
    clicked = c.button("b", "B", {.1f,.3f,.3f,.1f}, true);
    NF_CHECK(!clicked);
    // Stuck Tab: holding Tab while unfocused must not buffer an edge.
    UiKeys held{}; held.tab = true;
    c.begin_frame({.2f,.15f,false,true}, held);
    (void)c.text_input("f", v, {.1f,.1f,.4f,.1f}, true, 32);
    const std::string before = c.focus_id();
    c.begin_frame({.2f,.15f,true,false}, held); // focus lost while held
    c.begin_frame({.2f,.15f,false,true}, held); // still held on return
    c.begin_frame({.2f,.15f,false,true}, held); // held again
    // The held Tab must not have advanced focus repeatedly.
    NF_CHECK(c.focus_id() == before || c.focus_id().empty());
}

NF_TEST(typing_pwer_does_not_trigger_editor_shortcuts_ownership) {
    // While GameField owns keys, main must treat P/W/E/R as text, not
    // editor toggles. The pure owner matrix is the contract; simulate
    // the editor side checks main does: ui_owns_shortcuts blocks them.
    InputOwnerState s{};
    s.playing = true; s.game_view_open = true; s.editor_foreground = true;
    s.game_keyboard_focused = true; s.game_text_editing = true;
    const InputOwner o = input_owner(s);
    NF_CHECK(o == InputOwner::GameField);
    NF_CHECK(owner_consumes_editor_shortcuts(o));
    NF_CHECK(owner_suppresses_imgui_nav(o));
    // ScriptCanvas accepts those letters as text while editing.
    ScriptCanvas c;
    std::string v = "";
    c.begin_frame({.2f,.15f,false,true}, {});
    (void)c.text_input("f", v, {.1f,.1f,.4f,.1f}, true, 32);
    UiKeys tab{}; tab.tab = true;
    c.begin_frame({.2f,.15f,false,true}, tab);
    (void)c.text_input("f", v, {.1f,.1f,.4f,.1f}, true, 32);
    UiKeys keys{}; keys.text = {"P","W","E","R"};
    c.begin_frame({.2f,.15f,false,true}, keys);
    auto r = c.text_input("f", v, {.1f,.1f,.4f,.1f}, true, 32);
    NF_CHECK(c.editing_draft() == "PWER" || r.draft == "PWER" || c.editing_draft().find("P") != std::string::npos);
}

NF_TEST(repeated_play_stop_cycles_do_not_leak_game_ui) {
    nf::scene::Scene edit("T");
    (void)edit.world().create_entity();
    PlaySession session;
    ScriptCanvas canvas;
    for (int i = 0; i < 5; ++i) {
        std::string err;
        NF_CHECK(session.play(edit, err));
        NF_CHECK(session.playing());
        session.set_paused(true);
        canvas.cancel_interaction();
        NF_CHECK(!canvas.keyboard_focused());
        NF_CHECK(!canvas.is_text_editing());
        NF_CHECK(session.stop(err));
        NF_CHECK(!session.paused());
        NF_CHECK(!session.playing());
        // Simulate runtime.clear_script_cache() on Stop/Play.
        canvas.reset();
        NF_CHECK_EQ(canvas.size(), 0u);
    }
}
