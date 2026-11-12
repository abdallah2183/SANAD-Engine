#include <NF/UI/ScriptCanvas.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace nf::ui {
namespace {
bool valid_rect(UiRect r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h) &&
           r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 && r.x + r.w <= 1.001f && r.y + r.h <= 1.001f;
}
bool key_edge(bool now, bool was) { return now && !was; }

bool is_printable_codepoint(const std::string& utf8_cp) {
    if (utf8_cp.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(utf8_cp[0]);
        return c >= 0x20 && c != 0x7F;
    }
    return !utf8_cp.empty(); // any valid multibyte codepoint from WM_CHAR is printable text
}

// Locale-independent number parsing (never follows Windows locale).
enum class NumParse { Ok, Empty, Invalid, OutOfRange };
NumParse parse_number_draft(const std::string& draft, double mn, double mx, bool integer_mode, double& out) {
    std::string s = draft;
    auto trim = [](std::string& v) {
        size_t a = v.find_first_not_of(" \t");
        if (a == std::string::npos) { v.clear(); return; }
        size_t b = v.find_last_not_of(" \t");
        v = v.substr(a, b - a + 1);
    };
    trim(s);
    if (s.empty() || s == "-" || s == "+" || s == "." || s == "-." || s == "+.") return NumParse::Empty;
    if (integer_mode) {
        long long v = 0;
        const char* first = s.data();
        const char* last = s.data() + s.size();
        auto res = std::from_chars(first, last, v);
        if (res.ec == std::errc::result_out_of_range) return NumParse::OutOfRange;
        if (res.ec != std::errc() || res.ptr != last) return NumParse::Invalid;
        out = static_cast<double>(v);
        if (!std::isfinite(out) || out < mn || out > mx) return NumParse::OutOfRange;
        return NumParse::Ok;
    }
    double v = 0.0;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    auto res = std::from_chars(first, last, v);
    if (res.ec == std::errc::result_out_of_range) return NumParse::Invalid; // overflow -> invalid, never Inf
    if (res.ec != std::errc() || res.ptr != last) return NumParse::Invalid;
    if (!std::isfinite(v)) return NumParse::Invalid; // NaN/Inf rejected
    if (v < mn || v > mx) return NumParse::OutOfRange;
    out = v;
    return NumParse::Ok;
}

std::string format_number(double v, bool integer_mode) {
    char buf[64];
    if (integer_mode) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    } else {
        // Invariant formatting that round-trips the common cases.
        bool integral = std::isfinite(v) && std::floor(v) == v && std::abs(v) < 1e15;
        if (integral) std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        else std::snprintf(buf, sizeof(buf), "%.10g", v);
    }
    return std::string(buf);
}

} // namespace

TextAlign ScriptCanvas::align_from_name(std::string_view name) {
    return name == "center" ? TextAlign::Center : (name == "right" ? TextAlign::Right : TextAlign::Left);
}
bool ScriptCanvas::anchor_from_name(std::string_view name, UiAnchor& out) {
    if (name == "topleft") out = UiAnchor::TopLeft;
    else if (name == "topright") out = UiAnchor::TopRight;
    else if (name == "bottomleft") out = UiAnchor::BottomLeft;
    else if (name == "bottomright") out = UiAnchor::BottomRight;
    else if (name == "topcenter") out = UiAnchor::TopCenter;
    else if (name == "bottomcenter") out = UiAnchor::BottomCenter;
    else if (name == "leftcenter") out = UiAnchor::LeftCenter;
    else if (name == "rightcenter") out = UiAnchor::RightCenter;
    else if (name == "center") out = UiAnchor::Center;
    else return false;
    return true;
}

