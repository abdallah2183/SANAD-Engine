#pragma once

// NF/UI/GameUI.hpp — the runtime game UI facade (Game-Ready G3).
//
// One header a game includes to get the whole shipped-game UI spine:
//
//   GameFlow flow;              // main → play → pause → game-over (+ settings)
//   flow.on_start_game = [&]{ ... };
//   flow.handle(ui::Action::Confirm);   // from input, Lua, or C#
//
// Layers, bottom to top:
//   1. Widgets/Canvas (Widgets.hpp) — retained widgets + display list.
//   2. Hud — health bar, ammo readout, reticle, timed message line.
//   3. Menu — title + selectable items; DialoguePresenter — dialogue box
//      over gameplay::DialogueRunner (data layer, untouched).
//   4. GameFlow — the screen state machine; menus are its views.
//
// Script drives: nf.ui.* (Lua, install_ui_bindings) and UiHostApi (C#,
// NFUiSandbox.DriveUi) call the SAME GameFlow methods the C++ sample uses,
// so there is exactly one behaviour to test.
//
// Localization: every player-visible string in the default menus comes from
// ui::tr() keys (Arabic included — the red line). Games that build their own
// menus pass their own tr() text into MenuItem::label.
//
// Settings: SettingsData's three volume floats are the STABLE audio-bind API
// for G5 — buses attach to these values, nothing else (documented in
// COORDINATION.md R-G3-3).

#include <NF/Core/Types.hpp>
#include <NF/Gameplay/Dialogue.hpp>
#include <NF/UI/Widgets.hpp>

#include <functional>
#include <string>

namespace nf::ui {

// --- screens and input --------------------------------------------------------

enum class Screen : unsigned char {
    MainMenu = 0,
    Settings = 1,
    Playing = 2,
    Paused = 3,
    GameOver = 4,
};

/// Screen names usable from scripts and logs ("main_menu", "playing", ...).
const char* screen_name(Screen s);

/// Abstract menu/pad-style action. Input backends map keys/buttons onto these;
/// the flow never sees raw keycodes.
enum class Action : unsigned char {
    None = 0,
    Up = 1,
    Down = 2,
    Left = 3,
    Right = 4,
    Confirm = 5,
    Back = 6,
};

/// Stable script/name vocabulary for Action: "none", "up", "down", "left",
/// "right", "confirm", "back". action_from_name is case-insensitive and
/// answers Action::None for anything unknown (never throws) — the Lua nf.ui
/// binding and the C# host API both route through these, so the two backends
/// cannot drift.
const char* action_name(Action a);
Action action_from_name(const char* name);

/// Settings the game exposes to the player. The three volume floats are the
/// G5 audio-bus bind points (master/music/sfx); sensitivity is gameplay-side.
struct SettingsData {
    float master_volume = 1.0f;
    float music_volume = 1.0f;
    float sfx_volume = 1.0f;
    float mouse_sensitivity = 1.0f;

    /// bus is "master", "music" or "sfx"; value is clamped to [0, 1].
    /// False for an unknown bus.
    bool set_volume(const char* bus, float value);
    /// bus is "master", "music" or "sfx"; -1.0f for an unknown bus.
    float volume(const char* bus) const;
};

// --- HUD ------------------------------------------------------------------------

/// Retained HUD: health bar, ammo readout, reticle, one timed message line.
/// Health and ammo are stored as real state and the widgets are rendered from
/// it (never parsed back out of display text, which would break the moment the
/// label is Arabic).
class Hud {
public:
    Hud(); // builds the default layout

    /// fraction = current / max, clamped to [0, 1] (max <= 0 => 0).
    void set_health(float current, float max);
    float health_fraction() const;
    void set_ammo(int magazine, int reserve);
    int ammo_magazine() const;
    int ammo_reserve() const;
    void set_reticle_visible(bool visible);
    bool reticle_visible() const;
    /// Shows `text` for `seconds`, then the line hides itself in update(dt).
    void show_message(const std::string& text, float seconds = 3.0f);
    const std::string& message() const;
    /// Ticks the message timer. dt <= 0 is ignored.
    void update(float dt);

    /// Re-resolves the HUD's own tr() labels ("health", "ammo") after
    /// set_language; values and visibility are untouched.
    void refresh_labels();

