#pragma once
#include <NF/UI/TextLayout.hpp>
#include <NF/UI/Widgets.hpp>
#include <unordered_map>

namespace nf::ui {
struct UiPointer {
    float x = -1, y = -1; // normalized client coordinates
    bool down = false, focused = false;
};
/// Level key state for the current frame. `focused` mirrors the window focus
/// (UiPointer::focused); keys are level-triggered (down), ScriptCanvas
/// edge-detects Tab/Enter/Space/arrows. `text` carries WM_CHAR codepoints
/// (one UTF-8 string per codepoint, surrogates already combined); control
/// keys backspace/delete/home/end/escape are level-triggered like the rest.
struct UiKeys {
    bool focused = true;
    bool tab = false, shift = false;
    bool enter = false, space = false;
    bool left = false, right = false, up = false, down = false;
    bool backspace = false, delete_key = false, home = false, end = false, escape = false;
    float wheel = 0; // notches: + up, - down (one notch ~ 1 row for a list)
    std::vector<std::string> text; // UTF-8 codepoints from platform char events
};
enum class UiAnchor : unsigned char { TopLeft, TopRight, BottomLeft, BottomRight, TopCenter, BottomCenter, LeftCenter, RightCenter, Center };

struct TextInputResult {
    std::string value;    // committed value on commit, else the caller's value (draft is internal)
    bool committed = false; // Enter confirmed a draft
    bool cancelled = false; // Escape discarded the draft
    bool editing = false;   // this frame the field had keyboard focus
    std::string draft;      // current draft (shown); equals value when not editing
};

struct NumberInputResult {
    double value = 0;       // last valid value (never clobbered by invalid edits)
    bool committed = false; // Enter confirmed
    bool cancelled = false; // Escape discarded the draft
    bool editing = false;
    bool valid = true;      // draft parses and is in range (interim "-" / "" is invalid)
    std::string draft;      // current draft text
    std::string error;      // "" when valid; otherwise a short status (shown under the field)
};

// Immediate script UI, rebuilt each simulation frame. IDs are scoped by script
// entity, while pointer capture survives frames and is cancelled on scene load.
//
// Text editing scope (honest limits — see Docs/Runtime_UI.md):
// - One line, no multi-line paste wrapping, no drag/shift selection, no undo.
// - IME: only committed WM_CHAR characters are inserted; composition-window
//   positioning and candidate selection are not provided.
// - Cursor moves by codepoint (Home/End/arrows); shaping is ArabicShaper
//   (simplified bidi). Hebrew is not shaped; full Unicode BiDi is not claimed.
class ScriptCanvas {
public:
    static constexpr size_t kMaxElements = 512;
    void begin_frame(UiPointer pointer = {}, UiKeys keys = {});
    void reset();
    void set_scope(uint32_t id, uint32_t generation);
    void cursor(bool visible) { m_cursor = visible; }
    bool wants_pointer(UiPointer pointer) const;
    void set_viewport(float width, float height);
    /// Font measuring for tooltips/caret overflow. GameUiOverlay supplies ImFont
    /// measurement; headless/tests may inject a fake or leave the fallback.
    void set_text_measurer(TextMeasurer measurer) { m_measurer = std::move(measurer); }
    const TextMeasurer& text_measurer() const { return m_measurer; }
    UiRect anchor_rect(UiAnchor anchor, float width, float height, float margin = 0.02f,
                       float min_width_px = 0, float min_height_px = 0) const;
    void panel(std::string id, UiRect rect);
    void label(std::string id, std::string text, UiRect rect, float size = 22, TextAlign align = TextAlign::Left);
    bool button(std::string id, std::string text, UiRect rect, bool enabled = true, TextAlign align = TextAlign::Center);
    void bar(std::string id, std::string text, double fraction, UiRect rect);
    double slider(std::string id, std::string text, double value, double minimum, double maximum, UiRect rect);
    bool checkbox(std::string id, std::string text, bool checked, UiRect rect, bool enabled = true);
    /// `items` are labels; `selected` is 0-based (Lua passes 1-based). Returns
    /// the new 0-based selection (Ctrl: -1 when there is nothing to select).
    int list(std::string id, std::vector<std::string> items, int selected, UiRect rect, int visible_rows = 4, bool enabled = true);
    void tooltip(std::string id, std::string text, UiRect rect);
    /// Single-line text field. `value` is the last committed value owned by the
    /// caller; the draft lives internally while focused. `max_length` caps
    /// codepoints (0 = 4096). Enter commits, Escape cancels, focus loss cancels.
    TextInputResult text_input(std::string id, std::string value, UiRect rect,
                               bool enabled = true, size_t max_length = 256);
    /// Controlled numeric field. `value` is the last valid value; interim
    /// drafts ("", "-") never overwrite it. `step` is the Up/Down increment;
    /// `integer_mode` rejects fractional input on commit. Parsing is
    /// locale-independent (std::from_chars); NaN/Inf/overflow are invalid.
    NumberInputResult number_input(std::string id, std::string label, double value,
                                   double minimum, double maximum, UiRect rect,
                                   bool enabled = true, double step = 1.0, bool integer_mode = false);
    /// Cancels capture/focus/editing without clearing the frame's elements, so
    /// a paused or hidden Game View keeps rendering its last snapshot while
    /// no hidden commit can queue. Scene-wide reset() is the stronger clear.
    void cancel_interaction();
    bool keyboard_focused() const { return !m_kb_focus.empty(); }
    bool consumes_keyboard() const { return !m_kb_focus.empty(); }
    bool wants_keyboard() const { return !m_kb_focus.empty(); }
    bool is_text_editing() const { return !m_editing_id.empty(); }
    const std::string& focus_id() const { return m_kb_focus; }
    // Test/host introspection: current draft for an editing field ("" if none).
    std::string editing_draft() const;
    DisplayList snapshot(float width, float height) const;
    size_t size() const { return m_elements.size(); }
    static TextAlign align_from_name(std::string_view name);
    static bool anchor_from_name(std::string_view name, UiAnchor& out);
private:
    enum class Kind { Label, Button, Bar, Slider, Panel, Checkbox, List, Tooltip, TextInput, NumberInput };
    struct Element {
        Kind kind{};
        std::string id, text;
        UiRect rect;
        double value = 0;
        float font_size = 22;
        bool enabled = true;
        TextAlign align = TextAlign::Left;
        std::vector<std::string> items;
        int selected = 0;
        int visible_rows = 4;
        // TextInput
        size_t max_length = 256;
        // NumberInput
        double num_min = 0, num_max = 1, num_step = 1;
        bool num_integer = false;
        std::string num_label;
    };
    struct EditState {
        std::string draft;      // text input draft OR number draft text
        size_t cursor = 0;      // byte offset, snapped to codepoint boundary
        double committed = 0;   // number: last valid value
        std::string error;      // number: status when invalid
        bool active = false;
    };
    bool contains(UiRect rect, UiPointer pointer) const;
    std::string key(const std::string& id) const;
    bool submit(Element element);
    bool is_interactive(Kind k) const {
        return k == Kind::Button || k == Kind::Slider || k == Kind::Checkbox || k == Kind::List ||
               k == Kind::TextInput || k == Kind::NumberInput;
    }
    void cancel_editing();
    std::vector<Element> m_elements;
    std::unordered_map<std::string, size_t> m_slots;
    std::unordered_map<std::string, int> m_list_scroll;
    std::unordered_map<std::string, EditState> m_edits;
    std::string m_scope, m_capture, m_consumed, m_kb_focus, m_editing_id;
    UiPointer m_pointer;
    UiKeys m_keys, m_prev_keys;
    Kind m_capture_kind = Kind::Label;
    TextMeasurer m_measurer;
    bool m_was_down = false, m_released = false, m_cursor = false, m_block_until_up = false, m_consumed_key = false;
    bool m_kb_focus_activate = false, m_kb_left = false, m_kb_right = false, m_kb_up = false, m_kb_down = false;
    bool m_kb_backspace = false, m_kb_delete = false, m_kb_home = false, m_kb_end = false, m_kb_escape = false;
    float m_vport_w = 1280, m_vport_h = 720;
};
} // namespace nf::ui