bool ScriptCanvas::contains(UiRect rect, UiPointer pointer) const {
    return pointer.focused && std::isfinite(pointer.x) && std::isfinite(pointer.y) &&
           pointer.x >= rect.x && pointer.y >= rect.y && pointer.x < rect.x + rect.w && pointer.y < rect.y + rect.h;
}
void ScriptCanvas::set_viewport(float width, float height) {
    if (std::isfinite(width) && std::isfinite(height) && width > 0 && height > 0) {
        m_vport_w = width;
        m_vport_h = height;
    }
}
UiRect ScriptCanvas::anchor_rect(UiAnchor anchor, float width, float height, float margin,
                               float min_width_px, float min_height_px) const {
    float w = std::clamp(width, 0.0f, 1.0f);
    float h = std::clamp(height, 0.0f, 1.0f);
    if (m_vport_w > 0 && min_width_px > 0) w = std::max(w, std::min(1.0f, min_width_px / m_vport_w));
    if (m_vport_h > 0 && min_height_px > 0) h = std::max(h, std::min(1.0f, min_height_px / m_vport_h));
    const float mx = std::clamp(margin, 0.0f, 1.0f);
    float x = 0, y = 0;
    switch (anchor) {
    case UiAnchor::TopLeft: x = mx; y = mx; break;
    case UiAnchor::TopRight: x = 1 - mx - w; y = mx; break;
    case UiAnchor::BottomLeft: x = mx; y = 1 - mx - h; break;
    case UiAnchor::BottomRight: x = 1 - mx - w; y = 1 - mx - h; break;
    case UiAnchor::TopCenter: x = 0.5f - w * 0.5f; y = mx; break;
    case UiAnchor::BottomCenter: x = 0.5f - w * 0.5f; y = 1 - mx - h; break;
    case UiAnchor::LeftCenter: x = mx; y = 0.5f - h * 0.5f; break;
    case UiAnchor::RightCenter: x = 1 - mx - w; y = 0.5f - h * 0.5f; break;
    case UiAnchor::Center: x = 0.5f - w * 0.5f; y = 0.5f - h * 0.5f; break;
    }
    x = std::clamp(x, 0.0f, 1.0f - w);
    y = std::clamp(y, 0.0f, 1.0f - h);
    return {x, y, w, h};
}
void ScriptCanvas::cancel_editing() {
    if (!m_editing_id.empty()) {
        m_edits.erase(m_editing_id);
        m_editing_id.clear();
    }
}
void ScriptCanvas::begin_frame(UiPointer pointer, UiKeys keys) {
    if (!pointer.focused && pointer.down) m_block_until_up = true;
    if (!pointer.down) m_block_until_up = false;
    m_released = m_was_down && !pointer.down && pointer.focused;
    const bool win_focused = pointer.focused && keys.focused;
    if (!win_focused) {
        cancel_editing(); // focus loss cancels the live draft, never commits it
        m_capture.clear();
        m_kb_focus.clear();
        m_pointer = pointer;
        m_keys = keys;
        m_prev_keys = keys;
        m_was_down = false;
        m_consumed.clear();
        m_elements.clear(); m_slots.clear(); m_cursor = false;
        return;
    }
    auto is_inter = [&](const Element& e) { return is_interactive(e.kind) && e.enabled; };
    if (!m_kb_focus.empty()) {
        const auto it = std::find_if(m_elements.begin(), m_elements.end(),
                                     [&](const Element& e) { return e.id == m_kb_focus; });
        if (it == m_elements.end()) {
            // Element vanished (page change) or was disabled: cancel any draft.
            if (!m_editing_id.empty() && m_editing_id == m_kb_focus) cancel_editing();
            m_kb_focus.clear();
        } else if (!it->enabled) {
            if (!m_editing_id.empty() && m_editing_id == m_kb_focus) cancel_editing();
            m_kb_focus.clear();
        }
    }
    const bool k_tab = key_edge(keys.tab, m_prev_keys.tab);
    const bool k_enter = key_edge(keys.enter, m_prev_keys.enter);
    const bool k_space = key_edge(keys.space, m_prev_keys.space);
    m_consumed_key = false;
    if (k_tab) {
        m_consumed_key = true;
        // Leaving a text field via Tab cancels its draft (same as focus loss).
        if (!m_editing_id.empty()) cancel_editing();
        std::vector<const Element*> order;
        for (const auto& e : m_elements) if (is_inter(e)) order.push_back(&e);
        if (!order.empty()) {
            int idx = -1;
            for (int i = 0; i < (int)order.size(); ++i) if (order[i]->id == m_kb_focus) { idx = i; break; }
            const int n = (int)order.size();
            idx = keys.shift ? ((idx <= 0) ? (n - 1) : (idx - 1)) : ((idx < 0) ? 0 : ((idx + 1) % n));
            m_kb_focus = order[idx]->id;
        } else {
            m_kb_focus.clear();
        }
    }
    m_kb_focus_activate = (k_enter || k_space) && !m_kb_focus.empty();
    m_kb_left = key_edge(keys.left, m_prev_keys.left);
    m_kb_right = key_edge(keys.right, m_prev_keys.right);
    m_kb_up = key_edge(keys.up, m_prev_keys.up);
    m_kb_down = key_edge(keys.down, m_prev_keys.down);
    m_kb_backspace = key_edge(keys.backspace, m_prev_keys.backspace);
    m_kb_delete = key_edge(keys.delete_key, m_prev_keys.delete_key);
    m_kb_home = key_edge(keys.home, m_prev_keys.home);
    m_kb_end = key_edge(keys.end, m_prev_keys.end);
    m_kb_escape = key_edge(keys.escape, m_prev_keys.escape);
    if (!m_was_down && pointer.down && !m_block_until_up) {
        m_capture.clear();
        for (auto it = m_elements.rbegin(); it != m_elements.rend(); ++it) {
            if (is_interactive(it->kind) && contains(it->rect, pointer)) {
                m_capture = it->id;
                m_capture_kind = it->kind;
                m_kb_focus = it->id;
                break;
            }
        }
        if (m_capture.empty()) {
            if (!m_editing_id.empty()) cancel_editing();
            m_kb_focus.clear();
        } else if (!m_editing_id.empty() && m_capture != m_editing_id) {
            // Clicking another widget cancels the live draft.
            cancel_editing();
        }
    }
    if (!m_released && !pointer.down) m_capture.clear();
    m_pointer = pointer;
    m_keys = keys;
    m_prev_keys = keys;
    m_was_down = pointer.down && pointer.focused;
    m_consumed.clear();
    m_elements.clear(); m_slots.clear(); m_cursor = false;
}
void ScriptCanvas::cancel_interaction() {
    cancel_editing();
    m_capture.clear();
    m_kb_focus.clear();
    m_consumed.clear();
    m_kb_focus_activate = false;
    m_kb_left = m_kb_right = m_kb_up = m_kb_down = false;
    m_kb_backspace = m_kb_delete = m_kb_home = m_kb_end = m_kb_escape = false;
    m_consumed_key = false;
}
void ScriptCanvas::reset() {
    m_elements.clear(); m_slots.clear(); m_scope.clear(); m_capture.clear(); m_consumed.clear();
    m_edits.clear(); m_editing_id.clear();
    m_pointer = {}; m_was_down = false; m_released = false; m_cursor = false; m_block_until_up = false;
    m_kb_focus.clear(); m_keys = {}; m_prev_keys = {}; m_list_scroll.clear();
    m_kb_focus_activate = false; m_kb_left = m_kb_right = m_kb_up = m_kb_down = false;
    m_kb_backspace = m_kb_delete = m_kb_home = m_kb_end = m_kb_escape = false;
}
void ScriptCanvas::set_scope(uint32_t id, uint32_t generation) {
    m_scope = std::to_string(id) + ":" + std::to_string(generation) + ":";
}
std::string ScriptCanvas::key(const std::string& id) const { return id.empty() ? std::string() : m_scope + id; }
std::string ScriptCanvas::editing_draft() const {
    if (m_editing_id.empty()) return {};
    auto it = m_edits.find(m_editing_id);
    return it == m_edits.end() ? std::string{} : it->second.draft;
}
bool ScriptCanvas::wants_pointer(UiPointer pointer) const {
    if (!pointer.focused) return false;
    if (m_cursor || !m_capture.empty()) return true;
    return std::any_of(m_elements.begin(), m_elements.end(), [&](const auto& element) {
        return element.enabled && is_interactive(element.kind) && contains(element.rect, pointer);
    });
}
bool ScriptCanvas::submit(Element element) {
    if (element.id.empty() || !valid_rect(element.rect) || element.id.size() > 160 || element.text.size() > 4096) return false;
    for (const auto& it : element.items) if (it.size() > 512) return false;
    if (element.kind == Kind::TextInput && element.text.size() > 16384) return false;
    if (auto found = m_slots.find(element.id); found != m_slots.end()) {
        m_elements[found->second] = std::move(element);
        return true;
    }
    if (m_elements.size() >= kMaxElements) return false;
    m_slots.emplace(element.id, m_elements.size());
    m_elements.push_back(std::move(element));
    return true;
}
void ScriptCanvas::panel(std::string id, UiRect rect) { submit({Kind::Panel, key(id), {}, rect}); }

