#pragma once

// NF/Editor/FileSystemPanel.hpp — Godot-style FileSystem dock.
//
// The panel is ImGui-only (app target), while the navigation / favorites /
// icon rules it renders live in NF/Editor/AssetBrowser.hpp as pure functions
// covered by EditorTests. This header is the thin recording entry point the
// dockspace calls once per frame.

#include <NF/Editor/EditorApp.hpp>
#include <NF/Editor/UiShell.hpp>

namespace nf::editor {

// Records the full FileSystem dock inside the current ImGui window stack:
// tabs (FileSystem/History), nav bar (back/forward/up/refresh + breadcrumb),
// filter row, left favorites+tree / right files split, status bar and the
// import tools. Reads EditorApp, writes only through EditorApp methods and
// the UiIntents it fills (scene open on double-click, mesh drop is handled
// by drag payloads like before).
void filesystem_panel(EditorApp& app, UiIntents& intents);

} // namespace nf::editor
