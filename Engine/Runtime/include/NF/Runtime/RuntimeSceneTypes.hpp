#pragma once

#include <NF/Assets/AssetId.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/ECS/Entity.hpp>

#include <string>
#include <vector>

namespace nf {

namespace ai {
class NavMesh;
} // namespace ai

namespace runtime {

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

/// A day/night cycle driven by one clock (design doc §64).
///
/// `rendering::TimeOfDay` already derives a sun direction, a sun colour/intensity
/// and a matching sky palette from an hour-of-day — but it was reachable only
/// from the hand-written samples (Basic3D, MedievalVillage). A scene could not
/// carry one, so "the sun moves and the sky follows" was a C++ edit, not an
/// authoring decision. This component is that decision, as data.
///
/// It attaches to the SAME entity as a `SkyComponent` and drives it: while it
/// is present the Runtime advances the clock each frame and overrides the sky
/// palette and the entity's directional light with the computed values. The
/// authored `Sky:` palette stays the fallback for the frames before the first
/// advance and for a scene that disables the cycle.
///
/// Why the component holds the clock state rather than the rendering class: the
/// clock is authored data that a save must round-trip, and `TimeOfDay` is pure
/// math over it with no device behind it. Keeping the two apart means the
/// component can be asserted without a GPU.
struct TimeOfDayComponent {
    /// Hour of day in [0, 24). The loader wraps out-of-range values rather than
    /// rejecting the line, because 25.0 is an obvious way to write "1am" and
    /// refusing the whole scene over it would be the larger surprise.
    float time_hours = 12.0f;
    /// Real seconds for one full 24h cycle. <= 0 freezes the clock: the frame
    /// still renders, it just stops advancing. Frozen is a legitimate authoring
    /// choice (a fixed golden hour), so it is not an error.
    float day_length_seconds = 240.0f;
    /// false = keep the authored Sky palette and directional light untouched.
    /// A scene with the component but this off is a way to author the settings
    /// without letting them drive anything.
    bool enabled = true;
    /// Drive the entity's directional light from the computed sun as well as the
    /// sky. A scene whose sun is hand-posed (a stylized fixed key light) can take
    /// the moving sky without having its lighting swung around underneath it.
    bool drive_light = true;
};

/// The post-processing stack (design §206) as scene data.
///
/// The renderer has owned a post block since the tonemap pass existed, but a
/// scene could not reach any of it: exposure and the tonemap operator were
/// `Renderer3D` setters, so "give this level a glow" was a C++ edit. This is
/// that decision, as data — the same move `SkyComponent` and `TimeOfDayComponent`
/// made for the sky.
///
/// Flat floats rather than `rendering::PostFxParams`, following `SkyComponent`:
/// this header is included by the editor and by the tools, and pulling
/// `NF/Rendering/Renderer3D.hpp` (and through it the RHI) into all of them to
/// carry fifteen numbers would be the wrong trade.
///
/// Every field defaults to the renderer's own neutral value, so a scene that
/// carries the component with nothing set renders exactly as a scene that has
/// never heard of it. That is the same "absent means unchanged" contract the
/// scene writer relies on to keep old files byte-identical.
struct PostProcessComponent {
    // Bloom (§206). The chain is a real multi-pass blur; `enabled` is the only
    // field that costs anything, because with it false the renderer records no
    // bloom pass at all.
    bool bloom_enabled = false;
    /// Luminance above which a pixel blooms. In raw HDR units, before exposure.
    float bloom_threshold = 1.0f;
    /// Width of the soft ramp across the threshold. 0 is a hard cut, which makes
    /// the glow pop as a surface crosses the threshold.
    float bloom_knee = 0.5f;
    /// Multiplier on the summed levels. 0 is exactly "off" regardless of
    /// `bloom_enabled`, so a slider at zero and an unchecked box agree.
    float bloom_intensity = 1.0f;
    /// Blur spread, as a multiplier on the kernel's tap spacing.
    float bloom_radius = 1.0f;

    // Colour grading, applied in HDR before the tonemap operator.
    bool grade_enabled = false;
    float grade_contrast = 1.0f;
    /// HDR luminance the contrast scale leaves fixed.
    float grade_pivot = 1.0f;
    /// -1 cool .. +1 warm.
    float grade_temperature = 0.0f;
    /// -1 green .. +1 magenta.
    float grade_tint = 0.0f;
    float grade_gamma = 1.0f;

