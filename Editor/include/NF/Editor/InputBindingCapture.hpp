#pragma once
#include <NF/Runtime/InputBindings.hpp>
#include <imgui.h>
#include <string>

namespace nf::editor {

struct InputBindingCapture {
    std::string action;
    bool requested = false;
    int started_frame = 0;
    void begin(const std::string& name);
};

bool binding_action_matches(const std::string& action, const std::string& search);
void input_binding_capture_popup(InputBindingCapture& state, runtime::InputBindings& bindings);

} // namespace nf::editor
