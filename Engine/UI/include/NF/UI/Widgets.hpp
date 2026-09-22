#pragma once

// NF/UI/Widgets.hpp — retained-mode runtime game UI widgets (Game-Ready G3).
//
// This is the runtime (shipped-game) half of Engine/UI, distinct from the
// editor's ImGui/Localization path: widgets are RETAINED — a game builds them
// once, mutates values per frame, and asks for a snapshot; nothing here talks
// to ImGui or the editor. Coordinates are NORMALIZED viewport space
// (x, y in [0, 1], origin top-left) so a HUD laid out once survives any
// resolution; snapshot(width, height) bakes them into a pixel display list.
//
// The DisplayList is the render contract: rects + lines + text runs, nothing
// else. A draw backend (GPU overlay, software blit, debug console) consumes
// only that — see COORDINATION.md R-G3-2 for the render-core request.
//
// Text is logical-order UTF-8 straight from ui::tr(); shaping/bidi is the
// backend's job (the editor renders Arabic through shape_arabic(), a runtime
// backend would do the same). No widget ever stores a raw Latin literal that
// should have been a key.

#include <NF/Core/Types.hpp>

#include <string>
#include <vector>

namespace nf::ui {

// --- primitives -------------------------------------------------------------

struct UiColor {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct UiRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

enum class TextAlign : unsigned char {
    Left = 0,
    Center = 1,
    Right = 2,
};

// Display-list primitives (pixels).
struct DlText {
    float x = 0.0f;
    float y = 0.0f;
    std::string text;
    float size = 24.0f;
    UiColor color;
    TextAlign align = TextAlign::Left;
};

struct DlRect {
    UiRect rect; // pixels
    UiColor color;
};

struct DlLine {
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    float width = 1.0f;
    UiColor color;
};

struct DisplayList {
    std::vector<DlRect> rects;
    std::vector<DlLine> lines;
    std::vector<DlText> texts;
};

// --- widgets (retained; games mutate fields directly or via the GameUI layer)

struct Label {
    std::string id;
    UiRect rect;
    std::string text;
    float size = 24.0f;
    UiColor color;
    TextAlign align = TextAlign::Left;
    bool visible = true;
};

struct ProgressBar {
    std::string id;
    UiRect rect;
    float fraction = 1.0f; // clamped to [0, 1]
    UiColor fill{0.85f, 0.15f, 0.15f, 1.0f};
    UiColor background{0.1f, 0.1f, 0.1f, 0.75f};
    std::string label; // drawn centered over the bar (optional)
    float label_size = 16.0f;
    bool visible = true;
};

struct Reticle {
    std::string id;
    float x = 0.5f; // normalized center
    float y = 0.5f;
    float radius = 0.015f; // normalized arm length
    float gap = 0.004f;    // normalized hole in the middle
    float thickness = 2.0f;
    UiColor color{1.0f, 1.0f, 1.0f, 0.9f};
    bool visible = true;
};

struct MenuItem {
    std::string id; // stable handle the game switches on ("new_game", ...)
    std::string label; // already-localized display text
    bool enabled = true;
};

struct MenuList {
    std::string id;
    UiRect rect;
    std::string title;
    float title_size = 48.0f;
    float item_size = 28.0f;
    UiColor color{1.0f, 1.0f, 1.0f, 1.0f};
    UiColor selected_color{1.0f, 0.8f, 0.2f, 1.0f};
    std::vector<MenuItem> items;
    int selected = 0; // index into items (only meaningful when items is set)

    /// Moves the selection by `delta` (wraps), skipping disabled items.
    /// False when nothing is selectable (empty or all disabled).
    bool move(int delta);
    /// Selects the first enabled item (called after set_items).
    void reset_selection();
    const MenuItem* selected_item() const;
    /// The selected item when it is enabled, else nullptr.
    const MenuItem* activate() const;
};

struct Slider {
    std::string id; // doubles as the settings key ("master", "music", ...)
    UiRect rect;
    std::string label;
    float value = 0.5f; // clamped to [0, 1]
    float step = 0.05f;
    bool visible = true;
};

// --- canvas -----------------------------------------------------------------

/// A retained screen of widgets. Games own canvases (HUD canvas, menu
/// canvas, ...) and snapshot the active one per frame.
class Canvas {
public:
    std::vector<Label> labels;
    std::vector<ProgressBar> bars;
    std::vector<Reticle> reticles;
    std::vector<MenuList> menus;
    std::vector<Slider> sliders;

    Label& add_label(Label l);
    ProgressBar& add_bar(ProgressBar b);
    Reticle& add_reticle(Reticle r);
    MenuList& add_menu(MenuList m);
    Slider& add_slider(Slider s);

    Label* find_label(const std::string& id);
    ProgressBar* find_bar(const std::string& id);
    Reticle* find_reticle(const std::string& id);
    MenuList* find_menu(const std::string& id);
    Slider* find_slider(const std::string& id);

    // Const overloads: a snapshot-time getter (Hud::health_fraction, script
    // bindings reading state off a const canvas) must not need a mutable
    // canvas just to look a widget up.
    const Label* find_label(const std::string& id) const;
    const ProgressBar* find_bar(const std::string& id) const;
    const Reticle* find_reticle(const std::string& id) const;
    const MenuList* find_menu(const std::string& id) const;
    const Slider* find_slider(const std::string& id) const;

    /// Bakes visible widgets into a pixel display list for a w*h viewport.
    DisplayList snapshot(float width, float height) const;
};

} // namespace nf::ui
