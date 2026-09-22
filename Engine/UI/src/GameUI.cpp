// NF/UI/GameUI.cpp — runtime game UI facade (Game-Ready G3).

#include <NF/UI/GameUI.hpp>

#include <NF/UI/Localization.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nf::ui {

namespace {

constexpr const char* kMsgId = "hud_message";
constexpr const char* kHealthId = "hud_health";
constexpr const char* kAmmoId = "hud_ammo";
constexpr const char* kReticleId = "hud_reticle";

float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

} // namespace

// --- screen names --------------------------------------------------------------

const char* screen_name(Screen s) {
    switch (s) {
    case Screen::MainMenu: return "main_menu";
    case Screen::Settings: return "settings";
    case Screen::Playing: return "playing";
    case Screen::Paused: return "paused";
    case Screen::GameOver: return "game_over";
    }
    return "unknown";
}

// --- action names ----------------------------------------------------------------

const char* action_name(Action a) {
    switch (a) {
    case Action::None: return "none";
    case Action::Up: return "up";
    case Action::Down: return "down";
    case Action::Left: return "left";
    case Action::Right: return "right";
    case Action::Confirm: return "confirm";
    case Action::Back: return "back";
    }
    return "none";
}

Action action_from_name(const char* name) {
    if (name == nullptr) return Action::None;
    // Case-insensitive compare without allocation (names are ASCII literals).
    auto eq = [](const char* a, const char* b) {
        for (; *a != '\0' && *b != '\0'; ++a, ++b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a - 'A' + 'a') : *a;
            if (ca != *b) return false;
        }
        return *a == '\0' && *b == '\0';
    };
    if (eq(name, "up")) return Action::Up;
    if (eq(name, "down")) return Action::Down;
    if (eq(name, "left")) return Action::Left;
    if (eq(name, "right")) return Action::Right;
    if (eq(name, "confirm")) return Action::Confirm;
    if (eq(name, "back")) return Action::Back;
    return Action::None;
}

// --- SettingsData -----------------------------------------------------------------

bool SettingsData::set_volume(const char* bus, float value) {
    const float v = clamp01(value);
    if (std::strcmp(bus, "master") == 0) master_volume = v;
    else if (std::strcmp(bus, "music") == 0) music_volume = v;
    else if (std::strcmp(bus, "sfx") == 0) sfx_volume = v;
    else return false;
    return true;
}

float SettingsData::volume(const char* bus) const {
    if (std::strcmp(bus, "master") == 0) return master_volume;
    if (std::strcmp(bus, "music") == 0) return music_volume;
    if (std::strcmp(bus, "sfx") == 0) return sfx_volume;
    return -1.0f;
}

// --- Hud ---------------------------------------------------------------------------

Hud::Hud() {
    ProgressBar& hp = m_canvas.add_bar(ProgressBar{kHealthId, UiRect{0.03f, 0.04f, 0.22f, 0.035f}});
    hp.label_size = 16.0f;
    hp.label = tr("health");

    Label& ammo = m_canvas.add_label(Label{kAmmoId, UiRect{0.75f, 0.88f, 0.22f, 0.05f}});
    ammo.size = 26.0f;
    ammo.align = TextAlign::Right;
    refresh_ammo_label();

    m_canvas.add_reticle(Reticle{kReticleId});

    Label& msg = m_canvas.add_label(Label{kMsgId, UiRect{0.15f, 0.75f, 0.7f, 0.05f}});
    msg.size = 24.0f;
    msg.align = TextAlign::Center;
    msg.visible = false;
}

void Hud::set_health(float current, float max) {
    ProgressBar* b = m_canvas.find_bar(kHealthId);
    if (b == nullptr) return;
    b->fraction = (max > 0.0f) ? clamp01(current / max) : 0.0f;
    b->label = tr("health");
}

float Hud::health_fraction() const {
    const ProgressBar* b = m_canvas.find_bar(kHealthId);
    return (b != nullptr) ? b->fraction : 0.0f;
}

void Hud::set_ammo(int magazine, int reserve) {
    m_ammo_magazine = magazine;
    m_ammo_reserve = reserve;
    refresh_ammo_label();
}

