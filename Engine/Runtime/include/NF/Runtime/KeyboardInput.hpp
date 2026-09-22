#pragma once

// NF/Runtime/KeyboardInput.hpp — the default keyboard input for a windowed
// runtime run.
//
// A game window that never installs an input source means a scene with a
// PlayerController renders beautifully and never moves — every gameplay module
// that polls sees a dead device. This source polls the real keyboard so the
// standalone player behaves like the editor's play mode.
//
// The action vocabulary is the same one the editor's play input uses (it is
// the vocabulary PlayerControllerModule and OrbitCameraModule read): arrows
// and WASD drive movement, Space jumps, Shift sprints, E interacts. A game
// that wants its own device installs its IInputSource over this one.

#include <NF/Gameplay/GameplayModule.hpp>

namespace nf::runtime {

class KeyboardInputSource final : public nf::gameplay::IInputSource {
public:
    [[nodiscard]] bool action_pressed(std::string_view action) const override;
    [[nodiscard]] float action_axis(std::string_view action) const override;
};

} // namespace nf::runtime