void ScriptCanvas::label(std::string id, std::string text, UiRect rect, float size, TextAlign align) {
    Element element{Kind::Label, key(id), std::move(text), rect};
    element.font_size = std::isfinite(size) ? std::clamp(size, 10.0f, 64.0f) : 22;
    element.align = align;
    submit(std::move(element));
}
bool ScriptCanvas::button(std::string id, std::string text, UiRect rect, bool enabled, TextAlign align) {
    const auto scoped_id = key(id);
    Element element{Kind::Button, scoped_id, std::move(text), rect};
    element.enabled = enabled;
    element.align = align;
    if (!submit(std::move(element))) return false;
    if (!enabled && m_capture == scoped_id) m_capture.clear();
    if (!enabled && m_kb_focus == scoped_id) m_kb_focus.clear();
    if (enabled && m_consumed != scoped_id && m_capture_kind == Kind::Button && m_released && m_capture == scoped_id && contains(rect, m_pointer)) {
        m_consumed = scoped_id;
        return true;
    }
    if (enabled && m_kb_focus == scoped_id && m_kb_focus_activate && m_consumed != scoped_id) {
        m_consumed = scoped_id;
        m_kb_focus_activate = false;
        m_consumed_key = true;
        return true;
    }
    return false;
}
void ScriptCanvas::bar(std::string id, std::string text, double fraction, UiRect rect) {
    Element element{Kind::Bar, key(id), std::move(text), rect};
    element.value = std::isfinite(fraction) ? std::clamp(fraction, 0.0, 1.0) : 0;
    submit(std::move(element));
}
double ScriptCanvas::slider(std::string id, std::string text, double value, double minimum, double maximum, UiRect rect) {
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum || !std::isfinite(maximum - minimum)) return std::isfinite(value) ? value : 0;
    const auto scoped_id = key(id);
    double result = std::isfinite(value) ? std::clamp(value, minimum, maximum) : minimum;
    if (valid_rect(rect) && m_capture_kind == Kind::Slider && m_capture == scoped_id && m_pointer.down && m_pointer.focused)
        result = minimum + std::clamp(static_cast<double>((m_pointer.x - rect.x) / rect.w), 0.0, 1.0) * (maximum - minimum);
    if (m_kb_focus == scoped_id) {
        const double step = (maximum - minimum) * 0.05;
        if (m_kb_left) { result = std::clamp(result - step, minimum, maximum); m_kb_left = false; m_consumed_key = true; }
        if (m_kb_right) { result = std::clamp(result + step, minimum, maximum); m_kb_right = false; m_consumed_key = true; }
    }
    Element element{Kind::Slider, scoped_id, std::move(text), rect};
    element.value = (result - minimum) / (maximum - minimum);
    submit(std::move(element));
    return result;
}
bool ScriptCanvas::checkbox(std::string id, std::string text, bool checked, UiRect rect, bool enabled) {
    const auto scoped_id = key(id);
    bool result = checked;
    if (enabled && m_capture_kind == Kind::Checkbox && m_released && m_capture == scoped_id && m_consumed != scoped_id && contains(rect, m_pointer)) {
        result = !result;
        m_consumed = scoped_id;
    }
    if (enabled && m_kb_focus == scoped_id && m_kb_focus_activate && m_consumed != scoped_id) {
        result = !result;
        m_consumed = scoped_id;
        m_kb_focus_activate = false;
        m_consumed_key = true;
    }
    Element element{Kind::Checkbox, scoped_id, std::move(text), rect};
    element.enabled = enabled;
    element.value = result ? 1.0 : 0.0;
    if (!submit(std::move(element))) return checked;
    if (!enabled && m_capture == scoped_id) m_capture.clear();
    if (!enabled && m_kb_focus == scoped_id) m_kb_focus.clear();
    return result;
}
int ScriptCanvas::list(std::string id, std::vector<std::string> items, int selected, UiRect rect, int visible_rows, bool enabled) {
    const auto scoped_id = key(id);
    if (items.empty()) return -1;
    int sel = std::clamp(selected, 0, (int)items.size() - 1);
    int rows = std::clamp(visible_rows, 1, 32);
    int& scroll = m_list_scroll[scoped_id];
    scroll = std::clamp(scroll, 0, std::max(0, (int)items.size() - rows));
    if (enabled && m_capture_kind == Kind::List && m_released && m_capture == scoped_id && m_consumed != scoped_id && contains(rect, m_pointer)) {
        const float rel = (m_pointer.y - rect.y) / rect.h;
        const int row = std::clamp((int)(rel * rows), 0, rows - 1);
        const int idx = scroll + row;
        if (idx >= 0 && idx < (int)items.size()) {
            sel = idx;
            m_consumed = scoped_id;
        }
    }
    if (m_keys.wheel != 0 && m_capture == scoped_id) {
        scroll = std::clamp(scroll - (int)std::lround(m_keys.wheel), 0, std::max(0, (int)items.size() - rows));
        m_keys.wheel = 0;
    }
    if (enabled && m_kb_focus == scoped_id) {
        if (m_kb_up) { sel = std::max(0, sel - 1); m_kb_up = false; m_consumed_key = true; }
        if (m_kb_down) { sel = std::min((int)items.size() - 1, sel + 1); m_kb_down = false; m_consumed_key = true; }
        if (m_kb_left) { scroll = std::max(0, scroll - 1); m_kb_left = false; }
        if (m_kb_right) { scroll = std::min(std::max(0, (int)items.size() - rows), scroll + 1); m_kb_right = false; }
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + rows) scroll = sel - rows + 1;
    }
    Element element{Kind::List, scoped_id, "", rect};
    element.enabled = enabled;
    element.items = items;
    element.selected = sel;
    element.visible_rows = rows;
    element.value = sel;
    if (!submit(std::move(element))) return selected;
    if (!enabled && m_capture == scoped_id) m_capture.clear();
    if (!enabled && m_kb_focus == scoped_id) m_kb_focus.clear();
    return sel;
}
void ScriptCanvas::tooltip(std::string id, std::string text, UiRect rect) {
    Element element{Kind::Tooltip, key(id), std::move(text), rect};
    submit(std::move(element));
}

