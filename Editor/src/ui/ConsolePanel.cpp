#include <NF/Editor/ConsolePanel.hpp>
#include <NF/Editor/UiText.hpp>
#include <NF/Editor/UiTextInput.hpp>
#include <NF/Editor/UiTheme.hpp>
#include <NF/UI/ArabicShaper.hpp>

#include <imgui.h>

#include <algorithm>

namespace nf::editor {
namespace {

ImVec4 severity_color(LogLevel level) {
    if (level >= LogLevel::Error) return theme::danger();
    if (level == LogLevel::Warn) return theme::warning();
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

// Start a new line when another control would leave the dock's visible area.
void next_control(float width) {
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right)
        ImGui::SameLine();
}

float button_width(const std::string& label) {
    return ImGui::CalcTextSize(label.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
}

} // namespace

void console_panel(ConsoleBuffer& buffer, ConsolePanelState& state) {
    const std::string all = AV("filter_all");
    const char* categories[] = {all.c_str(), "Core", "Render", "Physics", "Audio", "Network",
                               "Asset", "Editor", "Script", "ECS", "Platform", "RHI", "Jobs", "Scene"};
    constexpr LogCategory category_values[] = {
        LogCategory::All, LogCategory::Core, LogCategory::Render, LogCategory::Physics,
        LogCategory::Audio, LogCategory::Network, LogCategory::Asset, LogCategory::Editor,
        LogCategory::Script, LogCategory::ECS, LogCategory::Platform, LogCategory::RHI,
        LogCategory::Jobs, LogCategory::Scene};
    state.category = std::clamp(state.category, 0, IM_ARRAYSIZE(category_values) - 1);
    state.min_level = std::clamp(state.min_level, 0, 4);
    const float panel_width = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::min(110.0f, panel_width));
    ImGui::Combo("##consolecategory", &state.category, categories, IM_ARRAYSIZE(categories));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", AV("category").c_str());
    const std::string scroll_label = AV("autoscroll") + "###console_autoscroll";
    const float scroll_width = ImGui::CalcTextSize(scroll_label.c_str(), nullptr, true).x +
                               ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    next_control(std::min(180.0f, panel_width));
    float search_width = ImGui::GetContentRegionAvail().x;
    if (search_width >= 100.0f + scroll_width + ImGui::GetStyle().ItemSpacing.x)
        search_width -= scroll_width + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(std::max(1.0f, search_width));
    input_text_with_hint("##consolesearch", AV("console_search_hint").c_str(), state.search);
    next_control(scroll_width);
    ImGui::Checkbox(scroll_label.c_str(), &state.auto_scroll);

    ConsoleBuffer::Filter filter{static_cast<LogLevel>(state.min_level),
                                 category_values[state.category], state.search};
    auto snapshot = buffer.snapshot(filter);
    const char* keys[] = {"filter_all", "console_info", "console_warn", "console_error"};
    const int floors[] = {0, 2, 3, 4};
    for (int i = 0; i < IM_ARRAYSIZE(floors); ++i) {
        const int floor = floors[i];
        const std::string label = AV(keys[i]) + (floor == 0 ? " (" : "+ (") +
                                  std::to_string(snapshot.level_counts[floor]) + ")###console_level_" +
                                  std::to_string(floor);
        if (i != 0) next_control(button_width(label));
        const bool selected = state.min_level == floor;
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label.c_str())) state.min_level = floor;
        if (selected) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", AVF("console_visible_fmt",
            snapshot.lines.size(), snapshot.total, snapshot.dropped).c_str());
    }
    const std::string copy_label = AV("copy_visible_logs") + "###console_copy";
    next_control(button_width(copy_label));
    const bool copy = ImGui::Button(copy_label.c_str());
    const std::string clear_label = AV("clear") + "###console_clear";
    next_control(button_width(clear_label));
    const bool clear = ImGui::Button(clear_label.c_str());
    if (clear) buffer.clear();
    // Clicking a pill and typing both affect the displayed/copied rows this frame.
    if (clear || filter.min_level != static_cast<LogLevel>(state.min_level)) {
        filter.min_level = static_cast<LogLevel>(state.min_level);
        snapshot = buffer.snapshot(filter);
    }
    if (copy) ImGui::SetClipboardText(format_console_lines(snapshot.lines).c_str());
    // Keep the message area usable in the shallow default bottom dock.
    // The same counts are always available on the severity-button tooltips.
    if (ImGui::GetContentRegionAvail().y > ImGui::GetTextLineHeightWithSpacing() * 6) {
        ImGui::TextDisabled("%s", AVF("console_visible_fmt", snapshot.lines.size(), snapshot.total,
                                      snapshot.dropped).c_str());
    }
    ImGui::Separator();

    bool open_context = false;
    if (ImGui::BeginChild("##consolelines", ImVec2(0, 0), ImGuiChildFlags_Borders,
                           ImGuiWindowFlags_HorizontalScrollbar)) {
        const bool follow_tail = state.first_frame || ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        if (snapshot.lines.empty()) {
            ImGui::TextDisabled("%s", AV(snapshot.total == 0 ? "console_empty" : "console_no_matches").c_str());
        }
        // Messages can contain line breaks. A fixed-height list clipper skips
        // the wrong rows for those logs; the bounded buffer holds at most 500.
        for (const auto& message : snapshot.lines) {
            const ImVec4 color = severity_color(message.level);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetTextLineHeight();
            ImGui::Dummy(ImVec2(10.0f, h));
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 4, p.y + h * 0.5f),
                                                       3.0f, ImGui::GetColorU32(color));
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", console_category_name(message.category));
            ImGui::SameLine();
            const std::string shaped = ui::shape_arabic(message.text);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(shaped.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                state.context_message = message.text;
                open_context = true;
            }
        }
        // One popup owns a copy of the clicked message. It stays correct even
        // when new messages evict rows or change the active query behind it.
        if (open_context) ImGui::OpenPopup("##console_message");
        if (ImGui::BeginPopup("##console_message")) {
            if (ImGui::MenuItem(AV("copy_log_message").c_str()))
                ImGui::SetClipboardText(state.context_message.c_str());
            ImGui::EndPopup();
        }
        if (state.auto_scroll && follow_tail && !snapshot.lines.empty()) ImGui::SetScrollHereY(1.0f);
        state.first_frame = false;
    }
    ImGui::EndChild();
}

} // namespace nf::editor