void Hud::refresh_ammo_label() {
    Label* l = m_canvas.find_label(kAmmoId);
    if (l == nullptr) return;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d / %d", m_ammo_magazine, m_ammo_reserve);
    // Readout + localised unit. The numbers stay a plain LTR run; shaping is
    // the render backend's job (AV()/shape_arabic on the editor side).
    l->text = std::string(buf) + "  " + tr("ammo");
}

int Hud::ammo_magazine() const { return m_ammo_magazine; }

int Hud::ammo_reserve() const { return m_ammo_reserve; }

void Hud::refresh_labels() {
    if (ProgressBar* b = m_canvas.find_bar(kHealthId)) b->label = tr("health");
    refresh_ammo_label();
}

void Hud::set_reticle_visible(bool visible) {
    Reticle* r = m_canvas.find_reticle(kReticleId);
    if (r != nullptr) r->visible = visible;
}

bool Hud::reticle_visible() const {
    const Reticle* r = m_canvas.find_reticle(kReticleId);
    return (r != nullptr) && r->visible;
}

void Hud::show_message(const std::string& text, float seconds) {
    m_message = text;
    m_message_timer = std::max(0.0f, seconds);
    Label* l = m_canvas.find_label(kMsgId);
    if (l != nullptr) {
        l->text = text;
        l->visible = true;
    }
}

const std::string& Hud::message() const { return m_message; }

void Hud::update(float dt) {
    if (dt <= 0.0f) return;
    if (m_message_timer <= 0.0f) {
        // A zero-second message never displays: hide it on the first tick
        // instead of early-returning with the label stuck visible.
        if (Label* l = m_canvas.find_label(kMsgId)) l->visible = false;
        return;
    }
    m_message_timer -= dt;
    if (m_message_timer <= 0.0f) {
        Label* l = m_canvas.find_label(kMsgId);
        if (l != nullptr) l->visible = false;
    }
}

// --- Menu ----------------------------------------------------------------------------

Menu::Menu(std::string id, std::string title) {
    MenuList list;
    list.id = std::move(id);
    list.rect = UiRect{0.35f, 0.3f, 0.3f, 0.4f};
    list.title = std::move(title);
    m_canvas.add_menu(std::move(list));
}

void Menu::set_title(const std::string& title) {
    MenuList* m = m_canvas.menus.empty() ? nullptr : &m_canvas.menus.front();
    if (m != nullptr) m->title = title;
}

const std::string& Menu::title() const {
    static const std::string kEmpty;
    return m_canvas.menus.empty() ? kEmpty : m_canvas.menus.front().title;
}

void Menu::set_items(std::vector<MenuItem> items) {
    MenuList* m = m_canvas.menus.empty() ? nullptr : &m_canvas.menus.front();
    if (m == nullptr) return;
    m->items = std::move(items);
    m->reset_selection();
}

usize Menu::item_count() const {
    return m_canvas.menus.empty() ? 0 : m_canvas.menus.front().items.size();
}

const MenuItem* Menu::item(usize index) const {
    if (m_canvas.menus.empty()) return nullptr;
    const MenuList& m = m_canvas.menus.front();
    if (index >= m.items.size()) return nullptr;
    return &m.items[index];
}

bool Menu::move(int delta) {
    MenuList* m = m_canvas.menus.empty() ? nullptr : &m_canvas.menus.front();
    return (m != nullptr) && m->move(delta);
}

const MenuItem* Menu::selected() const {
    return m_canvas.menus.empty() ? nullptr : m_canvas.menus.front().selected_item();
}

const MenuItem* Menu::activate() const {
    return m_canvas.menus.empty() ? nullptr : m_canvas.menus.front().activate();
}

// --- DialoguePresenter -----------------------------------------------------------------

