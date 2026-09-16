#pragma once

// NF/Editor/Inspector.hpp — explicit per-component editors (no generic
// reflection in v0.1).
//
// Each edit is a validated factory: it reads the live component, checks the
// proposed values (finite numbers, sane ranges, parseable ids) and returns a
// command without touching the world. Invalid input yields an error string
// and leaves the scene untouched.

#include <NF/ECS/ECS.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/MaterialLibrary.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Transform.hpp>

#include <memory>
#include <string>

namespace nf::runtime {
class Runtime;
} // namespace nf::runtime

namespace nf::editor {

class ICommand;

struct TransformEdit {
    float px = 0, py = 0, pz = 0;
    float rx = 0, ry = 0, rz = 0; // degrees
    float sx = 1, sy = 1, sz = 1;
};

struct CameraEdit {
    bool active = false;
    float fov_y = 60.0f;
    float near_plane = 0.1f;
    float far_plane = 1000.0f;
};

struct LightEdit {
    float dir_x = -0.5f, dir_y = -1.0f, dir_z = -0.3f;
    float color_r = 1.0f, color_g = 1.0f, color_b = 1.0f;
    float intensity = 1.0f;
    bool cast_shadows = true;
    float shadow_strength = 1.0f;
    float shadow_bias = 0.0005f;
};

// Procedural sky settings for the selected entity (Phase 13). Mirrors
// runtime::SkyComponent; validated like every other edit (finite, clamped).
struct SkyEdit {
    float zenith[3] = {0.20f, 0.42f, 0.85f};
    float horizon[3] = {0.62f, 0.72f, 0.82f};
    float ground[3] = {0.09f, 0.09f, 0.11f};
    float sun_disk = 1.0f;
    float sun_glow = 1.0f;
    bool enabled = true;
};

struct MaterialEdit {
    float base_color[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.4f;
    float ao = 1.0f;
    float emission[3] = {0.0f, 0.0f, 0.0f};
    float emission_strength = 0.0f;
};

TransformEdit read_transform(const ecs::World& world, ecs::Entity e);
CameraEdit read_camera(const ecs::World& world, ecs::Entity e, bool& out_has);
LightEdit read_light(const ecs::World& world, ecs::Entity e, bool& out_has);
SkyEdit read_sky(const ecs::World& world, ecs::Entity e, bool& out_has);

// All factories return nullptr + err on invalid entity or invalid values.
std::unique_ptr<ICommand> make_transform_command(ecs::World& world, ecs::Entity e,
                                                 const TransformEdit& edit, std::string& out_err);
std::unique_ptr<ICommand> make_camera_command(ecs::World& world, ecs::Entity e,
                                              const CameraEdit& edit, std::string& out_err);
std::unique_ptr<ICommand> make_light_command(ecs::World& world, ecs::Entity e,
                                              const LightEdit& edit, std::string& out_err);
std::unique_ptr<ICommand> make_sky_command(ecs::World& world, ecs::Entity e,
                                           const SkyEdit& edit, std::string& out_err);
std::unique_ptr<ICommand> make_mesh_command(ecs::World& world, ecs::Entity e,
                                            const std::string& asset_id_text,
                                            const std::string& material, std::string& out_err);
std::unique_ptr<ICommand> make_rename_command(ecs::World& world, ecs::Entity e,
                                              const std::string& new_name, std::string& out_err);

// Material assignment (entity must carry a MeshComponent; empty path selects
// the default material).
std::unique_ptr<ICommand> make_material_assignment_command(ecs::World& world, ecs::Entity e,
                                                           const std::string& after_path,
                                                           std::string& out_err);

// Shared material parameter edit. Reads current values from runtime (file or
// live instance) and validates ranges; the command writes through on
// apply/undo so the viewport updates next frame.
rendering::PBRMaterialParams read_material_params(runtime::Runtime& runtime, const std::string& path);
std::unique_ptr<ICommand> make_material_params_command(runtime::Runtime& runtime,
                                                       const std::string& path,
                                                       const MaterialEdit& edit, std::string& out_err);
// Albedo bind validated by the caller (EditorApp pre-uploads); the factory
// only snapshots the before-path for undo.
std::unique_ptr<ICommand> make_material_albedo_command(runtime::Runtime& runtime,
                                                       const std::string& material_path,
                                                       const std::string& before_tex,
                                                       const std::string& after_tex);
// Mip filter switch. Snapshots the live mode for undo; rejects out-of-range
// modes without touching the runtime.
std::unique_ptr<ICommand> make_material_mip_command(runtime::Runtime& runtime,
                                                    const std::string& material_path,
                                                    rhi::MipMapMode after_mode,
                                                    std::string& out_err);

} // namespace nf::editor
