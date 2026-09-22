#pragma once

// NF/Editor/UiText.hpp — the ONE display-string path for the ImGui editor.
//
// Every string a widget draws goes through AV(): translate (logical UTF-8)
// then shape into visual presentation forms. ImGui has no bidi reordering, so
// an unshaped logical string renders mirrored — that was the entire "reversed
// Arabic" bug class. There is deliberately no TR() helper: logic that needs an
// unshaped string (IDs, comparisons, lookups) calls ui::tr() directly so the
// unshaped call is visibly deliberate.
//
// RTL alignment: when Arabic is active the rt_* wrappers right-align
// single-line text by shifting the cursor to the content right edge. The shift
// applies ONLY at a line start (SameLine chains are untouched), never inside
// auto-fit frames (tooltips/popups measure with degenerate rects), and never
// for multi-line wrapped paragraphs (only their single-line labels align).
// Menu-popup repositioning and tree-row full mirroring are out of scope:
// order, tab flow and floating windows stay exactly as in English.

#include <NF/UI/ArabicShaper.hpp>
#include <NF/UI/Localization.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <string>
#include <vector>

namespace nf::editor {

// Translated + SHAPED for display (visual UTF-8, ready for a widget).
inline std::string AV(const char* key) {
    return ui::shape_arabic(ui::tr(key));
}

// Formatted display string: snprintf over the TRANSLATED format, then shape.
// Mixed content (Latin paths, %d/%s numbers) keeps its order — the shaper
// preserves Latin/digit runs inside an Arabic line.
template <typename... Args>
inline std::string AVF(const char* key, Args... args) {
    const std::string fmt = ui::tr(key);
    // Size first (snprintf with nullptr/0), then format for real.
    const int n = std::snprintf(nullptr, 0, fmt.c_str(), args...);
    if (n <= 0) {
        return ui::shape_arabic(fmt);
    }
    std::vector<char> buf(static_cast<std::size_t>(n) + 1, '\0');
    std::snprintf(buf.data(), buf.size(), fmt.c_str(), args...);
    return ui::shape_arabic(std::string(buf.data(), static_cast<std::size_t>(n)));
}

// True while the Arabic UI is active (the RTL switch for the wrappers below).
inline bool rt_active() {
    return ui::current_language() == ui::Language::Arabic;
}

// Shifts the cursor right so a `text_w`-wide string ends at the content right
// edge. No-op unless Arabic AND at a line start AND inside a laid-out window.
inline void rt_shift_for(float text_w) {
    if (!rt_active() || text_w <= 0.0f) {
        return;
    }
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win == nullptr || win->SkipItems) {
        return;
    }
    // Auto-fit frames (tooltips, popups measuring themselves) have no stable
    // right edge — shifting there would throw the popup off anchor.
    if (win->AutoFitFramesX > 0 || win->AutoFitFramesY > 0) {
        return;
    }
    // CursorPos/CursorStartPos are screen-space pen positions: equal means the
    // item starts the line. Anything past it is a SameLine chain — leave it.
    if (win->DC.CursorPos.x > win->DC.CursorStartPos.x + 0.5f) {
        return;
    }
    const float avail = win->WorkRect.Max.x - win->DC.CursorPos.x;
    if (text_w < avail) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - text_w));
    }
}

// Single-line text wrappers (right-aligned when Arabic).
inline void rt_text_str(const std::string& s) {
    rt_shift_for(ImGui::CalcTextSize(s.c_str()).x);
    ImGui::TextUnformatted(s.c_str());
}
inline void rt_text_key(const char* key) {
    rt_text_str(AV(key));
}
inline void rt_disabled_str(const std::string& s) {
    rt_shift_for(ImGui::CalcTextSize(s.c_str()).x);
    ImGui::TextDisabled("%s", s.c_str());
}
inline void rt_disabled_key(const char* key) {
    rt_disabled_str(AV(key));
}
inline void rt_colored_str(const ImVec4& col, const std::string& s) {
    rt_shift_for(ImGui::CalcTextSize(s.c_str()).x);
    ImGui::TextColored(col, "%s", s.c_str());
}
// Multi-line paragraphs: no per-line shift exists for wrapped text, so these
// stay naturally aligned (documented limitation); the call still goes through
// AV so the wording translates and shapes.
inline void rt_wrapped_str(const std::string& s) {
    ImGui::TextWrapped("%s", s.c_str());
}
inline void rt_wrapped_key(const char* key) {
    rt_wrapped_str(AV(key));
}

} // namespace nf::editor