    Canvas& canvas() { return m_canvas; }
    const Canvas& canvas() const { return m_canvas; }

private:
    void refresh_ammo_label();

    Canvas m_canvas;
    std::string m_message;
    float m_message_timer = 0.0f;
    int m_ammo_magazine = 0;
    int m_ammo_reserve = 0;
};

// --- menus ------------------------------------------------------------------------

/// One screen of selectable items. Labels arrive already localized.
class Menu {
public:
    Menu() = default;
    Menu(std::string id, std::string title);

    void set_title(const std::string& title);
    const std::string& title() const;
    void set_items(std::vector<MenuItem> items); // resets selection
    usize item_count() const;
    const MenuItem* item(usize index) const;
    bool move(int delta); // forwards to the canvas MenuList
    const MenuItem* selected() const;
    const MenuItem* activate() const; // nullptr when nothing enabled

    Canvas& canvas() { return m_canvas; }
    const Canvas& canvas() const { return m_canvas; }

private:
    Canvas m_canvas;
};

/// Dialogue presentation over the gameplay data layer. Owns a runner; the
/// tree and tag container stay game-owned. Tag-gated choices the player does
/// not hold are hidden, and picking one grants its sets_tag — exactly the
/// DialogueRunner contract, surfaced as a widget.
class DialoguePresenter {
public:
    /// tree + tags must outlive the presenter's active period.
    /// False (and stays closed) when the start node is missing.
    bool start(const gameplay::DialogueTree& tree, const std::string& start_node,
               gameplay::TagContainer& tags);
    void close();
    bool active() const;

    const std::string& speaker() const;
    const std::string& text() const;
    usize choice_count() const;
    int selected_choice() const;
    bool move_selection(int delta);
    /// Confirms: picks the selected choice, or advances a choiceless node.
    /// True while the dialogue remains open afterwards.
    bool confirm();

    Canvas& canvas() { return m_canvas; }
    const Canvas& canvas() const { return m_canvas; }

private:
    void refresh();

    Canvas m_canvas;
    gameplay::DialogueRunner m_runner;
    gameplay::TagContainer* m_tags = nullptr;
    std::vector<const gameplay::DialogueChoice*> m_choices;
    int m_selected = 0;
    bool m_active = false;
};

// --- flow ------------------------------------------------------------------------

using FlowCallback = std::function<void()>;

/// The screen state machine + every screen's widgets. This is the object the
/// sample, the Lua bindings and the C# host API all drive.
class GameFlow {
public:
    GameFlow(); // builds default menus from ui::tr() keys

    Screen screen() const;

    /// The single entry point for input (from C++, Lua nf.ui.action, or the
    /// C# UiHostApi). Drives menus, settings sliders and dialogue.
    void handle(Action action);

    /// The game calls this when the run ends (health 0, timer out, ...).
    void notify_game_over();

    Hud& hud();
    SettingsData& settings();
    Menu& main_menu();
    Menu& pause_menu();
    Menu& game_over_menu();
    Menu& settings_menu();
    DialoguePresenter& dialogue();

    /// The canvas to draw this frame: the active menu's canvas on menu
    /// screens, the HUD canvas while playing (nullptr only in an impossible
    /// screen state). Games overlay flow.dialogue().canvas() when active().
    Canvas* active_canvas();

    /// Re-resolves every default-menu string from ui::tr() (call after
    /// set_language). Custom labels the game set itself are left alone
    /// (rebuilding replaces them — that is what this does by definition).
    void refresh_labels();

    // Game-owned callbacks. Assign before handling input.
    FlowCallback on_start_game; // Main menu -> "new_game"
    FlowCallback on_restart;    // Pause/GameOver -> "restart"
    FlowCallback on_resume;     // Pause -> "resume"
    FlowCallback on_quit;       // "quit" on any menu
    FlowCallback on_settings_changed; // any slider moved (G5 hooks here)

private:
    void build_settings_menu();
    void apply_slider(const Slider& s);

    Screen m_screen = Screen::MainMenu;
    Screen m_settings_return = Screen::MainMenu;
    Hud m_hud;
    SettingsData m_settings;
    Menu m_main_menu;
    Menu m_pause_menu;
    Menu m_game_over_menu;
    Menu m_settings_menu;
    DialoguePresenter m_dialogue;
};

} // namespace nf::ui
