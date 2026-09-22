#pragma once

// NF/Runtime/PlayerControllerModule.hpp — arrow/WASD player movement.
//
// Attach to any entity with a Transform and press Play: arrows (or WASD)
// drive it on the ground plane, Shift sprints, Space jumps. Works with or
// without physics — a Dynamic body is steered by velocity so the solver stays
// in charge, anything else is moved kinematically.
//
// Lives in NFRuntime, not in the editor: a scene authored in the editor must
// play the same in the standalone game window, and a module the player binary
// cannot link is a module that silently does nothing there. Registered as
// "PlayerController" so the inspector dropdown offers it and Runtime::init_
// gameplay() instantiates it with no engine code naming this type.

#include <NF/Gameplay/GameplayModule.hpp>

#include <NF/Core/Math.hpp>
#include <NF/Core/Reflection.hpp>

namespace nf::gameplay {

struct PlayerControllerSettings {
    f32 speed = 5.0f;
    f32 sprint_multiplier = 2.0f;
    f32 jump_velocity = 5.0f;
    bool use_input = true;

    NF_CLASS(PlayerControllerSettings)
    NF_PROPERTY(PlayerControllerSettings, speed, Float, Prop_EditAnywhere | Prop_SerializeField, "Movement")
    NF_PROPERTY(PlayerControllerSettings, sprint_multiplier, Float, Prop_EditAnywhere | Prop_SerializeField, "Movement")
    NF_PROPERTY(PlayerControllerSettings, jump_velocity, Float, Prop_EditAnywhere | Prop_SerializeField, "Movement")
    NF_PROPERTY(PlayerControllerSettings, use_input, Bool, Prop_EditAnywhere | Prop_SerializeField, "Input")
    NF_CLASS_END(PlayerControllerSettings)
};

class PlayerControllerModule final : public GameplayModule {
public:
    [[nodiscard]] const char* name() const override { return "PlayerController"; }

    void on_update(GameplayContext& ctx) override;

    GameplayStateBinding state() override {
        return {&settings, PlayerControllerSettings::nf_class_meta()};
    }

    PlayerControllerSettings settings;

    [[nodiscard]] u32 moves() const { return m_moves; }

private:
    u32 m_moves = 0;
};

} // namespace nf::gameplay
