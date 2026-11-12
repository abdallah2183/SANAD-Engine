#include <NF/Editor/AddObjectIcon.hpp>
#include <NF/Editor/UiText.hpp>

#include <algorithm>
#include <cmath>

namespace nf::editor {

void draw_add_object_icon(ImDrawList& draw, AddObjectKind kind, ImVec2 center, float size) {
    if (size <= 0.0f) return;
    const auto p = [center, size](float x, float y) {
        return ImVec2(center.x + x * size, center.y + y * size);
    };
    const float line = std::max(1.2f, size * 0.025f);
    const ImU32 outline = IM_COL32(190, 215, 245, 220);
    const ImU32 blue = IM_COL32(74, 144, 224, 255);
    const ImU32 pale = IM_COL32(181, 222, 255, 255);
    const ImU32 gold = IM_COL32(255, 201, 93, 255);
    const ImU32 purple = IM_COL32(171, 141, 245, 255);
    const ImU32 teal = IM_COL32(88, 200, 185, 255);
    const ImU32 dark = IM_COL32(24, 35, 52, 255);

    switch (kind) {
    case AddObjectKind::Empty:
        draw.AddCircle(p(0, 0), size * 0.29f, IM_COL32(124, 153, 188, 120), 32, line);
        draw.AddLine(p(-0.21f, 0), p(0.21f, 0), outline, line * 1.4f);
        draw.AddLine(p(0, -0.21f), p(0, 0.21f), outline, line * 1.4f);
        draw.AddCircleFilled(p(0, 0), size * 0.055f, pale, 16);
        break;
    case AddObjectKind::Cube:
        draw.AddQuadFilled(p(0, -0.36f), p(0.34f, -0.19f), p(0, 0), p(-0.34f, -0.19f),
                           IM_COL32(146, 206, 255, 255));
        draw.AddQuadFilled(p(-0.34f, -0.19f), p(0, 0), p(0, 0.36f), p(-0.34f, 0.17f),
                           IM_COL32(63, 122, 190, 255));
        draw.AddQuadFilled(p(0, 0), p(0.34f, -0.19f), p(0.34f, 0.17f), p(0, 0.36f),
                           IM_COL32(45, 91, 155, 255));
        draw.AddLine(p(0, -0.36f), p(0.34f, -0.19f), pale, line);
        draw.AddLine(p(-0.34f, -0.19f), p(0, 0), pale, line);
        draw.AddLine(p(0, 0), p(0, 0.36f), IM_COL32(172, 213, 249, 170), line);
        break;
    case AddObjectKind::Sphere:
        draw.AddCircleFilled(p(0, 0), size * 0.34f, IM_COL32(47, 105, 171, 255), 48);
        draw.AddCircleFilled(p(-0.04f, -0.06f), size * 0.28f, blue, 48);
        draw.AddCircleFilled(p(-0.11f, -0.14f), size * 0.16f, IM_COL32(137, 203, 255, 150), 32);
        draw.AddCircleFilled(p(-0.15f, -0.18f), size * 0.055f, IM_COL32(225, 246, 255, 215), 20);
        draw.AddCircle(p(0, 0), size * 0.34f, outline, 48, line);
        break;
    case AddObjectKind::Quad:
        draw.AddQuadFilled(p(-0.35f, -0.22f), p(0.18f, -0.31f), p(0.36f, 0.20f),
                           p(-0.18f, 0.31f), IM_COL32(69, 140, 211, 220));
        draw.AddQuad(p(-0.35f, -0.22f), p(0.18f, -0.31f), p(0.36f, 0.20f),
                     p(-0.18f, 0.31f), pale, line);
        draw.AddLine(p(-0.27f, -0.12f), p(0.19f, -0.20f),
                     IM_COL32(208, 238, 255, 130), line);
        break;
    case AddObjectKind::Ground:
        draw.AddQuadFilled(p(-0.40f, 0.03f), p(0, -0.20f), p(0.40f, 0.03f), p(0, 0.28f),
                           IM_COL32(88, 169, 137, 255));
        draw.AddQuadFilled(p(-0.40f, 0.03f), p(0, 0.28f), p(0, 0.37f), p(-0.40f, 0.12f),
                           IM_COL32(50, 96, 86, 255));
        draw.AddQuadFilled(p(0, 0.28f), p(0.40f, 0.03f), p(0.40f, 0.12f), p(0, 0.37f),
                           IM_COL32(40, 80, 76, 255));
        draw.AddLine(p(-0.20f, -0.08f), p(0.20f, 0.16f), IM_COL32(180, 235, 204, 170), line);
        draw.AddLine(p(0.20f, -0.08f), p(-0.20f, 0.16f), IM_COL32(180, 235, 204, 170), line);
        break;
    case AddObjectKind::Sprite:
        draw.AddRectFilled(p(-0.33f, -0.29f), p(0.33f, 0.29f), IM_COL32(75, 95, 168, 255),
                           size * 0.055f);
        draw.AddRect(p(-0.33f, -0.29f), p(0.33f, 0.29f), IM_COL32(196, 197, 255, 240),
                     size * 0.055f, 0, line);
        draw.AddCircleFilled(p(0.14f, -0.12f), size * 0.07f, gold, 18);
        draw.AddTriangleFilled(p(-0.29f, 0.23f), p(-0.06f, -0.05f), p(0.20f, 0.23f),
                               IM_COL32(130, 211, 205, 255));
        draw.AddTriangleFilled(p(-0.02f, 0.23f), p(0.16f, 0.03f), p(0.29f, 0.23f),
                               IM_COL32(97, 166, 178, 255));
        break;
    case AddObjectKind::Tilemap:
    case AddObjectKind::RuleTile: {
        const float tile = size * 0.19f;
        const float gap = size * 0.025f;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                const ImVec2 top = p(-0.31f + static_cast<float>(col) * 0.215f,
                                     -0.31f + static_cast<float>(row) * 0.215f);
                const ImU32 color = (row + col) % 2 == 0
                    ? IM_COL32(147, 126, 219, 255) : IM_COL32(91, 81, 157, 255);
                draw.AddRectFilled(top, ImVec2(top.x + tile, top.y + tile), color, gap);
            }
        }
        if (kind == AddObjectKind::RuleTile) {
            draw.AddCircleFilled(p(0.26f, 0.26f), size * 0.15f, IM_COL32(40, 120, 105, 255), 24);
            draw.AddLine(p(0.18f, 0.26f), p(0.24f, 0.32f), IM_COL32(212, 255, 235, 255), line * 1.5f);
            draw.AddLine(p(0.24f, 0.32f), p(0.34f, 0.19f), IM_COL32(212, 255, 235, 255), line * 1.5f);
        }
        break;
    }
    case AddObjectKind::Camera:
        draw.AddQuadFilled(p(0.17f, -0.15f), p(0.37f, -0.27f), p(0.37f, 0.23f),
                           p(0.17f, 0.13f), IM_COL32(72, 106, 160, 255));
        draw.AddRectFilled(p(-0.36f, -0.25f), p(0.19f, 0.25f), IM_COL32(70, 108, 159, 255),
                           size * 0.07f);
        draw.AddRect(p(-0.36f, -0.25f), p(0.19f, 0.25f), outline, size * 0.07f, 0, line);
        draw.AddCircleFilled(p(-0.08f, 0), size * 0.17f, dark, 32);
        draw.AddCircleFilled(p(-0.08f, 0), size * 0.11f, IM_COL32(90, 177, 225, 255), 32);
        draw.AddCircleFilled(p(-0.12f, -0.05f), size * 0.04f, pale, 16);
        break;
    case AddObjectKind::Sun:
        draw.AddCircleFilled(p(0, 0), size * 0.35f, IM_COL32(255, 176, 59, 35), 32);
        for (int ray = 0; ray < 8; ++ray) {
            const float angle = static_cast<float>(ray) * 0.785398163f;
            const float x = std::cos(angle);
            const float y = std::sin(angle);
            draw.AddLine(p(x * 0.30f, y * 0.30f), p(x * 0.43f, y * 0.43f), gold, line * 1.5f);
        }
        draw.AddCircleFilled(p(0, 0), size * 0.23f, gold, 36);
        draw.AddCircleFilled(p(-0.07f, -0.07f), size * 0.09f, IM_COL32(255, 237, 177, 185), 24);
        break;
    case AddObjectKind::PointLight:
        draw.AddCircleFilled(p(0, -0.07f), size * 0.30f, IM_COL32(255, 196, 85, 40), 36);
        draw.AddCircleFilled(p(0, -0.09f), size * 0.20f, gold, 36);
        draw.AddRectFilled(p(-0.11f, 0.08f), p(0.11f, 0.25f), IM_COL32(233, 190, 104, 255),
                           size * 0.025f);
        draw.AddLine(p(-0.10f, 0.17f), p(0.10f, 0.17f), dark, line);
        draw.AddLine(p(-0.07f, 0.28f), p(0.07f, 0.28f), outline, line * 1.5f);
        draw.AddCircleFilled(p(-0.06f, -0.15f), size * 0.055f, IM_COL32(255, 248, 209, 225), 16);
        break;
    case AddObjectKind::SpotLight:
        draw.AddTriangleFilled(p(-0.02f, -0.12f), p(-0.34f, 0.34f), p(0.34f, 0.34f),
                               IM_COL32(255, 203, 102, 75));
        draw.AddQuadFilled(p(-0.17f, -0.32f), p(0.17f, -0.32f), p(0.10f, -0.08f),
                           p(-0.10f, -0.08f), IM_COL32(116, 139, 171, 255));
        draw.AddLine(p(-0.12f, -0.08f), p(0.12f, -0.08f), gold, line * 2.0f);
        draw.AddLine(p(-0.21f, 0.22f), p(0.21f, 0.22f), IM_COL32(255, 222, 163, 120), line);
        break;
    case AddObjectKind::Audio:
        draw.AddQuadFilled(p(-0.37f, -0.12f), p(-0.23f, -0.12f), p(-0.06f, -0.27f),
                           p(-0.06f, 0.27f), purple);
        draw.AddQuadFilled(p(-0.37f, -0.12f), p(-0.06f, 0.27f), p(-0.23f, 0.12f),
                           p(-0.37f, 0.12f), purple);
        draw.AddBezierCubic(p(0.02f, -0.17f), p(0.16f, -0.12f), p(0.16f, 0.12f),
                            p(0.02f, 0.17f), pale, line * 1.5f);
        draw.AddBezierCubic(p(0.12f, -0.29f), p(0.37f, -0.19f), p(0.37f, 0.19f),
                            p(0.12f, 0.29f), pale, line * 1.5f);
        break;
    case AddObjectKind::Sky:
        draw.AddCircleFilled(p(0, 0), size * 0.34f, IM_COL32(63, 131, 194, 255), 40);
        draw.AddCircleFilled(p(0.12f, -0.12f), size * 0.08f, gold, 20);
        draw.AddTriangleFilled(p(-0.30f, 0.15f), p(-0.09f, -0.06f), p(0.12f, 0.25f), teal);
        draw.AddTriangleFilled(p(-0.10f, 0.25f), p(0.13f, 0.02f), p(0.30f, 0.15f),
                               IM_COL32(145, 219, 207, 255));
        draw.AddLine(p(-0.24f, 0.24f), p(0.24f, 0.24f), IM_COL32(204, 241, 238, 180), line);
        break;
    case AddObjectKind::Weather:
        draw.AddCircleFilled(p(-0.14f, -0.09f), size * 0.15f, IM_COL32(183, 204, 226, 255), 24);
        draw.AddCircleFilled(p(0.04f, -0.16f), size * 0.20f, IM_COL32(205, 222, 238, 255), 28);
        draw.AddCircleFilled(p(0.22f, -0.08f), size * 0.14f, IM_COL32(183, 204, 226, 255), 24);
        draw.AddRectFilled(p(-0.27f, -0.08f), p(0.32f, 0.04f), IM_COL32(183, 204, 226, 255),
                           size * 0.04f);
        for (int drop = -1; drop <= 1; ++drop) {
            const float x = static_cast<float>(drop) * 0.17f;
            draw.AddLine(p(x + 0.03f, 0.12f), p(x - 0.03f, 0.28f), blue, line * 1.5f);
        }
        break;
    case AddObjectKind::Cable:
        draw.AddBezierCubic(p(-0.32f, -0.20f), p(-0.32f, 0.36f), p(0.32f, 0.36f),
                            p(0.32f, -0.20f), IM_COL32(107, 176, 229, 255), line * 2.6f);
        draw.AddBezierCubic(p(-0.32f, -0.20f), p(-0.32f, 0.36f), p(0.32f, 0.36f),
                            p(0.32f, -0.20f), pale, line * 0.8f);
        draw.AddCircleFilled(p(-0.32f, -0.20f), size * 0.07f, gold, 20);
        draw.AddCircleFilled(p(0.32f, -0.20f), size * 0.07f, gold, 20);
        break;
    case AddObjectKind::FlowField:
        for (int row = -1; row <= 1; ++row) {
            const float y = static_cast<float>(row) * 0.21f;
            draw.AddBezierCubic(p(-0.35f, y + 0.08f), p(-0.13f, y + 0.08f),
                                p(0.08f, y - 0.08f), p(0.27f, y - 0.08f), teal, line * 1.8f);
            draw.AddTriangleFilled(p(0.39f, y - 0.08f), p(0.23f, y - 0.17f),
                                   p(0.23f, y + 0.01f), teal);
        }
        break;
    case AddObjectKind::Talent:
        draw.AddLine(p(0, -0.24f), p(0, 0.02f), outline, line * 1.5f);
        draw.AddLine(p(-0.25f, 0.13f), p(0.25f, 0.13f), outline, line * 1.5f);
        draw.AddLine(p(-0.25f, 0.13f), p(-0.25f, 0.24f), outline, line * 1.5f);
        draw.AddLine(p(0, 0.02f), p(0, 0.24f), outline, line * 1.5f);
        draw.AddLine(p(0.25f, 0.13f), p(0.25f, 0.24f), outline, line * 1.5f);
        draw.AddCircleFilled(p(0, -0.25f), size * 0.11f, gold, 24);
        draw.AddCircleFilled(p(-0.25f, 0.25f), size * 0.10f, blue, 24);
        draw.AddCircleFilled(p(0, 0.25f), size * 0.10f, purple, 24);
        draw.AddCircleFilled(p(0.25f, 0.25f), size * 0.10f, teal, 24);
        break;
    }
}

