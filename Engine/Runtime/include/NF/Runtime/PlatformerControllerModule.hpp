#pragma once

#include <NF/Core/Reflection.hpp>
#include <NF/Gameplay/GameplayModule.hpp>

namespace nf::gameplay {

struct PlatformerControllerSettings {
    f32 speed = 4.0f;
    f32 jump_velocity = 7.0f;

    NF_CLASS(PlatformerControllerSettings)
    NF_PROPERTY(PlatformerControllerSettings, speed, Float, Prop_EditAnywhere | Prop_SerializeField, "Movement")
    NF_PROPERTY(PlatformerControllerSettings, jump_velocity, Float, Prop_EditAnywhere | Prop_SerializeField, "Movement")
    NF_CLASS_END(PlatformerControllerSettings)
};

/// Side-view movement, driven through the same actions on every platform.
/// Requires a dynamic rigid body and collider; physics owns the position.
class PlatformerControllerModule final : public GameplayModule {
public:
    [[nodiscard]] const char* name() const override { return "PlatformerController"; }
    void on_update(GameplayContext& ctx) override;
    void on_scene_unload(GameplayContext&) override { m_jump_held = false; }
    GameplayStateBinding state() override { return {&settings, PlatformerControllerSettings::nf_class_meta()}; }
    PlatformerControllerSettings settings;

private:
    bool m_jump_held = false;
};

} // namespace nf::gameplay
