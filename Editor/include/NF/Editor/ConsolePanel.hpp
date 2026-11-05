#pragma once

#include <NF/Editor/Console.hpp>

namespace nf::editor {

struct ConsolePanelState {
    int min_level = 2;
    int category = 0;
    std::string search;
    bool auto_scroll = true;
    bool first_frame = true;
    std::string context_message;
};

// Record inside the Console window. Kept window/device-free so interaction
// tests can exercise the same widgets without starting the engine.
void console_panel(ConsoleBuffer& buffer, ConsolePanelState& state);

} // namespace nf::editor
