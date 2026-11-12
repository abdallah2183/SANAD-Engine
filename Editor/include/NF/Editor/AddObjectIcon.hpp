#pragma once

#include <NF/Editor/AddObjectCatalog.hpp>
#include <imgui.h>

namespace nf::editor {

// Draw an object illustration at the requested size in screen coordinates.
void draw_add_object_icon(ImDrawList& draw, AddObjectKind kind, ImVec2 center, float size);
void draw_add_object_card(const AddObjectEntry& entry, int index, float width, int& selected,
                          bool& create_requested, bool search_focused, bool keyboard_moved);

} // namespace nf::editor