void draw_add_object_card(const AddObjectEntry& entry, int index, float width, int& selected,
                          bool& create_requested, bool search_focused, bool keyboard_moved) {
    constexpr float height = 122.0f;
    ImGui::PushID(index);
    const bool activated = ImGui::InvisibleButton("##object_card", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (activated || (focused && ImGui::GetIO().NavVisible && !search_focused)) selected = index;
    if ((hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ||
        (focused && !search_focused && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
        selected = index;
        create_requested = true;
    }
    if (keyboard_moved && selected == index) ImGui::SetScrollHereY();

    const bool active = selected == index;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, active ? IM_COL32(40, 65, 94, 255)
                                       : hovered ? IM_COL32(40, 50, 64, 255)
                                                 : IM_COL32(30, 38, 49, 255), 8.0f);
    draw->AddRect(min, max, active ? IM_COL32(104, 172, 238, 255)
                                  : hovered ? IM_COL32(91, 119, 151, 220)
                                            : IM_COL32(63, 76, 92, 180), 8.0f, 0, active ? 2.0f : 1.0f);
    const ImVec2 icon_center((min.x + max.x) * 0.5f, min.y + 43.0f);
    draw->AddCircleFilled(icon_center, 36.0f, IM_COL32(20, 29, 42, 110), 36);
    draw_add_object_icon(*draw, entry.kind, icon_center, 78.0f);

    const std::string label = AV(entry.label_key);
    const float label_width = std::max(1.0f, width - 14.0f);
    const ImVec2 text_size = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), 10000.0f,
                                                            label_width, label.c_str());
    const ImVec2 text_pos(min.x + std::max(7.0f, (width - text_size.x) * 0.5f), min.y + 84.0f);
    const ImVec4 clip(min.x + 5.0f, min.y + 80.0f, max.x - 5.0f, max.y - 4.0f);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), text_pos, IM_COL32(229, 237, 247, 255),
                  label.c_str(), nullptr, label_width, &clip);
    ImGui::PopID();
}

} // namespace nf::editor
