#pragma once

#include <NF/Editor/Outliner.hpp>
#include <NF/Editor/OutlinerBadges.hpp>
#include <NF/Editor/UiPathText.hpp>
#include <cstring>

namespace nf::editor::uiw {

inline std::string fit_outliner_text(std::string text, float width) {
    if (ImGui::CalcTextSize(text.c_str(), nullptr, false).x <= width) return text;
    constexpr const char* ellipsis = "...";
    if (width < ImGui::CalcTextSize(ellipsis).x) return {};
    while (!text.empty()) {
        remove_last_utf8_character(text);
        if (ImGui::CalcTextSize((text + ellipsis).c_str(), nullptr, false).x <= width)
            return text + ellipsis;
    }
    return ellipsis;
}

// DFS rows are clipped independently, so each row supplies its own indentation
// and never pushes a TreeNode scope for descendants that may be off screen.
struct OutlinerRowUi {
    ImVec2 minimum{}, maximum{};
    std::string full_text, visible_text;
    bool hovered = false, clipped = false;
    float label_end = 0;

    OutlinerRowUi(const OutlinerRow& row, OutlinerState& state, bool selected, float icon_gutter = 23) {
        ImGui::PushID(static_cast<int>(row.entity.id));
        indent = std::min(std::max(0, row.depth) * ImGui::GetStyle().IndentSpacing,
                          std::max(0.0f, ImGui::GetContentRegionAvail().x - 100));
        if (indent > 0) ImGui::Indent(indent);
        auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                     ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (selected) flags |= ImGuiTreeNodeFlags_Selected;
        if (!row.has_children) flags |= ImGuiTreeNodeFlags_Leaf;
        else ImGui::SetNextItemOpen(state.is_expanded(row.entity), ImGuiCond_Always);
        const std::string id = "###e" + std::to_string(row.entity.id);
        const bool open = ImGui::TreeNodeEx(id.c_str(), flags);
        if (row.has_children) state.set_expanded(row.entity, open);
        hovered = ImGui::IsItemHovered();
        minimum = ImGui::GetItemRectMin();
        maximum = ImGui::GetItemRectMax();
        full_text = ui::shape_arabic(row.is_prefab ? ui::tr("prefab") + ": " + row.label : row.label);
        std::string single_line = full_text;
        for (char& character : single_line)
            if (character == '\n' || character == '\r' || character == '\t') character = ' ';
        const float text_x = minimum.x + ImGui::GetTreeNodeToLabelSpacing() + icon_gutter;
        const float font_size = std::max(1.0f, std::min(ImGui::GetFontSize(), maximum.y - minimum.y - 4));
        const float badge_width = outliner_badge_count(row.feature_mask)
            ? ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0, "+8").x + 26 : 0;
        visible_text = fit_outliner_text(single_line, maximum.x - text_x - badge_width - 4);
        clipped = visible_text != single_line;
        label_end = text_x + ImGui::CalcTextSize(visible_text.c_str(), nullptr, false).x;
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(minimum, maximum, true);
        draw->AddText(ImVec2(text_x, minimum.y), ImGui::GetColorU32(ImGuiCol_Text), visible_text.c_str());
        draw->PopClipRect();
        draw_outliner_badge(row.feature_mask, minimum, maximum, label_end);
    }

    ~OutlinerRowUi() {
        if (indent > 0) ImGui::Unindent(indent);
        ImGui::PopID();
    }
    OutlinerRowUi(const OutlinerRowUi&) = delete;
    OutlinerRowUi& operator=(const OutlinerRowUi&) = delete;

    // Draw after all row interactions so tooltip widgets cannot steal the row item.
    void tooltip(uint16_t mask) const {
        if (!hovered || ImGui::IsMouseDown(ImGuiMouseButton_Left) || (!clipped && !outliner_badge_count(mask)))
            return;
        std::string text = full_text;
        const auto systems = outliner_badge_description(mask);
        if (!systems.empty()) text += "\n\n" + systems;
        ImGui::SetTooltip("%s", text.c_str());
    }

private:
    float indent = 0;
};

inline bool outliner_root_drop_target(ecs::Entity& dropped) {
    // A real empty-space item prevents the last visible row from accepting a
    // second, conflicting drop that would immediately unparent the entity.
    const auto available = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(std::max(1.0f, available.x), std::max(20.0f, available.y)));
    bool delivered = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("NF_ENTITY")) {
            if (payload->DataSize == sizeof(ecs::Entity)) {
                std::memcpy(&dropped, payload->Data, sizeof(dropped));
                delivered = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    return delivered;
}
} // namespace nf::editor::uiw
