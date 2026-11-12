#pragma once

// NF/Editor/InputOwnership.hpp — who owns the keyboard/mouse this frame.
//
// Explicit owner: Editor, Game, or GameField (a text/number field inside
// the game). The rules (pure, testable — no HWND/ImGui here):
//
// - Edit mode, headless, closed Game View, paused, or the editor window
//   unfocused while playing  -> Editor (game gets no pointer/keys).
// - Playing + foreground + Game View open + game UI keyboard-focused
//   (Tab focus) but no field editing -> Game (Tab/Enter/arrows/Space
//   belong to ScriptCanvas once; ImGui Nav must not move again).
// - Playing + foreground + Game View open + text/number editing
//   -> GameField (every pad/key/mouse action that could drive gameplay
//   is blocked; P/W/E/R type letters, Escape cancels the draft first).
// - A popup or editor text field (ImGui WantTextInput) always wins over
//   Game/GameField: the game cannot steal keys while the editor is
//   typing in its own field.
//
// main.cpp uses this to (a) gate editor shortcuts, (b) suspend ImGui
// Nav for the frame, and (c) decide pointer/key eligibility. Panels
// shows the same owner in the Game View banner so the user sees who
// the keys belong to.

namespace nf::editor {

enum class InputOwner : int { Editor = 0, Game = 1, GameField = 2 };

inline const char* input_owner_name(InputOwner o) {
    switch (o) {
    case InputOwner::Editor: return "Editor";
    case InputOwner::Game: return "Game";
    case InputOwner::GameField: return "GameField";
    }
    return "Editor";
}

struct InputOwnerState {
    bool headless = false;
    bool automation = false;
    bool playing = false;
    bool paused = false;
    bool game_view_open = false;
    bool game_keyboard_focused = false; // ScriptCanvas::keyboard_focused()
    bool game_text_editing = false;     // ScriptCanvas::is_text_editing()
    bool editor_foreground = true;
    bool editor_want_text_input = false; // ImGui::GetIO().WantTextInput
    bool editor_popup_open = false;
};

inline InputOwner input_owner(const InputOwnerState& s) {
    if (s.headless || s.automation) {
        return InputOwner::Editor;
    }
    // Editor's own text/popups outrank the game.
    if (s.editor_want_text_input || s.editor_popup_open) {
        return InputOwner::Editor;
    }
    if (!s.playing || s.paused || !s.game_view_open || !s.editor_foreground) {
        return InputOwner::Editor;
    }
    if (s.game_text_editing) {
        return InputOwner::GameField;
    }
    if (s.game_keyboard_focused) {
        return InputOwner::Game;
    }
    return InputOwner::Editor;
}

inline bool owner_consumes_editor_shortcuts(InputOwner o) {
    return o != InputOwner::Editor;
}

inline bool owner_allows_game_input(InputOwner o) {
    return o == InputOwner::Game || o == InputOwner::GameField;
}

inline bool owner_suppresses_imgui_nav(InputOwner o) {
    return o != InputOwner::Editor;
}

} // namespace nf::editor
