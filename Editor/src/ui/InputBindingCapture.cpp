#include <NF/Editor/InputBindingCapture.hpp>
#include <NF/Editor/UiText.hpp>
#include <algorithm>
#include <cctype>

namespace nf::editor {
namespace {
bool imgui_key_to_vk(ImGuiKey key, nf::u16& out) {
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
        out = static_cast<nf::u16>('A' + (key - ImGuiKey_A));
        return true;
    }
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
        out = static_cast<nf::u16>('0' + (key - ImGuiKey_0));
        return true;
    }
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) {
        out = static_cast<nf::u16>(0x70 + (key - ImGuiKey_F1));
        return true;
    }
    switch (key) {
    case ImGuiKey_Space: out = 0x20; return true;
    case ImGuiKey_Enter: out = 0x0D; return true;
    case ImGuiKey_Tab: out = 0x09; return true;
    case ImGuiKey_Backspace: out = 0x08; return true;
    case ImGuiKey_Escape: out = 0x1B; return true;
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift: out = 0x10; return true;
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl: out = 0x11; return true;
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt: out = 0x12; return true;
    case ImGuiKey_UpArrow: out = 0x26; return true;
    case ImGuiKey_DownArrow: out = 0x28; return true;
    case ImGuiKey_LeftArrow: out = 0x25; return true;
    case ImGuiKey_RightArrow: out = 0x27; return true;
    case ImGuiKey_Delete: out = 0x2E; return true;
    case ImGuiKey_Insert: out = 0x2D; return true;
    case ImGuiKey_Home: out = 0x24; return true;
    case ImGuiKey_End: out = 0x23; return true;
    case ImGuiKey_PageUp: out = 0x21; return true;
    case ImGuiKey_PageDown: out = 0x22; return true;
    case ImGuiKey_Minus: out = 0xBD; return true;
    case ImGuiKey_Equal: out = 0xBB; return true;
    case ImGuiKey_Comma: out = 0xBC; return true;
    case ImGuiKey_Period: out = 0xBE; return true;
    case ImGuiKey_Slash: out = 0xBF; return true;
    case ImGuiKey_Semicolon: out = 0xBA; return true;
    case ImGuiKey_Apostrophe: out = 0xDE; return true;
    case ImGuiKey_LeftBracket: out = 0xDB; return true;
    case ImGuiKey_RightBracket: out = 0xDD; return true;
    case ImGuiKey_Backslash: out = 0xDC; return true;
    case ImGuiKey_GraveAccent: out = 0xC0; return true;
    default: return false;
    }
}

} // namespace

bool binding_action_matches(const std::string& action, const std::string& search) {
    return std::search(action.begin(), action.end(), search.begin(), search.end(),
        [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }) != action.end()
        || search.empty();
}

void InputBindingCapture::begin(const std::string& name) {
    action = name;
    requested = true;
    started_frame = ImGui::GetFrameCount();
}

void input_binding_capture_popup(InputBindingCapture& state, runtime::InputBindings& bindings) {
    const auto title = AV("rebind") + "###NFBindingCapture";
    if (state.requested) {
        ImGui::OpenPopup(title.c_str());
        state.requested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(state.action.c_str());
        ImGui::TextWrapped("%s", AV("press_a_key").c_str());
        ImGui::Button((AV("bindings_mouse_area") + "###capture_mouse").c_str(), ImVec2(-1, 60));
        int mouse = -1;
        for (int i = 0; i < 3; ++i) if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(i)) mouse = i;
        const bool cancel = ImGui::Button((AV("cancel") + "###capture_cancel").c_str()) ||
                            ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        runtime::InputBinding binding{};
        bool captured = false;
        if (!cancel && ImGui::GetFrameCount() > state.started_frame) {
            if (mouse >= 0) {
                constexpr nf::u16 codes[] = {0x01, 0x02, 0x04};
                binding = {runtime::BindingSource::MouseButton, codes[mouse], 1.0f};
                captured = true;
            } else if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
                    nf::u16 code = 0;
                    if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false) &&
                        imgui_key_to_vk(static_cast<ImGuiKey>(k), code)) {
                        binding = {runtime::BindingSource::Key, code, 1.0f};
                        captured = true;
                        break;
                    }
                }
            }
        }
        if (captured) bindings.set(state.action, {binding});
        if (cancel || captured) {
            state.action.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (!ImGui::IsPopupOpen(title.c_str())) {
        state.action.clear();
    }
}

} // namespace nf::editor
