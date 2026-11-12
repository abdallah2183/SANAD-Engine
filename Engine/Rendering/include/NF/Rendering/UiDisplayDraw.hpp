#pragma once
// NF/Rendering/UiDisplayDraw.hpp — shared DisplayList drawing for NFPlayer and
// the editor Game View. Both call draw_display_list on their own ImGui
// context, so text measurement, shaping and styling cannot drift between the
// shipped player and the editor preview. The caller owns the ImGui context
// (NewFrame/Render, draw list, font) — this only records commands.

#include <NF/UI/TextLayout.hpp>
#include <imgui.h>
#include <NF/UI/Widgets.hpp>

struct ImDrawList;
struct ImFont;

namespace nf::rendering {

/// Draws a ui::DisplayList into `dl`, offset by `offset` (the game content
/// origin in the draw list's coordinate space). Coordinates in the list are
/// pixels for the game viewport; `offset` places them on the screen (editor
/// Game View) or at (0,0) (NFPlayer fullscreen). `font` is the ImGui font to
/// measure/draw with; `measurer` must be the same measurement the canvas
/// sized tooltips/carets with, or wrap and box disagree.
void draw_display_list(ImDrawList* dl, const ui::DisplayList& list, ImVec2 offset,
                       ImFont* font, float default_font_size,
                       const ui::TextMeasurer& measurer);

} // namespace nf::rendering
