#pragma once

#include <NF/Assets/AssetId.hpp>
#include <NF/Core/Types.hpp>

#include <string>

namespace nf::runtime {

// Components used in the Runtime Scene (.nfscene v1)
// These are explicit, not generic reflection, for v0.1

struct MeshComponent {
    assets::AssetId mesh_id;
    std::string material; // logical path or name, e.g. "content://Materials/Default"
};

struct MaterialComponent {
    std::string material_name;
    // For v0.1, just a name; later this will be an AssetId for Material asset
};

struct DirectionalLight {
    float dir_x = -0.5f, dir_y = -1.0f, dir_z = -0.3f;
    float color_r = 1.0f, color_g = 1.0f, color_b = 1.0f;
    float intensity = 1.0f;
    bool cast_shadows = true; // Phase 13: feeds the renderer's shadow map
};

struct CameraComponent {
    float fov_y = 60.0f; // degrees
    float aspect = 16.0f/9.0f;
    float near_plane = 0.1f;
    float far_plane = 1000.0f;
    bool is_active = false;
    // Position is stored in Transform, not here
};

} // namespace nf::runtime