    // Unsharp mask on the linear HDR image, before the bloom add.
    bool sharpen_enabled = false;
    float sharpen_amount = 0.0f;
    float sharpen_radius = 1.0f;

    // The two stages that predate the rest of the stack, in the order
    // PostFxParams declares them so the two structs read the same way.
    float saturation = 1.0f;
    float vignette = 0.0f;

    // Lens effects (§206): the two UV-space warps a real lens imposes. Both
    // neutral at zero, and zero is exactly identity — a scene that never
    // touches these renders the frame it rendered before the stage existed.
    bool lens_enabled = false;
    /// Barrel (+) / pincushion (-), as a coefficient on r². 0 = none.
    float lens_distortion = 0.0f;
    /// Red/blue radial split, as a fraction of r. 0 = none.
    float lens_chromatic_aberration = 0.0f;

    // Depth of field (§206). `dof_max_radius` at 0 is exactly "off".
    bool dof_enabled = false;
    /// World-space distance from the camera that stays in focus.
    float dof_focus_distance = 10.0f;
    /// Distance over which the blur ramps from zero to the full radius.
    float dof_focus_range = 2.0f;
    /// Blur radius in output texels at full confusion.
    float dof_max_radius = 6.0f;

    // Motion blur (§206), from depth reprojection. `motion_intensity` at 0 is
    // exactly "off".
    bool motion_enabled = false;
    /// Multiplier on the reprojected velocity.
    float motion_intensity = 1.0f;
    /// Cap on the smear, in uv units.
    float motion_max_length = 0.05f;

    // Exposure and the tonemap operator (§206).
    //
    // Both are OPT-IN, and their sentinels are what makes them safe to add:
    // 0 exposure and -1 operator mean "the scene says nothing", so the
    // renderer's own setting survives — which is what the editor's acceptance
    // run depends on, its golden pixel count being pinned to the defaults.
    // Exposure is otherwise a DISPLAY preference rather than scene data (it is
    // the author's chosen key for the frame), so it is written to the file only
    // when a scene actually names it.
    /// 0 = not authored; otherwise the renderer's exposure (must be > 0).
    float exposure = 0.0f;
    /// -1 = not authored; otherwise a `rendering::TonemapMode` value.
    int tonemap = -1;

    // Colour-grading LUT (§206). `lut_path` is a logical IMAGE path the runtime
    // resolves through the VFS into a `kLutSize^3` strip; `lut_strength` at 0
    // is exactly "off", so a scene with no LUT is the frame it was before the
    // stage existed.
    /// Empty = no LUT.
    std::string lut_path;
    /// 0 = off .. 1 = the LUT fully replaces the graded colour.
    float lut_strength = 0.0f;