TextInputResult ScriptCanvas::text_input(std::string id, std::string value, UiRect rect,
                                          bool enabled, size_t max_length) {
    const auto scoped_id = key(id);
    TextInputResult out;
    out.value = value;
    out.draft = value;
    if (!enabled) {
        if (m_capture == scoped_id) m_capture.clear();
        if (m_kb_focus == scoped_id) m_kb_focus.clear();
        if (m_editing_id == scoped_id) cancel_editing();
        Element element{Kind::TextInput, scoped_id, value, rect};
        element.enabled = false;
        element.max_length = max_length;
        element.text = value;
        submit(std::move(element));
        return out;
    }
    const size_t cap = (max_length == 0 ? 4096 : std::min<size_t>(max_length, 4096));
    if (value.size() > 16384) value.resize(16384); // hard guard: never trust the caller blindly
    // Apply control keys to the live draft (edge-triggered, codepoint safe).
    const bool focused = (m_kb_focus == scoped_id);
    if (focused) {
        if (m_editing_id != scoped_id) {
            // Focus just arrived: seed the draft from the caller's value.
            EditState st;
            st.draft = value;
            st.cursor = value.size();
            st.active = true;
            m_edits[scoped_id] = std::move(st);
            m_editing_id = scoped_id;
        }
        auto& st = m_edits[scoped_id];
        st.cursor = utf8_snap_boundary(st.draft, st.cursor);
        // Escape cancels BEFORE insertion so the char batch is dropped too.
        bool cancelled = false;
        if (m_kb_escape) {
            m_kb_escape = false;
            m_kb_focus_activate = false;
            m_consumed_key = true;
            out.cancelled = true;
            out.editing = true;
            out.draft = value;
            m_edits.erase(scoped_id);
            m_editing_id.clear();
            m_kb_focus.clear();
            cancelled = true;
        }
        if (!cancelled) {
            // Cursor motion (one codepoint per press).
            if (m_kb_left) {
                m_kb_left = false;
                if (st.cursor > 0) st.cursor = utf8_prev_boundary(st.draft, st.cursor);
                m_consumed_key = true;
            }
            if (m_kb_right) {
                m_kb_right = false;
                if (st.cursor < st.draft.size()) st.cursor = utf8_next_boundary(st.draft, st.cursor);
                m_consumed_key = true;
            }
            if (m_kb_home) { m_kb_home = false; st.cursor = 0; m_consumed_key = true; }
            if (m_kb_end) { m_kb_end = false; st.cursor = st.draft.size(); m_consumed_key = true; }
            if (m_kb_backspace) {
                m_kb_backspace = false;
                if (st.cursor > 0) {
                    const size_t prev = utf8_prev_boundary(st.draft, st.cursor);
                    st.draft.erase(prev, st.cursor - prev);
                    st.cursor = prev;
                }
                m_consumed_key = true;
            }
            if (m_kb_delete) {
                m_kb_delete = false;
                if (st.cursor < st.draft.size()) {
                    const size_t next = utf8_next_boundary(st.draft, st.cursor);
                    st.draft.erase(st.cursor, next - st.cursor);
                }
                m_consumed_key = true;
            }
            // Insert WM_CHAR codepoints (each is one valid UTF-8 codepoint).
            for (const std::string& cp_text : m_keys.text) {
                if (cp_text.empty() || cp_text == "\r" || cp_text == "\n" || cp_text == "\t") continue;
                if (!is_printable_codepoint(cp_text)) continue;
                // Never split a sequence: the platform combines surrogates first;
                // a lone surrogate must never reach here as two half bytes.
                if (utf8_codepoint_count(st.draft) + utf8_codepoint_count(cp_text) > cap) continue;
                st.draft.insert(st.cursor, cp_text);
                st.cursor += cp_text.size();
                st.cursor = utf8_snap_boundary(st.draft, st.cursor);
                m_consumed_key = true;
            }
            // Enter commits. The generic focus-activate path must not ALSO fire
            // a button under the text field on the same press.
            if (m_kb_focus_activate) {
                m_kb_focus_activate = false;
                m_consumed_key = true;
                out.committed = true;
                out.value = st.draft;
                out.draft = st.draft;
                out.editing = true;
                m_edits.erase(scoped_id);
                m_editing_id.clear();
                m_kb_focus.clear();
                Element element{Kind::TextInput, scoped_id, out.value, rect};
                element.enabled = true;
                element.max_length = cap;
                element.text = out.value;
                submit(std::move(element));
                return out;
            }
            out.editing = true;
            out.draft = st.draft;
            out.value = value; // committed stays the caller's until Enter
        } else {
            // Cancel path already cleared the edit; submit the old value.
        }
    } else {
        if (m_editing_id == scoped_id) cancel_editing();
    }
    Element element{Kind::TextInput, scoped_id, value, rect};
    element.enabled = true;
    element.max_length = cap;
    // Snapshot shows the draft while editing so the user sees what they type.
    if (focused && !m_editing_id.empty() && m_editing_id == scoped_id) {
        auto it = m_edits.find(scoped_id);
        if (it != m_edits.end()) element.text = it->second.draft;
    }
    submit(std::move(element));
    out.draft = element.text;
    return out;
}

