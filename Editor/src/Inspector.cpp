#include <NF/Editor/Inspector.hpp>
#include <NF/Editor/Commands.hpp>
#include <NF/Assets/AssetId.hpp>
#include <NF/Runtime/Runtime.hpp>

#include <cmath>

namespace nf::editor {

namespace {

bool all_finite(std::initializer_list<float> vs) {
    for (float v : vs) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

// Strict "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" hex check. The engine's
// UUID::from_string is intentionally lenient (skips stray characters), so an
// explicit shape check is required to reject garbage like "not-a-uuid".
bool strict_uuid_shape(const std::string& s) {
    if (s.size() != 36) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        const bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
        const char c = s[i];
        if (dash) {
            if (c != '-') {
                return false;
            }
        } else {
            const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!hex) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TransformEdit read_transform(const ecs::World& world, ecs::Entity e) {
    TransformEdit out;
    const auto* t = world.get<scene::Transform>(e);
    if (t == nullptr) {
        return out;
    }
    out.px = t->local_x;
    out.py = t->local_y;
    out.pz = t->local_z;
    out.rx = t->rot_x;
    out.ry = t->rot_y;
    out.rz = t->rot_z;
    out.sx = t->scale_x;
    out.sy = t->scale_y;
    out.sz = t->scale_z;
    return out;
}

CameraEdit read_camera(const ecs::World& world, ecs::Entity e, bool& out_has) {
    CameraEdit out;
    const auto* c = world.get<runtime::CameraComponent>(e);
    out_has = (c != nullptr);
    if (c == nullptr) {
        return out;
    }
    out.active = c->is_active;
    out.fov_y = c->fov_y;
    out.near_plane = c->near_plane;
    out.far_plane = c->far_plane;
    return out;
}

LightEdit read_light(const ecs::World& world, ecs::Entity e, bool& out_has) {
    LightEdit out;
    const auto* l = world.get<runtime::DirectionalLight>(e);
    out_has = (l != nullptr);
    if (l == nullptr) {
        return out;
    }
    out.dir_x = l->dir_x;
    out.dir_y = l->dir_y;
    out.dir_z = l->dir_z;
    out.color_r = l->color_r;
    out.color_g = l->color_g;
    out.color_b = l->color_b;
    out.intensity = l->intensity;
    return out;
}

std::unique_ptr<ICommand> make_transform_command(ecs::World& world, ecs::Entity e,
                                                 const TransformEdit& edit, std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    auto* t = world.get<scene::Transform>(e);
    if (t == nullptr) {
        out_err = "Entity has no Transform";
        return nullptr;
    }
    if (!all_finite({edit.px, edit.py, edit.pz, edit.rx, edit.ry, edit.rz, edit.sx, edit.sy,
                     edit.sz})) {
        out_err = "Transform values must be finite numbers";
        return nullptr;
    }
    if (edit.sx == 0.0f || edit.sy == 0.0f || edit.sz == 0.0f) {
        out_err = "Scale components must be non-zero";
        return nullptr;
    }
    // Clamp absurd magnitudes instead of corrupting the scene.
    auto clamp_pos = [](float v) { return (v > 100000.0f) ? 100000.0f : ((v < -100000.0f) ? -100000.0f : v); };
    scene::Transform after = *t;
    after.local_x = clamp_pos(edit.px);
    after.local_y = clamp_pos(edit.py);
    after.local_z = clamp_pos(edit.pz);
    after.rot_x = edit.rx;
    after.rot_y = edit.ry;
    after.rot_z = edit.rz;
    after.scale_x = edit.sx;
    after.scale_y = edit.sy;
    after.scale_z = edit.sz;
    return std::make_unique<SetTransformCommand>(e, *t, after);
}

std::unique_ptr<ICommand> make_camera_command(ecs::World& world, ecs::Entity e,
                                              const CameraEdit& edit, std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    if (!all_finite({edit.fov_y, edit.near_plane, edit.far_plane})) {
        out_err = "Camera values must be finite numbers";
        return nullptr;
    }
    if (edit.fov_y < 1.0f || edit.fov_y > 179.0f) {
        out_err = "FOV must be within [1, 179] degrees";
        return nullptr;
    }
    if (edit.near_plane <= 0.0f) {
        out_err = "Near plane must be positive";
        return nullptr;
    }
    if (edit.far_plane <= edit.near_plane) {
        out_err = "Far plane must be greater than near plane";
        return nullptr;
    }
    const auto* cur = world.get<runtime::CameraComponent>(e);
    const bool had = (cur != nullptr);
    const runtime::CameraComponent before = had ? *cur : runtime::CameraComponent{};
    runtime::CameraComponent after = before;
    after.is_active = edit.active;
    after.fov_y = edit.fov_y;
    after.near_plane = edit.near_plane;
    after.far_plane = edit.far_plane;
    return std::make_unique<SetCameraCommand>(e, had, before, after);
}

std::unique_ptr<ICommand> make_light_command(ecs::World& world, ecs::Entity e,
                                             const LightEdit& edit, std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    if (!all_finite({edit.dir_x, edit.dir_y, edit.dir_z, edit.color_r, edit.color_g, edit.color_b,
                     edit.intensity})) {
        out_err = "Light values must be finite numbers";
        return nullptr;
    }
    const float len2 = edit.dir_x * edit.dir_x + edit.dir_y * edit.dir_y + edit.dir_z * edit.dir_z;
    if (len2 < 1e-12f) {
        out_err = "Light direction must be non-zero";
        return nullptr;
    }
    if (edit.intensity < 0.0f) {
        out_err = "Light intensity must be non-negative";
        return nullptr;
    }
    if (edit.color_r < 0.0f || edit.color_r > 1.0f || edit.color_g < 0.0f || edit.color_g > 1.0f ||
        edit.color_b < 0.0f || edit.color_b > 1.0f) {
        out_err = "Light color channels must be within [0, 1]";
        return nullptr;
    }
    const auto* cur = world.get<runtime::DirectionalLight>(e);
    const bool had = (cur != nullptr);
    const runtime::DirectionalLight before = had ? *cur : runtime::DirectionalLight{};
    runtime::DirectionalLight after = before;
    after.dir_x = edit.dir_x;
    after.dir_y = edit.dir_y;
    after.dir_z = edit.dir_z;
    after.color_r = edit.color_r;
    after.color_g = edit.color_g;
    after.color_b = edit.color_b;
    after.intensity = edit.intensity;
    return std::make_unique<SetLightCommand>(e, had, before, after);
}

std::unique_ptr<ICommand> make_mesh_command(ecs::World& world, ecs::Entity e,
                                            const std::string& asset_id_text,
                                            const std::string& material, std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    if (!strict_uuid_shape(asset_id_text)) {
        out_err = "Invalid AssetId '" + asset_id_text + "' (expected xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)";
        return nullptr;
    }
    const assets::AssetId id = assets::AssetId::from_string(asset_id_text);
    if (!id.valid()) {
        out_err = "Invalid AssetId '" + asset_id_text + "'";
        return nullptr;
    }
    const auto* cur = world.get<runtime::MeshComponent>(e);
    const bool had = (cur != nullptr);
    const runtime::MeshComponent before = had ? *cur : runtime::MeshComponent{};
    runtime::MeshComponent after;
    after.mesh_id = id;
    after.material = material;
    return std::make_unique<SetMeshCommand>(e, had, before, after);
}

std::unique_ptr<ICommand> make_rename_command(ecs::World& world, ecs::Entity e,
                                              const std::string& new_name, std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    if (new_name.size() > 128) {
        out_err = "Name is too long (max 128 characters)";
        return nullptr;
    }
    for (char c : new_name) {
        if (c == '\n' || c == '\r') {
            out_err = "Name must not contain newlines";
            return nullptr;
        }
    }
    auto cmd = std::make_unique<RenameCommand>(e, new_name);
    if (!cmd->capture_old(world)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    return cmd;
}

std::unique_ptr<ICommand> make_material_assignment_command(ecs::World& world, ecs::Entity e,
                                                           const std::string& after_path,
                                                           std::string& out_err) {
    if (!e.valid() || !world.is_alive(e)) {
        out_err = "Entity is not alive";
        return nullptr;
    }
    const auto* mc = world.get<runtime::MeshComponent>(e);
    if (mc == nullptr) {
        out_err = "Entity has no MeshComponent to assign a material to";
        return nullptr;
    }
    if (after_path.size() > 256) {
        out_err = "Material path is too long (max 256 characters)";
        return nullptr;
    }
    return std::make_unique<SetMaterialAssignmentCommand>(e, mc->material, after_path);
}

rendering::PBRMaterialParams read_material_params(runtime::Runtime& runtime, const std::string& path) {
    rendering::PBRMaterialParams params{};
    // Best effort: unknown paths resolve to defaults, never garbage.
    runtime.material_params(path, params);
    return params;
}

std::unique_ptr<ICommand> make_material_params_command(runtime::Runtime& runtime,
                                                       const std::string& path,
                                                       const MaterialEdit& edit, std::string& out_err) {
    if (!all_finite({edit.base_color[0], edit.base_color[1], edit.base_color[2], edit.base_color[3],
                     edit.metallic, edit.roughness, edit.ao, edit.emission[0], edit.emission[1],
                     edit.emission[2], edit.emission_strength})) {
        out_err = "Material values must be finite numbers";
        return nullptr;
    }
    for (int i = 0; i < 3; ++i) {
        if (edit.base_color[i] < 0.0f || edit.base_color[i] > 1.0f) {
            out_err = "Base color channels must be within [0, 1]";
            return nullptr;
        }
    }
    if (edit.metallic < 0.0f || edit.metallic > 1.0f) {
        out_err = "Metallic must be within [0, 1]";
        return nullptr;
    }
    if (edit.roughness < 0.0f || edit.roughness > 1.0f) {
        out_err = "Roughness must be within [0, 1]";
        return nullptr;
    }
    if (edit.ao < 0.0f || edit.ao > 1.0f) {
        out_err = "AO must be within [0, 1]";
        return nullptr;
    }
    for (int i = 0; i < 3; ++i) {
        if (edit.emission[i] < 0.0f || edit.emission[i] > 1.0f) {
            out_err = "Emission channels must be within [0, 1]";
            return nullptr;
        }
    }
    if (edit.emission_strength < 0.0f || edit.emission_strength > 8.0f) {
        out_err = "Emission strength must be within [0, 8]";
        return nullptr;
    }
    rendering::PBRMaterialParams before{};
    runtime.material_params(path, before);
    rendering::PBRMaterialParams after = before;
    for (int i = 0; i < 4; ++i) {
        after.base_color[i] = edit.base_color[i];
    }
    after.metallic = edit.metallic;
    after.roughness = edit.roughness;
    after.ao = edit.ao;
    for (int i = 0; i < 3; ++i) {
        after.emission[i] = edit.emission[i];
    }
    after.emission_strength = edit.emission_strength;
    // NOTE: use_base_color_texture is owned by the albedo binding, never by
    // scalar edits — copying `before` preserves it.
    return std::make_unique<SetMaterialParamsCommand>(&runtime, path, before, after);
}

std::unique_ptr<ICommand> make_material_albedo_command(runtime::Runtime& runtime,
                                                       const std::string& material_path,
                                                       const std::string& before_tex,
                                                       const std::string& after_tex) {
    return std::make_unique<SetMaterialAlbedoCommand>(&runtime, material_path, before_tex, after_tex);
}

} // namespace nf::editor
