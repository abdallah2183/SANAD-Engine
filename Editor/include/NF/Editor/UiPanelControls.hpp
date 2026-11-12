#pragma once

#include <NF/Editor/UiText.hpp>
#include <NF/Editor/UiTextInput.hpp>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace nf::editor::uiw {

// Wrap action rows using the actual translated button width.
inline void next_action(const std::string& label) {
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const float width = ImGui::CalcTextSize(label.c_str(), nullptr, true).x +
                        ImGui::GetStyle().FramePadding.x * 2;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right)
        ImGui::SameLine();
}

inline bool search_field(const char* id, const std::string& hint, std::string& query) {
    ImGui::PushID(id);
    const float button = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - button -
                                           ImGui::GetStyle().ItemSpacing.x));
    bool changed = input_text_with_hint("##query", hint.c_str(), query);
    ImGui::SameLine();
    ImGui::BeginDisabled(query.empty());
    if (ImGui::Button("X##clear", ImVec2(button, 0))) {
        query.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", AV("clear_search").c_str());
    ImGui::PopID();
    return changed;
}

inline std::string ascii_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// All words must match; logical Arabic and English are searchable in either UI language.
inline bool component_matches(const std::string& query, const char* key) {
    const std::string hay = ascii_lower(std::string(key) + " " +
        ui::tr_for_language(key, ui::Language::English) + " " +
        ui::tr_for_language(key, ui::Language::Arabic));
    const std::string words = ascii_lower(query);
    size_t at = 0;
    while (at < words.size()) {
        at = words.find_first_not_of(" \t\r\n", at);
        if (at == std::string::npos) break;
        const size_t end = words.find_first_of(" \t\r\n", at);
        if (hay.find(words.substr(at, end - at)) == std::string::npos) return false;
        at = end;
    }
    return true;
}

struct InspectorSections {
    std::string query;
    int bulk_open = -1;
    unsigned matches = 0;

    void begin_frame() { matches = 0; bulk_open = -1; }
    bool header(const char* key, const char* stable_id, ImGuiTreeNodeFlags flags = 0) {
        if (!component_matches(query, key)) return false;
        ++matches;
        const bool searching = query.find_first_not_of(" \t\r\n") != std::string::npos;
        // Separate storage means searching cannot overwrite the user's folded sections.
        const std::string label = AV(key) + "###" + (searching ? "search_" : "") + stable_id;
        if (searching || bulk_open >= 0)
            ImGui::SetNextItemOpen(searching || bulk_open != 0, ImGuiCond_Always);
        return ImGui::CollapsingHeader(label.c_str(), flags);
    }
};

// Property names such as Enabled, Color and Type repeat across components.
// Scope their IDs to the component, while leaving the header ID language-stable.
struct InspectorSection {
    InspectorSection(InspectorSections& sections, const char* key, const char* stable_id,
                     ImGuiTreeNodeFlags flags = 0, bool available = true)
        : opened(available && sections.header(key, stable_id, flags)) {
        if (opened) ImGui::PushID(stable_id);
    }
    ~InspectorSection() { if (opened) ImGui::PopID(); }
    explicit operator bool() const { return opened; }
    InspectorSection(const InspectorSection&) = delete;
    InspectorSection& operator=(const InspectorSection&) = delete;
private:
    bool opened = false;
};

struct VectorEditResult { bool changed = false; bool finished = false; };

inline VectorEditResult vector3_control(const char* label, float values[3], float reset_value,
                                       float speed, float minimum = 0, float maximum = 0,
                                       const char* format = "%.2f") {
    VectorEditResult result;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    const float width = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float button = ImGui::GetFrameHeight();
    const bool stacked = width < 3 * (button + 36 + spacing) + 2 * spacing;
    const float cell = stacked ? width : (width - 2 * spacing) / 3;
    const char* axes[] = {"X", "Y", "Z"};
    const ImVec4 colors[] = {{0.98f, .44f, .44f, 1}, {.20f, .83f, .60f, 1}, {.38f, .65f, .98f, 1}};
    for (int axis = 0; axis < 3; ++axis) {
        if (axis && !stacked) ImGui::SameLine();
        ImGui::PushID(axis);
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, colors[axis]);
        if (ImGui::Button(axes[axis], ImVec2(button, 0))) {
            values[axis] = reset_value;
            result.changed = result.finished = true;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", AV("reset_axis").c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(1.0f, cell - button - spacing));
        result.changed |= ImGui::DragFloat("##value", &values[axis], speed, minimum, maximum, format);
        result.finished |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::PopID();
    return result;
}
} // namespace nf::editor::uiw