NumberInputResult ScriptCanvas::number_input(std::string id, std::string label, double value,
                                              double minimum, double maximum, UiRect rect,
                                              bool enabled, double step, bool integer_mode) {
    const auto scoped_id = key(id);
    NumberInputResult out;
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum ||
        !std::isfinite(maximum - minimum) || !std::isfinite(step) || step <= 0.0 ||
        !std::isfinite(value)) {
        out.value = std::isfinite(value) ? value : 0.0;
        out.draft = format_number(out.value, integer_mode);
        out.valid = false;
        out.error = "Invalid range";
        Element element{Kind::NumberInput, scoped_id, label, rect};
        element.enabled = false;
        submit(std::move(element));
        return out;
    }
    const double last_valid = std::clamp(value, minimum, maximum);
    out.value = last_valid;
    out.draft = format_number(last_valid, integer_mode);
    if (!enabled) {
        if (m_capture == scoped_id) m_capture.clear();
        if (m_kb_focus == scoped_id) m_kb_focus.clear();
        if (m_editing_id == scoped_id) cancel_editing();
        Element element{Kind::NumberInput, scoped_id, label, rect};
        element.enabled = false;
        element.value = last_valid;
        element.num_min = minimum;
        element.num_max = maximum;
        element.num_step = step;
        element.num_integer = integer_mode;
        element.num_label = label;
        submit(std::move(element));
        return out;
    }
    const bool focused = (m_kb_focus == scoped_id);
    if (focused) {
        if (m_editing_id != scoped_id) {
            EditState st;
            st.draft = format_number(last_valid, integer_mode);
            st.cursor = st.draft.size();
            st.committed = last_valid;
            st.active = true;
            m_edits[scoped_id] = std::move(st);
            m_editing_id = scoped_id;
        }
        auto& st = m_edits[scoped_id];
        // If the caller changed the committed value externally while editing,
        // adopt it as the baseline (do not fight the caller).
        if (st.committed != last_valid) {
            // Only re-baseline when the draft is still the old canonical value.
            if (st.draft == format_number(st.committed, integer_mode)) {
                st.committed = last_valid;
                st.draft = format_number(last_valid, integer_mode);
                st.cursor = st.draft.size();
                st.error.clear();
            }
        }
        bool cancelled = false;
        if (m_kb_escape) {
            m_kb_escape = false;
            m_kb_focus_activate = false;
            m_consumed_key = true;
            out.cancelled = true;
            out.editing = true;
            out.value = last_valid;
            out.draft = format_number(last_valid, integer_mode);
            out.valid = true;
            m_edits.erase(scoped_id);
            m_editing_id.clear();
            m_kb_focus.clear();
            cancelled = true;
        }
        if (!cancelled) {
            // Up/Down step the committed value directly (clamped, committed).
            if (m_kb_up || m_kb_down) {
                const double delta = (m_kb_up ? step : -step);
                m_kb_up = m_kb_down = false;
                m_consumed_key = true;
                double next = std::clamp(st.committed + delta, minimum, maximum);
                if (integer_mode) next = std::round(next);
                st.committed = next;
                st.draft = format_number(next, integer_mode);
                st.cursor = st.draft.size();
                st.error.clear();
                out.committed = true;
                out.valid = true;
                out.value = next;
                out.draft = st.draft;
                out.editing = true;
                Element element{Kind::NumberInput, scoped_id, label, rect};
                element.enabled = true;
                element.value = next;
                element.num_min = minimum;
                element.num_max = maximum;
                element.num_step = step;
                element.num_integer = integer_mode;
                element.num_label = label;
                element.text = st.draft;
                submit(std::move(element));
                return out;
            }
            if (m_kb_left) {
                m_kb_left = false;
                if (st.cursor > 0) st.cursor = utf8_prev_boundary(st.draft, st.cursor);
                m_consumed_key = true;
            }
            if (m_kb_right) {
                m_kb_right = false;
                if (st.cursor < st.draft.size()) st.cursor = utf8_next_boundary(st.draft, st.cursor);
                m_consumed_key = true;
            }
            if (m_kb_home) { m_kb_home = false; st.cursor = 0; m_consumed_key = true; }
            if (m_kb_end) { m_kb_end = false; st.cursor = st.draft.size(); m_consumed_key = true; }
            if (m_kb_backspace) {
                m_kb_backspace = false;
                if (st.cursor > 0) {
                    const size_t prev = utf8_prev_boundary(st.draft, st.cursor);
                    st.draft.erase(prev, st.cursor - prev);
                    st.cursor = prev;
                }
                m_consumed_key = true;
            }
            if (m_kb_delete) {
                m_kb_delete = false;
                if (st.cursor < st.draft.size()) {
                    const size_t next = utf8_next_boundary(st.draft, st.cursor);
                    st.draft.erase(st.cursor, next - st.cursor);
                }
                m_consumed_key = true;
            }
            for (const std::string& cp_text : m_keys.text) {
                if (cp_text.empty() || cp_text == "\r" || cp_text == "\n" || cp_text == "\t") continue;
                if (st.draft.size() + cp_text.size() > 64) continue;
                st.draft.insert(st.cursor, cp_text);
                st.cursor += cp_text.size();
                m_consumed_key = true;
            }
            double parsed = 0.0;
            const NumParse parsed_kind = parse_number_draft(st.draft, minimum, maximum, integer_mode, parsed);
            if (parsed_kind == NumParse::Ok) {
                st.error.clear();
            } else if (parsed_kind == NumParse::Empty) {
                st.error = "Enter a number";
            } else if (parsed_kind == NumParse::Invalid) {
                st.error = "Invalid number";
            } else {
                st.error = "Out of range";
            }
            if (m_kb_focus_activate) {
                m_kb_focus_activate = false;
                m_consumed_key = true;
                out.editing = true;
                if (parsed_kind == NumParse::Ok) {
                    st.committed = parsed;
                    out.committed = true;
                    out.valid = true;
                    out.error.clear();
                    out.value = parsed;
                    out.draft = format_number(parsed, integer_mode);
                    m_edits.erase(scoped_id);
                    m_editing_id.clear();
                    m_kb_focus.clear();
                    Element element{Kind::NumberInput, scoped_id, label, rect};
                    element.enabled = true;
                    element.value = parsed;
                    element.num_min = minimum;
                    element.num_max = maximum;
                    element.num_step = step;
                    element.num_integer = integer_mode;
                    element.num_label = label;
                    element.text = out.draft;
                    submit(std::move(element));
                    return out;
                }
                // Invalid commit: keep last valid, keep editing open to fix it.
                out.valid = false;
                out.error = st.error;
                out.value = last_valid;
                out.draft = st.draft;
                Element element{Kind::NumberInput, scoped_id, label, rect};
                element.enabled = true;
                element.value = last_valid;
                element.num_min = minimum;
                element.num_max = maximum;
                element.num_step = step;
                element.num_integer = integer_mode;
                element.num_label = label;
                element.text = st.draft;
                submit(std::move(element));
                return out;
            }
            out.editing = true;
            out.value = last_valid; // never clobber the committed value mid-edit
            out.draft = st.draft;
            out.valid = (parsed_kind == NumParse::Ok);
            out.error = st.error;
        }
    } else {
        if (m_editing_id == scoped_id) cancel_editing();
        out.valid = true;
        out.error.clear();
    }
    Element element{Kind::NumberInput, scoped_id, label, rect};
    element.enabled = true;
    element.value = out.value;
    element.num_min = minimum;
    element.num_max = maximum;
    element.num_step = step;
    element.num_integer = integer_mode;
    element.num_label = label;
    element.text = out.draft;
    submit(std::move(element));
    return out;
}

