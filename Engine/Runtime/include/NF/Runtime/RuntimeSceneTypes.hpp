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
    float shadow_strength = 1.0f; // 0 = no darkening .. 1 = full shadow
    float shadow_bias = 0.0005f;  // depth bias against shadow acne
    // Cascaded shadow maps: how many camera-following ranges the atlas is split
    // into. One map fitted around the whole frustum is cheap but its texel is
    // coarse; splitting follows the viewer with far more detail up close, and
    // costs no extra memory because the tiles share the map the single cascade
    // used to occupy. Clamped by the renderer to [1, kMaxShadowCascades].
    u32 shadow_cascades = 4;
    // Farthest distance shadows are cast to; 0 means "the camera's far plane".
    // Lowering it concentrates the atlas on the range a player can actually
    // read, which is the usual open-world trade.
    float shadow_distance = 0.0f;
};

/// Procedural sky settings (Phase 13). Attach to any entity; the first one
/// found wins. Mirrors rendering::SkyParams so the renderer takes it as-is.
struct SkyComponent {
    float zenith_r = 0.20f, zenith_g = 0.42f, zenith_b = 0.85f;
    float horizon_r = 0.62f, horizon_g = 0.72f, horizon_b = 0.82f;
    float ground_r = 0.09f, ground_g = 0.09f, ground_b = 0.11f;
    float clear_r = 0.03f, clear_g = 0.03f, clear_b = 0.07f;
    float sun_disk = 1.0f;
    float sun_glow = 1.0f;
    bool enabled = true;
};

struct CameraComponent {
    float fov_y = 60.0f; // degrees
    float aspect = 16.0f/9.0f;
    float near_plane = 0.1f;
    float far_plane = 1000.0f;
    bool is_active = false;
    // Position is stored in Transform, not here
};

/// A breakable object authored in the scene (Phase 19, design doc §41
/// "Breakable meshes / Fracture assets / Debris / Impulses").
///
/// There is no fracture-asset import pipeline, so the asset is cooked at load
/// time from this spec — the same relationship an Animation component's
/// `procedural=` spec has to the clip it builds, and an Audio component's
/// `tone=` to the buffer it synthesises. The spec is what survives save/load;
/// the cooked FractureAsset is a runtime artefact.
///
/// The collider on the same entity supplies the shape the asset is cut from
/// (a box, for v0.1 — other shapes are ignored with a warning, not substituted,
/// because a silently-different fracture shape is the kind of gap this project
/// keeps having to re-find). The RigidBody must be Static: a dynamic
/// destructible would be simulated by the engine solver and by the debris world
/// at once, and the two would disagree about where it is.
struct DestructibleComponent {
    /// FractureParams::target_chunks: how many leaves the cut tree has, and so
    /// how many bodies a complete break can spawn.
    u32 chunks = 4u;
    /// FractureParams::seed. Same seed, same shards (§114 determinism): two
    /// runs of one scene produce one pile of debris.
    u32 seed = 0x5EEDBEEFu;
    /// FractureParams::strength_per_area: bond strength per unit of shared face
    /// area. Higher means the object takes a harder hit to come apart.
    f32 strength = 25.0f;
    /// Impact impulse (N·s) a contact must deliver in one step before the
    /// object takes damage at all. Resting contacts deliver only weight·dt per
    /// step, so this is what tells a slam from a stack.
    f32 damage_threshold = 8.0f;
    /// How far from the impact point the damage spreads, in world units.
    f32 blast_radius = 2.0f;
    /// Off: the object renders and collides but never breaks. Lets a level
    /// author place the prop without opting the scene into the debris budget.
    bool enabled = true;
};

} // namespace nf::runtime