    // Distance fog. It lives on the post block because that is the entity an
    // author already opens to shape the frame's atmosphere; the renderer applies
    // it in the lighting and transparency passes, not in the post chain, but the
    // scene-facing switch belongs next to exposure/grading. Off by default, and
    // the default band sits past the template's playable area, so a scene that
    // never sets fog renders exactly as it did before the fields existed.
    bool fog_enabled = false;
    /// Haze tint, linear RGB. Author it as the sky's horizon colour when the
    /// terrain should fade into that horizon.
    float fog_r = 0.62f;
    float fog_g = 0.66f;
    float fog_b = 0.70f;
    /// World-space distance where the fade begins / is fully haze.
    float fog_start = 60.0f;
    float fog_end = 120.0f;
};

// Local lights (Phase 26). A scene until now could only carry one directional
// light, which is the light of an outdoor day: the renderer has accepted point
// and spot lights since Phase 21 and the deferred path lights and shadows them
// properly, but only a hand-written sample could place one. A torch, a ceiling
// lamp and a lighthouse beam are the common case of every indoor and night
// scene, and "author the lighting" without them meant editing a C++ file.
//
// Position is NOT a field here: it is the entity's world Transform, the same
// rule the audio emitter follows. Moving the lamp in the viewport moves the
// light with it, and no second copy of the position exists to drift out of
// agreement with the one the gizmo drags.
//
// Direction, unlike position, IS a field — for the same reason the directional
// light stores `dir()` instead of reading a Transform rotation: a lamp's mesh
// can be posed to sit on a table while its beam aims elsewhere, so deriving the
// aim from the entity rotation would swing the beam every time a prop was
// rotated.
//
// Angles are stored in radians and the key names say so (`inner_rad=`,
// `outer_rad=`). Degrees read better to a human and cost a conversion in the
// loader, the serializer, the inspector and the tests — four places where a
// radian/degree mix-up hides as "my spotlight is a floodlight". The renderer's
// defaults (0.35 / 0.6 rad) survive here untouched, so a spot authored with
// defaults lights exactly like the renderer's default spot.

/// A point light authored in the scene.
struct PointLightComponent {
    float color_r = 1.0f, color_g = 1.0f, color_b = 1.0f;
    float intensity = 1.0f;
    /// Distance past which the light contributes nothing; feeds
    /// `rendering::PointLight::radius`, and the default far plane of its six
    /// shadow projectors when `shadow_distance` is 0.
    float radius = 10.0f;
    /// Six depth renders per frame — six times the price of the spot below, so
    /// a torch that never visibly casts a shadow should not pay for one. The
    /// first `kMaxShadowPointLights` shadow-casting point lights of the scene
    /// (entity order) get maps; the rest light the scene unshadowed whatever
    /// this says.
    bool cast_shadows = false;
    float shadow_strength = 1.0f; // 0 = no darkening .. 1 = full shadow
    float shadow_bias = 0.0005f;
    /// Farthest distance the six face projectors cover; 0 means `radius`.
    /// Spending precision past the radius only blurs the shadow.
    float shadow_distance = 0.0f;
    /// Off: the entity, its Transform and its name survive, it just throws no
    /// light — the "switch it off, keep it placed" state a level author needs.
    bool enabled = true;
};

/// A spot light authored in the scene: a point light with a cone.
struct SpotLightComponent {
    /// Direction the light TRAVELS, normalised on load. Same convention as
    /// `rendering::SpotLight::direction` and `DirectionalLight::dir_x/y/z`.
    float dir_x = 0.0f, dir_y = -1.0f, dir_z = 0.0f;
    float color_r = 1.0f, color_g = 1.0f, color_b = 1.0f;
    /// The renderer's own spot default, so an unset spot behaves like a default
    /// `rendering::SpotLight`.
    float intensity = 2.0f;
    /// Fully lit core of the cone, radians from the axis. May not exceed
    /// `outer_angle_rad`; the falloff between the two is the soft edge.
    float inner_angle_rad = 0.35f;
    /// Edge of the cone, radians from the axis. Capped at pi/2: the shadow
    /// projector is a perspective frustum and no single frustum fits a wider
    /// cone than that.
    float outer_angle_rad = 0.6f;
    /// The cone's reach. The default is the 25.0 that was hard-coded in
    /// lighting.frag before `SpotLight::range` existed (the renderer's
    /// `kLocalShadowSpotDefaultFar` — pinned equal by a static_assert in
    /// SceneExtraction.cpp), so a spot authored with defaults behaves exactly
    /// like the old hard-coded one.
    float range = 25.0f;
    /// One depth render per light — cheap beside a point light's six. Only the
    /// first `kMaxShadowSpotLights` shadow-casting spots get a map.
    bool cast_shadows = false;
    float shadow_strength = 1.0f;
    float shadow_bias = 0.0005f;
    /// Farthest distance the cone projector covers; 0 means `range`.
    float shadow_distance = 0.0f;
    /// See `PointLightComponent::enabled`.
    bool enabled = true;
};

struct CameraComponent {
    float fov_y = 60.0f; // degrees
    float aspect = 16.0f/9.0f;
    float near_plane = 0.1f;
    float far_plane = 1000.0f;
    bool is_active = false;
    // Position is stored in Transform, not here

    // --- The follow camera ------------------------------------------------
    //
    // A camera that always looks at the origin is a camera that can only
    // show a scene arranged around the origin, and no third-person game is
    // that. These fields make the camera follow an entity instead: where it
    // looks (the pivot), how far it sits and from which angle (the orbit),
    // how fast it catches up (smoothing), and what it does when a tree is
    // in the way (the collision sphere).
    //
    // The maths is in NF/Runtime/CameraFollow.hpp; this is the scene data
    // for it.