void DialoguePresenter::refresh() {
    m_choices.clear();
    if (m_active && m_tags != nullptr) {
        m_choices = m_runner.available_choices(*m_tags);
    }
    m_selected = 0;

    m_canvas.labels.clear();
    m_canvas.menus.clear();
    if (!m_active) return;
    const gameplay::DialogueNode* node = m_runner.current();
    if (node == nullptr) return;

    Label& speaker = m_canvas.add_label(Label{});
    speaker.id = "dialogue_speaker";
    speaker.rect = UiRect{0.2f, 0.66f, 0.6f, 0.04f};
    speaker.text = node->speaker;
    speaker.size = 22.0f;
    speaker.align = TextAlign::Center;
    speaker.color = UiColor{1.0f, 0.85f, 0.4f, 1.0f};

    Label& body = m_canvas.add_label(Label{});
    body.id = "dialogue_text";
    body.rect = UiRect{0.2f, 0.71f, 0.6f, 0.1f};
    body.text = node->text;
    body.size = 20.0f;
    body.align = TextAlign::Left;

    MenuList list;
    list.id = "dialogue_choices";
    list.rect = UiRect{0.22f, 0.82f, 0.56f, 0.14f};
    list.title.clear();
    list.item_size = 20.0f;
    for (const gameplay::DialogueChoice* c : m_choices) {
        list.items.push_back(MenuItem{c->id, c->text, true});
    }
    if (list.items.empty()) {
        // Choiceless nodes advance with the same button; show a hint item.
        list.items.push_back(MenuItem{"__continue", tr("dialogue_continue"), true});
    }
    list.reset_selection();
    m_canvas.add_menu(std::move(list));
}

bool DialoguePresenter::start(const gameplay::DialogueTree& tree, const std::string& start_node,
                              gameplay::TagContainer& tags) {
    m_tags = &tags;
    m_active = m_runner.start(tree, start_node);
    refresh();
    return m_active;
}

void DialoguePresenter::close() {
    m_active = false;
    m_runner = gameplay::DialogueRunner{};
    m_tags = nullptr;
    m_choices.clear();
    m_selected = 0;
    refresh();
}

bool DialoguePresenter::active() const { return m_active; }

const std::string& DialoguePresenter::speaker() const {
    static const std::string kEmpty;
    const Label* l = m_canvas.find_label("dialogue_speaker");
    return (l != nullptr) ? l->text : kEmpty;
}

const std::string& DialoguePresenter::text() const {
    static const std::string kEmpty;
    const Label* l = m_canvas.find_label("dialogue_text");
    return (l != nullptr) ? l->text : kEmpty;
}

usize DialoguePresenter::choice_count() const { return m_choices.size(); }

int DialoguePresenter::selected_choice() const { return m_selected; }

bool DialoguePresenter::move_selection(int delta) {
    if (!m_active || m_choices.empty()) return false;
    const int count = static_cast<int>(m_choices.size());
    m_selected += delta;
    m_selected %= count;
    if (m_selected < 0) m_selected += count;
    MenuList* list = m_canvas.find_menu("dialogue_choices");
    if (list != nullptr) list->selected = m_selected;
    return true;
}

bool DialoguePresenter::confirm() {
    if (!m_active) return false;
    if (m_choices.empty()) {
        m_runner.advance();
    } else {
        const gameplay::DialogueChoice* c = m_choices[static_cast<usize>(m_selected)];
        if (m_tags == nullptr) return false;
        m_runner.choose(c->id, *m_tags);
    }
    m_active = !m_runner.ended();
    refresh();
    return m_active;
}

// --- GameFlow ----------------------------------------------------------------------------

GameFlow::GameFlow() {
    m_main_menu = Menu{"main_menu", tr("main_menu")};
    m_pause_menu = Menu{"pause_menu", tr("paused")};
    m_game_over_menu = Menu{"game_over_menu", tr("game_over")};
    m_settings_menu = Menu{"settings_menu", tr("settings")};
    refresh_labels();
    build_settings_menu();
}

