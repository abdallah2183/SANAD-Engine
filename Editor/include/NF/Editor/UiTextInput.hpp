#pragma once

// NF/Editor/UiTextInput.hpp — a std::string-backed ImGui text field.
//
// ImGui's text field writes into a caller-owned FIXED buffer, and the value
// it hands back is whatever fits: a longer string is cut at the buffer's end
// and the rest is silently lost. The FileSystem dock's filter field used a
// char[128], so a 200-character query became 127 characters and the list
// filtered by a query the user never typed. The rename modal had the same
// buffer, and there the cut value became the DESTINATION FILE NAME.
//
// The resize callback is ImGui's own answer to that: the widget asks for
// more room through the callback, and the string grows instead of the text
// being cut. The buffer handed to ImGui is a per-frame copy of the value —
// ImGui owns the text while the widget is active and reloads from the user
// buffer when it is not — so the string's own storage is never aliased by
// the widget across frames.

#include <imgui.h>

#include <cstring>
#include <string>

namespace nf::editor {

// Grows the string behind an InputText when the widget needs more room.
inline int input_text_resize_callback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackResize) {
        return 0;
    }
    auto* text = static_cast<std::string*>(data->UserData);
    if (text == nullptr) {
        return 0;
    }
    // ImGui asks for BufSize bytes (the text plus its terminator). Growing
    // the string keeps the text at the front and only zeroes the tail, which
    // is exactly the room the widget asked for.
    const std::size_t want =
        (data->BufSize > 0) ? static_cast<std::size_t>(data->BufSize) : text->size() + 1;
    if (text->size() < want) {
        text->resize(want);
    }
    data->Buf = text->data();
    data->BufSize = static_cast<int>(text->size());
    return 0;
}

// An InputText bound to `value`. Returns true when the text changed.
//
// The value is updated in place; the widget's buffer is a copy, so the
// string's length is re-synced from the buffer's contents after the call
// (ImGui writes a NUL-terminated string and never touches the length).
inline bool input_text(const char* label, std::string& value, ImGuiInputTextFlags flags = 0) {
    std::string buf = value;
    buf.resize(buf.size() + 64); // room to type into before the first grow
    flags |= ImGuiInputTextFlags_CallbackResize;
    const bool changed = ImGui::InputText(label, buf.data(), buf.capacity(), flags,
                                          &input_text_resize_callback, &buf);
    value.assign(buf.c_str());
    return changed;
}

// The same, with a placeholder shown while the field is empty.
inline bool input_text_with_hint(const char* label, const char* hint, std::string& value,
                                 ImGuiInputTextFlags flags = 0) {
    std::string buf = value;
    buf.resize(buf.size() + 64);
    flags |= ImGuiInputTextFlags_CallbackResize;
    const bool changed = ImGui::InputTextWithHint(label, hint, buf.data(), buf.capacity(), flags,
                                                   &input_text_resize_callback, &buf);
    value.assign(buf.c_str());
    return changed;
}

} // namespace nf::editor
