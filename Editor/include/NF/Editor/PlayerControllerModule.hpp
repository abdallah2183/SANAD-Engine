#pragma once

// NF/Editor/PlayerControllerModule.hpp — forwarded to the engine.
//
// The module used to live in NFEditorCore, which meant only the editor binary
// could play a scene that names it: a standalone game window linking just the
// runtime had no "PlayerController" in its registry, so the character stood
// still no matter what the player pressed. It now lives in NFRuntime beside
// the rest of the play path (see NF/Runtime/PlayerControllerModule.hpp); this
// header keeps the editor-side include path working.

#include <NF/Runtime/PlayerControllerModule.hpp>

namespace nf::editor {
using ::nf::gameplay::PlayerControllerModule;
using ::nf::gameplay::PlayerControllerSettings;
} // namespace nf::editor