void GameFlow::refresh_labels() {
    m_main_menu.set_title(tr("main_menu"));
    m_main_menu.set_items({
        {"new_game", tr("new_game"), true},
        {"settings", tr("settings"), true},
        {"quit", tr("quit"), true},
    });

    m_pause_menu.set_title(tr("paused"));
    m_pause_menu.set_items({
        {"resume", tr("resume"), true},
        {"settings", tr("settings"), true},
        {"restart", tr("restart"), true},
        {"quit", tr("quit"), true},
    });

    m_game_over_menu.set_title(tr("game_over"));
    m_game_over_menu.set_items({
        {"restart", tr("restart"), true},
        {"quit", tr("quit"), true},
    });

    // Settings: sliders keep their values; only the labels re-resolve.
    const float master = m_settings.master_volume;
    const float music = m_settings.music_volume;
    const float sfx = m_settings.sfx_volume;
    const float sens = m_settings.mouse_sensitivity;
    build_settings_menu();
    Slider* s1 = m_settings_menu.canvas().find_slider("master");
    Slider* s2 = m_settings_menu.canvas().find_slider("music");
    Slider* s3 = m_settings_menu.canvas().find_slider("sfx");
    Slider* s4 = m_settings_menu.canvas().find_slider("sensitivity");
    if (s1 != nullptr) s1->value = master;
    if (s2 != nullptr) s2->value = music;
    if (s3 != nullptr) s3->value = sfx;
    if (s4 != nullptr) s4->value = sens;

    // The HUD's own labels ("health", "ammo") are tr() keys too.
    m_hud.refresh_labels();
}

void GameFlow::build_settings_menu() {
    MenuList* list = m_settings_menu.canvas().find_menu("settings_menu");
    if (list == nullptr) {
        MenuList add;
        add.id = "settings_menu";
        add.rect = UiRect{0.3f, 0.25f, 0.4f, 0.5f};
        add.title = tr("settings");
        m_settings_menu.canvas().add_menu(std::move(add));
        list = m_settings_menu.canvas().find_menu("settings_menu");
    }
    if (list == nullptr) return;
    list->title = tr("settings");

    // One slider per audio/gameplay row + Back. The menu's selected index
    // maps 1:1 onto the canvas slider order for rows 0..3 (row 4 = back).
    const char* ids[] = {"master", "music", "sfx", "sensitivity"};
    const char* keys[] = {"master_volume", "music_volume", "sfx_volume", "sensitivity"};
    auto ensure_slider = [&](const char* id, float y) -> Slider& {
        Slider* found = m_settings_menu.canvas().find_slider(id);
        if (found != nullptr) return *found;
        Slider add;
        add.id = id;
        add.rect = UiRect{0.32f, y, 0.36f, 0.06f};
        return m_settings_menu.canvas().add_slider(std::move(add));
    };
    float y = 0.4f;
    for (int i = 0; i < 4; ++i) {
        Slider& s = ensure_slider(ids[i], y);
        s.label = tr(keys[i]);
        y += 0.1f;
    }

    list->items.clear();
    for (int i = 0; i < 4; ++i) {
        list->items.push_back({ids[i], tr(keys[i]), true});
    }
    list->items.push_back({"back", tr("back"), true});
    list->reset_selection();
}

Screen GameFlow::screen() const { return m_screen; }

void GameFlow::notify_game_over() {
    m_screen = Screen::GameOver;
    if (MenuList* list = m_game_over_menu.canvas().find_menu("game_over_menu")) {
        list->reset_selection();
    }
}

void GameFlow::apply_slider(const Slider& s) {
    const float v = clamp01(s.value);
    if (s.id == "master" || s.id == "music" || s.id == "sfx") {
        (void)m_settings.set_volume(s.id.c_str(), v);
        if (on_settings_changed) on_settings_changed();
    } else if (s.id == "sensitivity") {
        m_settings.mouse_sensitivity = v;
    }
}

