#pragma once

// NF/Editor/OrbitCameraModule.hpp — a worked example of the gameplay API (Phase 10)
//
// This exists to answer "what does a gameplay module actually look like". It is
// deliberately a real module rather than a stub: it reads input, writes a
// transform, exposes reflected settings the inspector can edit, and reports an
// observable a test can assert on.
//
// It lives in NFEditorCore rather than in the editor executable so EditorTests
// can drive it — a sample that cannot be run headlessly is documentation, not a
// demonstration.

#include <NF/Gameplay/GameplayModule.hpp>

#include <NF/Core/Math.hpp>
#include <NF/Core/Reflection.hpp>

namespace nf::editor {

/// Settings for the orbit camera. Reflected, so the inspector shows them under
/// "Orbit" without a hand-written panel, and `save_game` persists them without a
/// hand-written serializer.
struct OrbitCameraSettings {
    f32  radius         = 6.0f;
    f32  yaw_degrees    = 0.0f;
    f32  pitch_degrees  = 20.0f;
    /// Degrees per second applied while the "look" axis is held.
    f32  yaw_speed      = 60.0f;
    /// Metres per second applied while the "zoom" axis is held.
    f32  zoom_speed     = 4.0f;
    f32  min_radius     = 1.0f;
    f32  max_radius     = 50.0f;
    bool use_input      = true;
    /// Entity to orbit. `u32_max` means "the entity carrying the camera".
    u32  target_entity  = u32_max;

    NF_CLASS(OrbitCameraSettings)
    NF_PROPERTY(OrbitCameraSettings, radius,        Float,    Prop_EditAnywhere | Prop_SerializeField, "Orbit")
    NF_PROPERTY(OrbitCameraSettings, yaw_degrees,   Float,    Prop_EditAnywhere | Prop_SerializeField, "Orbit")
    NF_PROPERTY(OrbitCameraSettings, pitch_degrees, Float,    Prop_EditAnywhere | Prop_SerializeField, "Orbit")
    NF_PROPERTY(OrbitCameraSettings, yaw_speed,     Float,    Prop_EditAnywhere | Prop_SerializeField, "Input")
    NF_PROPERTY(OrbitCameraSettings, zoom_speed,    Float,    Prop_EditAnywhere | Prop_SerializeField, "Input")
    NF_PROPERTY(OrbitCameraSettings, min_radius,    Float,    Prop_EditAnywhere | Prop_SerializeField, "Limits")
    NF_PROPERTY(OrbitCameraSettings, max_radius,    Float,    Prop_EditAnywhere | Prop_SerializeField, "Limits")
    NF_PROPERTY(OrbitCameraSettings, use_input,     Bool,     Prop_EditAnywhere | Prop_SerializeField, "Input")
    NF_PROPERTY(OrbitCameraSettings, target_entity, EntityRef, Prop_EditAnywhere | Prop_SerializeField, "Target")
    NF_CLASS_END(OrbitCameraSettings)
};

/// Positions the scene's camera on a sphere around a target and aims it inward.
///
/// Input actions it reads, when a source is present and `use_input` is set:
///   "look"  — yaw, in [-1, 1]
///   "zoom"  — radius, in [-1, 1]
class OrbitCameraModule final : public gameplay::GameplayModule {
public:
    [[nodiscard]] const char* name() const override { return "OrbitCamera"; }

    /// After the default 0.0, so a module that moves the target this frame has
    /// already run by the time the camera looks at it.
    [[nodiscard]] f32 update_priority() const override { return 10.0f; }

    void on_update(gameplay::GameplayContext& ctx) override;

    gameplay::GameplayStateBinding state() override {
        return {&settings, OrbitCameraSettings::nf_class_meta()};
    }

    OrbitCameraSettings settings;

    /// Frames in which the camera was actually placed. The observable that
    /// separates "the module ran" from "the module ran and found nothing to
    /// move", which is the failure this whole layer is prone to.
    [[nodiscard]] u32 placements() const { return m_placements; }
    /// Entity the last placement used, or `u32_max` when there was none.
    [[nodiscard]] u32 last_target() const { return m_last_target; }
    [[nodiscard]] Vec3 last_camera_position() const { return m_last_position; }

private:
    u32  m_placements   = 0;
    u32  m_last_target  = u32_max;
    Vec3 m_last_position;
};

} // namespace nf::editor
