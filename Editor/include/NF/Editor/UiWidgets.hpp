#pragma once

// NF/Editor/UiWidgets.hpp — the small shared primitives every editor panel uses.
//
// A professional-looking tool is mostly CONSISTENCY: the same section header,
// the same key/value rhythm, the same chip, the same hint styling on every
// panel. Without a shared set each panel re-invents them, they drift apart, and
// the result reads as a collection of screens rather than one application.
//
// Everything here is display-only and header-inline: no state, no EditorApp, so
// any panel (or dialog) can include it.

#include <NF/Editor/UiText.hpp>
#include <NF/Editor/UiTheme.hpp>

#include <imgui.h>

#include <cfloat>
#include <string>

namespace nf::editor::uiw {

// --- Sections ---------------------------------------------------------------

/// One collapsing section, one look. `default_open` is the common case for a
/// section whose contents are the reason the panel exists.
inline bool section(const char* label, bool default_open = true) {
    return ImGui::CollapsingHeader(
        label, default_open ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None);
}

/// A section that is only a heading (no collapse), for grouping without hiding.
inline void heading(const char* label) {
    ImGui::SeparatorText(label);
}

// --- Text -------------------------------------------------------------------

/// Dimmed, wrapped explanatory text. Wrapped because every one of these is a
/// full sentence and a panel can be a narrow column.
inline void hint(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

/// Dimmed single line, ellipsised to the space left on the line. For paths.
inline void hint_line(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::text_secondary());
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
}

// --- Key/value rows ---------------------------------------------------------

/// A read-only "key ......... value" row: the value is right-aligned against the
/// panel edge, which is what makes a column of numbers scannable instead of a
/// ragged left-aligned list.
inline void kv(const std::string& key, const std::string& value) {
    ImGui::TextDisabled("%s", key.c_str());
    ImGui::SameLine(0.0f, 8.0f);
    const float w = ImGui::CalcTextSize(value.c_str()).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (w < avail - 4.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    }
    ImGui::TextUnformatted(value.c_str());
}

/// Same row, value tinted — for a number that carries a verdict (healthy,
/// over budget, broken).
inline void kv_colored(const std::string& key, const std::string& value, const ImVec4& col) {
    ImGui::TextDisabled("%s", key.c_str());
    ImGui::SameLine(0.0f, 8.0f);
    const float w = ImGui::CalcTextSize(value.c_str()).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (w < avail - 4.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    }
    ImGui::TextColored(col, "%s", value.c_str());
}

// --- Chips ------------------------------------------------------------------

/// A small filled badge. Drawn, never a Button: a chip is a label, and a label
/// must not consume an ImGui ID (two chips with the same text would collide).
inline void chip(const std::string& text, const ImVec4& col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const float pad = 7.0f;
    const float h = ts.y + 4.0f;
    const ImVec2 a(pos.x, pos.y);
    const ImVec2 b(pos.x + ts.x + pad * 2.0f, pos.y + h);
    dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.18f)), 4.0f);
    dl->AddRect(a, b, ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.55f)), 4.0f, 0, 1.0f);
    dl->AddText(ImVec2(a.x + pad, a.y + 2.0f), ImGui::GetColorU32(col), text.c_str());
    ImGui::Dummy(ImVec2(b.x - a.x, h));
}

/// Chips for the components an entity carries, so the panel answers "what IS
/// this thing" before the user reads a single field.
inline void chip_row(const std::string* labels, int count) {
    for (int i = 0; i < count; ++i) {
        if (labels[i].empty()) {
            continue;
        }
        if (i > 0) {
            ImGui::SameLine(0.0f, 6.0f);
        }
        chip(labels[i], theme::accent());
    }
}

// --- Panel header -----------------------------------------------------------

/// The strip at the top of a dedicated editor: what this panel is bound to.
///
/// Every dedicated editor edits ONE live scene object, and the first thing a
/// user needs is which one — otherwise a Lighting panel on a scene with two
/// lights is a guess. `bound` is the object's display name; empty means nothing
/// to bind to, and the caller should show its own "no X in this scene" hint.
inline void bound_header(const std::string& kind, const std::string& bound) {
    ImGui::TextDisabled("%s", kind.c_str());
    ImGui::SameLine(0.0f, 8.0f);
    if (bound.empty()) {
        chip(AV("none"), theme::danger());
    } else {
        chip(bound, theme::accent());
    }
    ImGui::Spacing();
}

/// A labelled control row: fixed-width label column, control after it. Keeps
/// every numeric field in a panel on one vertical axis.
inline void row_label(const std::string& label, float width = 110.0f) {
    ImGui::TextUnformatted(label.c_str());
    if (width > 0.0f) {
        ImGui::SameLine(width);
    } else {
        ImGui::SameLine();
    }
}

/// Full-width primary action (Apply / Save). Centred text reads as the panel's
/// one commitment rather than one more field.
inline bool primary_button(const std::string& label) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::alpha(theme::accent(), 0.75f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::alpha(theme::accent(), 0.95f));
    const bool pressed = ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f));
    ImGui::PopStyleColor(2);
    return pressed;
}

} // namespace nf::editor::uiw
