#pragma once

// NF/Editor/OrbitCameraModule.hpp — forwarded to the engine.
//
// The module used to live in NFEditorCore; it moved to NFRuntime so a scene
// authored in the editor plays with the same camera behaviour in the
// standalone game window (see NF/Runtime/OrbitCameraModule.hpp). This header
// keeps the editor-side include path — and EditorTests — working unchanged.

#include <NF/Runtime/OrbitCameraModule.hpp>

namespace nf::editor {
using ::nf::gameplay::OrbitCameraModule;
using ::nf::gameplay::OrbitCameraSettings;
} // namespace nf::editor