void GameFlow::handle(Action action) {
    if (action == Action::None) return;

    // Dialogue captures input while open (over gameplay).
    if (m_screen == Screen::Playing && m_dialogue.active()) {
        switch (action) {
        case Action::Up: m_dialogue.move_selection(-1); return;
        case Action::Down: m_dialogue.move_selection(1); return;
        case Action::Confirm: m_dialogue.confirm(); return;
        case Action::Back: m_dialogue.close(); return;
        default: return;
        }
    }

    switch (m_screen) {
    case Screen::MainMenu: {
        switch (action) {
        case Action::Up: (void)m_main_menu.move(-1); return;
        case Action::Down: (void)m_main_menu.move(1); return;
        case Action::Confirm: {
            const MenuItem* it = m_main_menu.activate();
            if (it == nullptr) return;
            if (it->id == "new_game") {
                m_screen = Screen::Playing;
                if (on_start_game) on_start_game();
            } else if (it->id == "settings") {
                m_settings_return = Screen::MainMenu;
                m_screen = Screen::Settings;
            } else if (it->id == "quit") {
                if (on_quit) on_quit();
            }
            return;
        }
        default: return;
        }
    }
    case Screen::Settings: {
        // Menu rows 0..3 map 1:1 onto the canvas slider order; row 4 = back.
        MenuList* list = m_settings_menu.canvas().find_menu("settings_menu");
        const int sel = (list != nullptr) ? list->selected : -1;
        const bool is_back = (sel == static_cast<int>(m_settings_menu.canvas().sliders.size()));
        Slider* slider = (sel >= 0 && sel < static_cast<int>(m_settings_menu.canvas().sliders.size()))
                             ? &m_settings_menu.canvas().sliders[static_cast<usize>(sel)]
                             : nullptr;
        switch (action) {
        case Action::Up: (void)m_settings_menu.move(-1); return;
        case Action::Down: (void)m_settings_menu.move(1); return;
        case Action::Left:
            // Moving a slider applies it immediately, so on_settings_changed
            // fires live (G5's audio buses follow the slider, not the button).
            if (slider != nullptr) {
                slider->value = clamp01(slider->value - slider->step);
                apply_slider(*slider);
            }
            return;
        case Action::Right:
            if (slider != nullptr) {
                slider->value = clamp01(slider->value + slider->step);
                apply_slider(*slider);
            }
            return;
        case Action::Confirm:
            // Confirm applies the focused row; on the "back" row it leaves
            // settings, so a pad/menu-only player is never stuck here.
            if (is_back) {
                m_screen = m_settings_return;
            } else if (slider != nullptr) {
                apply_slider(*slider);
            }
            return;
        case Action::Back:
            m_screen = m_settings_return;
            return;
        default: return;
        }
    }
    case Screen::Playing: {
        // Only dialogue and Back matter in gameplay.
        if (action == Action::Back) m_screen = Screen::Paused;
        return;
    }
    case Screen::Paused: {
        switch (action) {
        case Action::Up: (void)m_pause_menu.move(-1); return;
        case Action::Down: (void)m_pause_menu.move(1); return;
        case Action::Back: // Esc on the pause menu resumes, like most games
            m_screen = Screen::Playing;
            if (on_resume) on_resume();
            return;
        case Action::Confirm: {
            const MenuItem* it = m_pause_menu.activate();
            if (it == nullptr) return;
            if (it->id == "resume") {
                m_screen = Screen::Playing;
                if (on_resume) on_resume();
            } else if (it->id == "settings") {
                m_settings_return = Screen::Paused;
                m_screen = Screen::Settings;
            } else if (it->id == "restart") {
                m_screen = Screen::Playing;
                if (on_restart) on_restart();
            } else if (it->id == "quit") {
                if (on_quit) on_quit();
            }
            return;
        }
        default: return;
        }
    }
    case Screen::GameOver: {
        switch (action) {
        case Action::Up: (void)m_game_over_menu.move(-1); return;
        case Action::Down: (void)m_game_over_menu.move(1); return;
        case Action::Confirm: {
            const MenuItem* it = m_game_over_menu.activate();
            if (it == nullptr) return;
            if (it->id == "restart") {
                m_screen = Screen::Playing;
                if (on_restart) on_restart();
            } else if (it->id == "quit") {
                if (on_quit) on_quit();
            }
            return;
        }
        default: return;
        }
    }
    }
}

Hud& GameFlow::hud() { return m_hud; }
SettingsData& GameFlow::settings() { return m_settings; }
Menu& GameFlow::main_menu() { return m_main_menu; }
Menu& GameFlow::pause_menu() { return m_pause_menu; }
Menu& GameFlow::game_over_menu() { return m_game_over_menu; }
Menu& GameFlow::settings_menu() { return m_settings_menu; }
DialoguePresenter& GameFlow::dialogue() { return m_dialogue; }

Canvas* GameFlow::active_canvas() {
    switch (m_screen) {
    case Screen::MainMenu: return &m_main_menu.canvas();
    case Screen::Settings: return &m_settings_menu.canvas();
    case Screen::Paused: return &m_pause_menu.canvas();
    case Screen::GameOver: return &m_game_over_menu.canvas();
    case Screen::Playing: return &m_hud.canvas();
    }
    return nullptr;
}

} // namespace nf::ui
