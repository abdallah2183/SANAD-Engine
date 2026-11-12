#pragma once

#include <NF/Editor/UiText.hpp>
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cstdint>

namespace nf::editor::uiw {

// The upper mask bits are category membership, not additional system badges.
inline unsigned outliner_badge_count(uint16_t mask) {
    return std::popcount(static_cast<unsigned>(mask & 0xff));
}

inline std::string outliner_badge_description(uint16_t mask) {
    constexpr const char* keys[] = {"filter_weather", "filter_cable", "rule_tile", "filter_nav",
                                    "filter_talents", "inventory", "ragdoll", "surface_audio"};
    std::string text;
    for (unsigned bit = 0; bit < 8; ++bit) {
        if (!(mask & (1u << bit))) continue;
        if (!text.empty()) text += '\n';
        text += AV(keys[bit]); // AV already translates and shapes Arabic exactly once.
    }
    return text;
}

// A drawn badge must never submit an item: selection, context menus and drag/drop
// still belong to the tree row. Omit the badge when a long name needs its space.
inline bool draw_outliner_badge(uint16_t mask, ImVec2 row_min, ImVec2 row_max, float label_end) {
    const unsigned count = outliner_badge_count(mask);
    const float height = row_max.y - row_min.y;
    if (!count || height < 8) return false;
    const std::string text = "+" + std::to_string(count);
    const float font_size = std::min(ImGui::GetFontSize(), height - 4);
    const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0, text.c_str());
    const float right = row_max.x - 4;
    const float left = right - size.x - 10;
    if (left < std::max(row_min.x, label_end) + 8) return false;
    const ImVec2 minimum(left, row_min.y + 1);
    const ImVec2 maximum(right, row_max.y - 1);
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(row_min, row_max, true);
    draw->AddRectFilled(minimum, maximum, IM_COL32(23, 37, 84, 210), 3);
    draw->AddRect(minimum, maximum, IM_COL32(59, 130, 246, 140), 3);
    draw->AddText(ImGui::GetFont(), font_size,
                  ImVec2(left + 5, row_min.y + (height - size.y) * .5f),
                  IM_COL32(191, 219, 254, 255), text.c_str());
    draw->PopClipRect();
    return true;
}
} // namespace nf::editor::uiw
