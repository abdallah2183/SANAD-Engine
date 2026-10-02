#pragma once

// NF/Editor/TransformGizmoView.hpp — ImGui drawing + pointer hit-testing for
// the in-viewport transform gizmo.
//
// One call per frame, right after the viewport image (and the orientation
// gizmo): draws the handles for the current selection/mode and, when the
// pointer grabs one, raises UiIntents::viewport_gizmo_press for the shell.
// The math (layout, hit-testing) is TransformGizmo.hpp; this file only owns
// pixels, colours and the ImGui calls.

#include <NF/Editor/UiShell.hpp>
#include <NF/Rendering/Camera.hpp>

#include <imgui.h>

namespace nf::editor {

class EditorApp;

/// Draws + hit-tests the gizmo. Returns the handle under the pointer
/// (None when the pointer grabs air). No-op while playing, with no selection,
/// or when the selection's origin entity carries no Transform. Never consumes
/// the click itself: the caller routes a click on the returned handle to
/// UiIntents::viewport_gizmo_press (and skips its pick press for it).
GizmoHandle draw_transform_gizmo(EditorApp& app, const rendering::Camera& cam,
                                 const ImVec2& img_min, const ImVec2& img_max);

} // namespace nf::editor