    /// The name of the entity the camera follows. Empty = no follow: the
    /// camera is wherever its own Transform says and looks at the origin,
    /// which is exactly what every camera did before.
    ///
    /// A NAME and not a handle: the scene file cannot point at another
    /// entity yet, and a handle that resolves to nothing at load time is a
    /// silent no-op. A name that matches nothing leaves the camera where it
    /// was, rather than snapping it to the origin.
    std::string follow_target;

    /// The pivot, in the TARGET'S local space: the point the camera orbits
    /// and looks at. (0, 1.6, 0) is roughly a character's head.
    float pivot_x = 0.0f;
    float pivot_y = 1.6f;
    float pivot_z = 0.0f;

    /// How far the camera sits from the pivot.
    float distance = 5.0f;

    /// The orbit angles, in degrees. yaw is the horizontal rotation (0 = the
    /// camera is behind the target); pitch is the vertical one (negative =
    /// looking down).
    float yaw = 0.0f;
    float pitch = -15.0f;

    /// How fast the camera catches up with the target, in units of the
    /// remaining distance per second. 0 means "no smoothing".
    float smoothing = 8.0f;

    /// The radius of the camera's collision sphere, in metres. When something
    /// solid is between the pivot and the camera, the camera is pulled in
    /// front of it by this much, so it never ends up inside a tree.
    float collision_radius = 0.35f;

    /// Whether the camera pulls in when something is in the way.
    bool avoid_collision = true;

    // --- Aiming the camera ------------------------------------------------
    //
    // A follow camera that only obeys the yaw in this file is a camera the
    // player watches. These fields let the player turn it: how far one pixel
    // of mouse travel turns it, how fast a held stick turns it, how far up
    // and down it may look, and whether the wheel changes the distance.
    //
    // The maths is in NF/Runtime/PlayerLook.hpp; this is the scene data for
    // it, and every field is optional in the file — a scene written before
    // aiming existed loads with the defaults below, which are the aiming
    // every third-person game has.

    /// Whether the mouse aims this camera while the game is playing.
    ///
    /// On by default, because a third-person camera the player cannot turn
    /// is the one thing the 0.3.0 notes listed as missing. A scene that
    /// wants the old fixed orbit sets `mouse=0`.
    bool mouse_look = true;

    /// Degrees of yaw per pixel of mouse travel.
    float look_sensitivity = 0.12f;

    /// Degrees per second at full stick deflection.
    float stick_sensitivity = 180.0f;

    /// The vertical clamp, in degrees. Negative looks down.
    float pitch_min = -80.0f;
    float pitch_max = 60.0f;

    /// Flip the vertical axis for players who grew up with it.
    bool invert_look_y = false;

    /// The wheel's distance range, in metres, and how far one notch moves.
    float zoom_min = 1.5f;
    float zoom_max = 12.0f;
    float zoom_step = 0.6f;

    /// Degrees per second the camera drifts back behind the character while
    /// it runs without the mouse. 0 disables it, which is what every scene
    /// authored before this existed gets.
    float align_speed = 0.0f;
};

/// The navigation volume: where the walkable mesh is baked, and how (design doc
/// §45 "Navigation" — NavMesh, off-mesh links, dynamic obstacles).
///
/// Until this existed the voxel navigation mesh was CPU-only and test-only: no
/// scene could name a volume, so no shipped game could path anything. This is
/// that decision, as data — the same move SkyComponent made for the sky and
/// DestructibleComponent for breakables.
///
/// Place is the ENTITY's Transform, not a field: the origin IS the volume's
/// minimum corner (the floor), and the sizes run along +X/+Y/+Z from it. A
/// level author drags the volume like any other object, and the floor it bakes
/// is `Transform.y` — so a volume sitting on the ground bakes a ground-level
/// mesh. No second copy of the corner exists to drift from the gizmo.
///
/// The height sampler is the honest v1: a FLAT floor at the volume's own floor
/// height. Terrain is not scene-authorable (there is no Terrain component in
/// `.nfscene`), so a sampler that invented hills would be fiction the physics
/// world does not share. What the mesh DOES read from the scene is the static
/// box colliders, which become obstacles: a crate is a step, a wall is a wall.
/// A richer sampler is a caller-side extension (`ai::NavMesh::build` takes any
/// callable), not a scene-format change.
struct NavMeshComponent {
    /// Volume extents from the entity origin, world metres. Each must be
    /// positive and finite; a degenerate one is refused by the loader rather
    /// than baked into an empty or infinite mesh.
    f32 area_x = 20.0f;
    f32 area_y = 8.0f; ///< Headroom above the floor (the sky bound).
    f32 area_z = 20.0f;

