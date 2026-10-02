#pragma once

// NF/Editor/ScenePanels.hpp — the dedicated editors: one panel per engine
// function, each bound to a live scene object.
//
// The Inspector answers "what is on the thing I selected". These answer "what is
// in my SCENE" — the light the renderer actually uses, the sky it actually
// paints, the camera the game actually looks through, and what the renderer is
// doing right now. They are separate windows on purpose: a lighting artist wants
// the light, not a tree row that happens to hold it, and every one of them shows
// which object it is bound to (the renderer takes the FIRST of each, so a panel
// that did not say which one would be a guess).
//
// Every write goes through EditorApp's validated command path, so an edit from
// here is undoable exactly like the same edit from the Inspector.

#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/UiShell.hpp>

namespace nf::editor {

/// Records the dedicated editors (Lighting, Environment, Camera, Render &
/// Performance, World). Visibility lives on EditorUiSettings so the Window menu
/// and these windows agree.
void scene_panels(EditorApp& app, const UiFrameStats& stats);

} // namespace nf::editor
