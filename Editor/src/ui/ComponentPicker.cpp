#include <NF/Editor/ComponentPicker.hpp>
#include <NF/Editor/UiPanelControls.hpp>
#include <algorithm>

namespace nf::editor {
std::optional<ComponentKind> component_picker_popup(ComponentPicker& picker, const ecs::World* world,
                                                     ecs::Entity entity, uint64_t scene) {
    std::optional<ComponentKind> choice;
    const auto* viewport = ImGui::GetMainViewport();
    const float width = std::max(1.0f, std::min(420.0f, viewport->WorkSize.x - 24));
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(width, viewport->WorkSize.y - 16));
    if (ImGui::BeginPopup("##add_component_menu")) {
        if (!world || world != picker.world || entity != picker.entity || scene != picker.scene ||
            !world->is_alive(entity)) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextWrapped("%s", AV("component_picker_hint").c_str());
            if (picker.focus_search) { ImGui::SetKeyboardFocusHere(); picker.focus_search = false; }
            uiw::search_field("component_search", AV("search_components"), picker.query);
            constexpr const char* groups[] = {"filter_all", "rendering", "component_group_2d",
                                             "filter_physics", "palette_world", "gameplay"};
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##component_group", AV(groups[picker.group + 1]).c_str())) {
                for (int index = 0; index < 6; ++index) {
                    if (ImGui::Selectable(AV(groups[index]).c_str(), picker.group == index - 1)) picker.group = index - 1;
                }
                ImGui::EndCombo();
            }
            ImGui::Checkbox(AV("show_attached_components").c_str(), &picker.show_attached);
            ImGui::Separator();
            picker.visible = 0;
            ImGui::BeginChild("##component_list", ImVec2(0, std::min(320.0f, viewport->WorkSize.y * .45f)));
            for (const auto& entry : kComponents) {
                if (picker.group >= 0 && static_cast<int>(entry.group) != picker.group) continue;
                const bool attached = has_component(*world, entity, entry.kind);
                if ((attached && !picker.show_attached) || !component_entry_matches(entry, picker.query)) continue;
                ++picker.visible;
                const std::string label = AV(entry.label_key) + (attached ? "  (" + AV("component_attached") + ")" : "") +
                                          "###component_" + std::to_string(static_cast<int>(entry.kind));
                ImGui::BeginDisabled(attached);
                if (ImGui::Selectable(label.c_str())) choice = entry.kind;
                ImGui::EndDisabled();
            }
            if (!picker.visible) ImGui::TextWrapped("%s", AV("component_picker_empty").c_str());
            ImGui::EndChild();
            if (choice) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return choice;
}
} // namespace nf::editor