    // Bake tuning, mirroring ai::NavMesh::Config one for one with the same
    // defaults, so an authored volume and a hand-built mesh agree exactly.
    f32 cell_size = 0.5f;
    f32 cell_height = 0.25f;
    f32 walkable_slope_deg = 45.0f;
    f32 walkable_climb = 0.5f;
    f32 walkable_height = 2.0f;
    f32 min_region_area = 2.0f;
    f32 agent_radius = 0.0f;
    f32 jump_distance = 4.0f;
    f32 jump_height = 1.5f;
    u32 max_verts_per_poly = 6;

    /// Off: the component survives (the author keeps the volume placed) but no
    /// mesh is baked and no agent moves. The same "switch it off, keep it
    /// placed" contract PointLightComponent uses.
    bool enabled = true;
};

/// An entity that walks the navigation mesh to a goal. Configuration plus the
/// authored goal; the live path is a runtime artefact and is never saved.
///
/// The goal is WORLD space, like a camera's placement: "walk to that door" is a
/// fact about the level, not about the agent's parent. The start is the
/// entity's own Transform, so dragging the agent in the viewport re-paths it
/// from where it now stands.
struct NavAgentComponent {
    /// Walk speed, world metres/second.
    f32 speed = 4.0f;
    /// Goal position, world metres.
    f32 goal_x = 0.0f;
    f32 goal_y = 0.0f;
    f32 goal_z = 0.0f;
    /// How close counts as arrived, world metres. A goal centred in a polygon
    /// is reached on the nose, so this only has to absorb waypoint rounding.
    f32 arrive_radius = 0.25f;
    /// Off: the agent keeps its placement and its goal but never moves — the
    /// "posed crowd" state a cinematic level wants.
    bool enabled = true;
};

/// One agent's live navigation state, as the viewport debug draw needs it.
/// A plain copy out of the runtime: position from the entity's Transform, the
/// authored goal from the component, the route from the bound path state.
/// Session state only — never saved, like the path itself.
struct NavDebugAgent {
    /// The agent's entity: the one stable key every agent has, and what the
    /// snapshot sorts by (the runtime's live map is unordered).
    ecs::Entity entity{};
    Vec3 position{};
    Vec3 goal{};
    /// Remaining route: waypoints from the one currently walked to, to the
    /// goal. Empty when the agent has no path (off the mesh, unreachable, or
    /// never stepped since adopt).
    std::vector<Vec3> path;
    bool has_path = false;
    bool at_goal = false;
};

/// Everything the navigation debug overlay draws, in one const call. Empty
/// (`has_mesh` false, no agents) when the scene carries no baked volume, so
/// the overlay draws nothing instead of a stale previous scene's mesh.
/// Agents are sorted by entity id: the runtime's live map is unordered, and an
/// overlay whose draw order flickers with pointer hashes is a flicker no test
/// can pin and no author can read.
struct NavDebugSnapshot {
    bool has_mesh = false;
    /// Bake generation the paths were planned against (see Runtime's
    /// m_nav_generation): a snapshot whose mesh was rebaked mid-frame is
    /// still self-consistent because planning and copying happen together.
    u64 mesh_generation = 0;
    /// Polygon outlines as vertex loops, one inner vector per polygon, in mesh
    /// order. The overlay strokes every loop edge.
    std::vector<std::vector<Vec3>> mesh_polygons;
    std::vector<NavDebugAgent> agents;
};

/// The mesh's polygon outlines as flat endpoint pairs (two Vec3 per edge, in
/// polygon order). Pure: the draw layer projects these with the viewport
/// camera, so the enumeration itself is testable without a window, a device
/// or ImGui. Shared edges appear once per polygon that owns them — the overlay
/// draws both strokes, which reads as a slightly brighter internal line.
std::vector<Vec3> nav_debug_mesh_edge_points(const ai::NavMesh& mesh);

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

} // namespace runtime
} // namespace nf
