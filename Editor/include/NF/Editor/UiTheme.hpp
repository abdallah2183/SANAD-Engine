#pragma once

// NF/Editor/UiTheme.hpp — the editor's semantic colour layer.
//
// UiBackend.cpp owns the ImGui *style* (the global palette every stock widget
// reads). This header owns the handful of colours a panel needs to pick by
// MEANING rather than by palette slot: "this is a destructive action", "this
// number is healthy", "this text is secondary". Before this existed those were
// re-derived as magic ImVec4 literals at every call site, so the play button's
// red and the console's error red were two different reds that drifted apart.
//
// Header-only and dependency-light on purpose: Panels.cpp and Toolbar.cpp both
// include it, and neither should drag in the settings object to ask what colour
// "warning" is.

#include <imgui.h>

namespace nf::editor::theme {

/// 8-bit sRGB triple to an ImVec4, the way a designer writes a hex code.
inline ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                  static_cast<float>(b) / 255.0f, a);
}

// --- Semantic roles ---------------------------------------------------------
// Deliberately NOT the accent: a destructive button must read as destructive
// whatever the user picked for the accent colour.

/// Destructive / stop / error. (#E5484D)
inline ImVec4 danger() { return rgb(0xE5, 0x48, 0x4D); }
/// Confirm / success / "no problems". (#46A758)
inline ImVec4 success() { return rgb(0x46, 0xA7, 0x58); }
/// Caution / needs attention. (#F5A524)
inline ImVec4 warning() { return rgb(0xF5, 0xA5, 0x24); }

// --- Text ladder ------------------------------------------------------------
// Three steps, so a panel can express "primary / secondary / barely there"
// without inventing a grey.
inline ImVec4 text_primary() { return rgb(0xDD, 0xE1, 0xE8); }
inline ImVec4 text_secondary() { return rgb(0x9A, 0xA3, 0xB2); }
inline ImVec4 text_faint() { return rgb(0x6B, 0x73, 0x82); }

// --- Surfaces ---------------------------------------------------------------
/// The panel background, for drawing a bar that must match the dockspace.
inline ImVec4 surface() { return rgb(0x1B, 0x1E, 0x24); }
/// One step above the panel background (headers, status bars).
inline ImVec4 surface_raised() { return rgb(0x22, 0x26, 0x2D); }
/// The window/void background behind every panel.
inline ImVec4 surface_sunken() { return rgb(0x14, 0x16, 0x1B); }

// --- Axis colours (the convention every DCC tool shares) --------------------
inline ImVec4 axis_x() { return rgb(0xE5, 0x5A, 0x5A); }
inline ImVec4 axis_y() { return rgb(0x6F, 0xC2, 0x5A); }
inline ImVec4 axis_z() { return rgb(0x4C, 0x8D, 0xFF); }

/// Returns `c` with its alpha replaced — the usual "tint this surface".
inline ImVec4 alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

/// The live accent colour (Settings > Accent colour). Defined in Toolbar.cpp
/// next to the settings object it reads, so this header stays free of it.
ImVec4 accent();

} // namespace nf::editor::theme