DisplayList ScriptCanvas::snapshot(float width, float height) const {
    DisplayList list;
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) return list;
    auto measure = m_measurer ? m_measurer : TextMeasurer{};
    auto push_focus = [&](const UiRect& r) {
        DlLine l;
        l.width = 2.0f;
        l.color = UiColor{1.0f, 0.82f, 0.2f, 1.0f};
        l.x1 = r.x - 2; l.y1 = r.y - 2; l.x2 = r.x + r.w + 2; l.y2 = r.y - 2; list.lines.push_back(l);
        l.y1 = r.y + r.h + 2; l.y2 = r.y + r.h + 2; list.lines.push_back(l);
        l.x1 = r.x - 2; l.y1 = r.y - 2; l.x2 = r.x - 2; l.y2 = r.y + r.h + 2; list.lines.push_back(l);
        l.x1 = r.x + r.w + 2; l.x2 = r.x + r.w + 2; list.lines.push_back(l);
    };
    for (const auto& element : m_elements) {
        const UiRect r{element.rect.x * width, element.rect.y * height, element.rect.w * width, element.rect.h * height};
        const bool interactive = is_interactive(element.kind);
        const bool kb_focused = interactive && element.enabled && element.id == m_kb_focus;
        const bool hover = interactive && element.enabled && contains(element.rect, m_pointer);
        if (element.kind == Kind::Checkbox) {
            const float box = std::min(r.h * 0.62f, 22.0f);
            const float bx = r.x + 6.0f, by = r.y + (r.h - box) * 0.5f;
            list.rects.push_back({{bx, by, box, box}, !element.enabled ? UiColor{.13f,.14f,.17f,.92f} : UiColor{.08f,.11f,.17f,.94f}});
            if (element.value > 0.5) {
                const float inset = box * 0.28f;
                DlLine tick;
                tick.width = 2.0f;
                tick.color = UiColor{.3f,.85f,1,1};
                tick.x1 = bx + inset; tick.y1 = by + box * 0.55f; tick.x2 = bx + box * 0.45f; tick.y2 = by + box - inset; list.lines.push_back(tick);
                tick.x1 = bx + box * 0.45f; tick.y1 = by + box - inset; tick.x2 = bx + box - inset; tick.y2 = by + inset; list.lines.push_back(tick);
            }
        } else if (element.kind == Kind::List) {
            list.rects.push_back({r, !element.enabled ? UiColor{.13f,.14f,.17f,.92f} : (hover ? UiColor{.14f,.28f,.48f,.96f} : UiColor{.08f,.11f,.17f,.94f})});
            const int rows = std::max(1, element.visible_rows);
            const float row_h = r.h / rows;
            int scroll = 0;
            auto sc = m_list_scroll.find(element.id);
            if (sc != m_list_scroll.end()) scroll = sc->second;
            scroll = std::clamp(scroll, 0, std::max(0, (int)element.items.size() - rows));
            const int first = scroll;
            const int last = std::min((int)element.items.size(), first + rows);
            for (int i = first; i < last; ++i) {
                const float ry = r.y + (i - first) * row_h;
                UiRect row{r.x, ry, r.w, row_h};
                const bool sel_row = i == element.selected;
                if (sel_row) list.rects.push_back({row, UiColor{.16f,.52f,.87f,.85f}});
                DlText t;
                t.x = row.x + 8.0f;
                t.y = row.y;
                t.wrap_width = std::max(0.0f, row.w - 16.0f);
                t.box_height = row_h;
                t.text = element.items[i];
                t.size = element.font_size;
                t.color = element.enabled ? UiColor{.94f,.96f,1,1} : UiColor{.45f,.48f,.53f,1};
                t.align = TextAlign::Left;
                list.texts.push_back(std::move(t));
            }
            if (first > 0) {
                list.rects.push_back({{r.x + r.w - 10.0f, r.y + 2.0f, 8.0f, 4.0f}, UiColor{.9f,.9f,.9f,.9f}});
            }
            if (last < (int)element.items.size()) {
                list.rects.push_back({{r.x + r.w - 10.0f, r.y + r.h - 6.0f, 8.0f, 4.0f}, UiColor{.9f,.9f,.9f,.9f}});
            }
        } else if (element.kind == Kind::Tooltip) {
            if (contains(element.rect, m_pointer)) {
                // Measured tooltip: box + lines come from the draw measurer.
                const TooltipLayout tl = layout_tooltip(element.text, width, height,
                                                        m_pointer.x * width, m_pointer.y * height,
                                                        element.font_size, measure);
                list.rects.push_back({{tl.x, tl.y, tl.w, tl.h}, UiColor{.07f,.09f,.12f,.96f}});
                DlText t;
                t.x = tl.x + 8.0f;
                t.y = tl.y + 6.0f;
                t.wrap_width = std::max(0.0f, tl.w - 16.0f);
                t.box_height = std::max(0.0f, tl.h - 12.0f);
                t.text = element.text;
                t.size = element.font_size;
                t.color = UiColor{.94f,.96f,1,1};
                t.align = TextAlign::Left;
                list.texts.push_back(std::move(t));
            }
            continue;
        } else if (element.kind == Kind::TextInput || element.kind == Kind::NumberInput) {
            // Field background + bottom border; caret/error below the text path.
            const UiColor bg = !element.enabled ? UiColor{.13f,.14f,.17f,.92f} :
                (kb_focused ? UiColor{.10f,.16f,.26f,.96f} : (hover ? UiColor{.14f,.28f,.48f,.96f} : UiColor{.08f,.11f,.17f,.94f}));
            list.rects.push_back({r, bg});
            // Outer border (focus = gold, else subtle).
            const UiColor border = kb_focused ? UiColor{1.0f,0.82f,0.2f,1.0f} : UiColor{.35f,.40f,.50f,1.0f};
            DlLine bl;
            bl.width = 1.5f;
            bl.color = border;
            bl.x1 = r.x; bl.y1 = r.y; bl.x2 = r.x + r.w; bl.y2 = r.y; list.lines.push_back(bl);
            bl.y1 = r.y + r.h; bl.y2 = r.y + r.h; list.lines.push_back(bl);
            bl.x1 = r.x; bl.y1 = r.y; bl.x2 = r.x; bl.y2 = r.y + r.h; list.lines.push_back(bl);
            bl.x1 = r.x + r.w; bl.x2 = r.x + r.w; list.lines.push_back(bl);
            if (kb_focused) push_focus(r);
            const std::string shown = element.text;
            // Caret: measure the logical prefix (shaped once) for its x.
            if (kb_focused && !m_editing_id.empty()) {
                auto it = m_edits.find(element.id);
                if (it != m_edits.end()) {
                    const std::string prefix = shown.substr(0, std::min(it->second.cursor, shown.size()));
                    const std::string shaped_prefix = needs_shaping(prefix) ? shape_arabic(prefix) : prefix;
                    float cw = 0.0f;
                    if (measure) {
                        cw = measure(shaped_prefix, element.font_size);
                    } else {
                        cw = fallback_text_width(shaped_prefix, element.font_size);
                    }
                    if (!std::isfinite(cw)) cw = 0.0f;
                    const float caret_x = std::min(r.x + 8.0f + cw, r.x + r.w - 3.0f);
                    DlLine caret;
                    caret.width = 2.0f;
                    caret.color = UiColor{1.0f,1.0f,1.0f,0.95f};
                    caret.x1 = caret_x; caret.y1 = r.y + 3.0f; caret.x2 = caret_x; caret.y2 = r.y + r.h - 3.0f;
                    list.lines.push_back(caret);
                }
            }
            DlText t;
            t.x = r.x + 8.0f;
            t.y = r.y;
            t.wrap_width = std::max(0.0f, r.w - 16.0f);
            t.box_height = r.h;
            t.text = shown;
            t.size = element.font_size;
            // Number errors are shown in red right under the field text.
            bool show_error = false;
            std::string err;
            if (element.kind == Kind::NumberInput) {
                auto it = m_edits.find(element.id);
                if (it != m_edits.end() && !it->second.error.empty()) {
                    show_error = true;
                    err = it->second.error;
                }
            }
            if (show_error) {
                t.color = UiColor{1.0f,0.45f,0.40f,1.0f};
            } else {
                t.color = element.enabled ? UiColor{.94f,.96f,1,1} : UiColor{.45f,.48f,.53f,1};
            }
            t.align = TextAlign::Left;
            list.texts.push_back(std::move(t));
            if (show_error) {
                // Clamp the status line so it never draws off the bottom edge.
                DlText e;
                e.x = r.x + 8.0f;
                e.y = std::min(r.y + r.h, height - element.font_size * 1.22f);
                e.wrap_width = std::max(0.0f, r.w - 16.0f);
                e.box_height = element.font_size * 1.22f;
                e.text = err;
                e.size = element.font_size * 0.82f;
                e.color = UiColor{1.0f,0.45f,0.40f,1.0f};
                e.align = TextAlign::Left;
                list.texts.push_back(std::move(e));
            }
            continue;
        } else if (element.kind != Kind::Label) {
            const UiColor background = !element.enabled ? UiColor{.13f,.14f,.17f,.92f} :
                hover ? UiColor{.14f,.28f,.48f,.96f} : UiColor{.08f,.11f,.17f,.94f};
            list.rects.push_back({r, background});
        }
        if (element.kind == Kind::Bar || element.kind == Kind::Slider) {
            UiRect fill = r; fill.w *= static_cast<float>(element.value);
            list.rects.push_back({fill, {.16f,.52f,.87f,.9f}});
        }
        if (kb_focused) push_focus(r);
        if (element.kind == Kind::Panel) continue;
        if (element.kind == Kind::List) continue;
        const UiColor color = element.enabled ? UiColor{.94f,.96f,1,1} : UiColor{.45f,.48f,.53f,1};
        const float tx_x = (element.kind == Kind::Checkbox) ? (r.x + std::min(r.h * 0.62f, 22.0f) + 16.0f) : r.x;
        const float tw_w = (element.kind == Kind::Checkbox) ? std::max(0.0f, r.w - (tx_x - r.x)) : r.w;
        DlText t;
        t.x = tx_x;
        t.y = r.y;
        t.wrap_width = tw_w;
        t.box_height = r.h;
        t.text = element.text;
        t.size = element.font_size;
        t.color = color;
        t.align = (element.kind == Kind::Label) ? element.align : (element.kind == Kind::Button ? element.align : TextAlign::Center);
        if (element.kind == Kind::Button || element.kind == Kind::Slider || element.kind == Kind::Bar) {
            if (element.kind != Kind::Label) t.align = (element.align == TextAlign::Left || element.align == TextAlign::Right) ? element.align : TextAlign::Center;
            if (element.kind == Kind::Bar || element.kind == Kind::Slider) t.align = TextAlign::Center;
        }
        list.texts.push_back(std::move(t));
    }
    return list;
}
} // namespace nf::ui
