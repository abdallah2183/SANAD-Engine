#pragma once

#include <NF/Assets/AssetId.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/ECS/Entity.hpp>
#include <NF/Runtime/CombatAI.hpp>
#include <NF/Runtime/Health.hpp>
#include <NF/Runtime/Suppression.hpp>
#include <NF/Runtime/Weapon.hpp>

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

/// A projected screen-space decal (Phase 36) as scene data.
///
/// The entity's Transform supplies position AND orientation: local +Z is the
/// direction the decal faces (the surface normal it was placed on), local XY
/// are the albedo's U/V plane, and the authored `size` becomes the full extent
/// of the unit box before rendering. `opacity` multiplies the material's base
/// alpha, so a material shared with an opaque surface still reads as a decal.
///
/// This header deliberately carries no rendering::DecalInstance: the loader and
/// the editor include it, and pulling NF/Rendering into both for four floats
/// would be the wrong trade (the same reason SkyComponent is flat).
struct DecalComponent {
    std::string material;                 // logical .nfmat path; empty = default
    float size_x = 1.0f;                  // full width  (local X)
    float size_y = 1.0f;                  // full height (local Y)
    float size_z = 1.0f;                  // projection depth (local Z)
    float opacity = 1.0f;                 // 0..1, multiplied with material alpha
    bool enabled = true;
};

/// A 2D sprite or billboard quad component (Phase 73).
///
/// In 2D scenes, sprites define the visual presentation on the XY plane.
/// When billboard is true, the sprite rotates in 3D to always face the camera.
struct SpriteComponent {
    std::string texture;                  // Logical path e.g. "content://Textures/hero.png"
    float width = 1.0f;
    float height = 1.0f;
    float color_r = 1.0f, color_g = 1.0f, color_b = 1.0f, color_a = 1.0f;
    float u0 = 0.0f, v0 = 0.0f;           // UV top-left
    float u1 = 1.0f, v1 = 1.0f;           // UV bottom-right
    bool flip_x = false;
    bool flip_y = false;
    int z_order = 0;                      // 2D sorting order / layer
    bool billboard = false;               // Face the active camera in 3D
    bool enabled = true;
};

/// A 2D tilemap grid component (Phase 74).
struct TilemapComponent {
    std::string tileset_texture;          // Path to atlas texture e.g. "content://Textures/tileset.png"
    float tile_size = 32.0f;              // Size of each tile in world/render units
    uint32_t columns = 16;                // Grid width in tiles
    uint32_t rows = 16;                   // Grid height in tiles
    int32_t z_order = 0;                  // 2D depth layer
    bool generate_collider = false;       // Auto-generate 2D collision boundaries for solid tiles
    bool enabled = true;
};

/// Deterministic 2D rigid body physics component (Phase 74).
struct RigidBody2DComponent {
    enum BodyType : int {
        Dynamic = 0,
        Static = 1,
        Kinematic = 2
    };

    int body_type = Dynamic;              // Dynamic (0), Static (1), Kinematic (2)
    float mass = 1.0f;                    // Mass in kg (for dynamic bodies)
    float linear_damping = 0.0f;          // Linear velocity damping [0, inf)
    float angular_damping = 0.05f;        // Angular velocity damping [0, inf)
    bool fixed_rotation = false;          // Prevent body from rotating
    bool enabled = true;
};

/// 2D collider attached to an entity with or without RigidBody2D (Phase 74).
struct Collider2DComponent {
    enum Shape : int {
        Box = 0,
        Circle = 1
    };

    int shape = Box;                      // Box (0) or Circle (1)
    float size_x = 1.0f;                  // Box width or half-width
    float size_y = 1.0f;                  // Box height or half-height
    float radius = 0.5f;                  // Circle radius
    float friction = 0.2f;                // Surface friction [0, 10]
    float restitution = 0.0f;             // Bounciness [0, 1]
    bool is_sensor = false;               // Trigger-only without physical collision
    bool enabled = true;
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

/// Values for PostProcessComponent::dynamic_resolution_timing. The component
/// stores an int rather than the rendering enum, following `tonemap`, so this
/// header stays free of renderer types. The loader maps names, and Runtime maps
/// these values to rendering::DynamicResolutionTiming at extraction.
inline constexpr int kDynamicResolutionTimingCpu = 0;
inline constexpr int kDynamicResolutionTimingGpu = 1;

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

    // Anti-aliasing (FXAA). Off by default, like every other optional stage:
    // the pass is an addition, and a scene that never names it renders the
    // frame it did before FXAA existed. When on, tonemap renders into an
    // intermediate image and a second pass resolves that image into the
    // output target (see Renderer3D::set_fxaa_enabled).
    bool fxaa_enabled = false;
    /// Relative luma range that counts as an edge. 1/8 is the classic FXAA
    /// value: lower filters more (and softens more), higher filters less.
    float fxaa_threshold = 0.125f;
    /// Absolute floor for that range, so a dark low-contrast area is not read
    /// as an edge and made to shimmer.
    float fxaa_threshold_min = 0.0312f;

    // Internal render scale (Phase 37). 1.0 renders one 3D texel per output
    // pixel; above 1 is supersampling (2.0 = exact 2x2 box average per output
    // pixel in the tonemap read), below 1 is a cheaper render that read
    // upscales. This is a property of the image, which is why it lives on the
    // post-process block: the tonemap pass is what consumes the extra texels.
    // It is NOT MSAA (that resolves coverage per sample, and is a separate
    // RHI item). The bounds match rendering::kMinRenderScale/kMaxRenderScale.
    float render_scale = 1.0f;
    /// Let the governor lower the scale when frames run long. `render_scale`
    /// is the ceiling it may return to; `min_render_scale` is the floor. Off by
    /// default, so nothing moves unless the scene asks for it.
    bool dynamic_resolution = false;
    /// Frame budget for the governor, in frames per second. 60 means it tries
    /// to hold 16.67 ms per frame.
    float target_fps = 60.0f;
    /// Lowest scale the governor may choose. Must be <= render_scale.
    float min_render_scale = 0.5f;
    /// Which clock drives the governor's load decision: CPU frame time
    /// (`kDynamicResolutionTimingCpu`) or real GPU timestamps when a fresh
    /// sample exists (`kDynamicResolutionTimingGpu`). GPU timing is a
    /// preference and falls back to CPU; the cooldown is CPU wall time in both
    /// modes.
    int dynamic_resolution_timing = kDynamicResolutionTimingCpu;

    /// Decide the render scale from the FRAME WINDOW instead of the per-frame
    /// governor (Phase 42): the 1% low lowers it, the average plus the 1% low
    /// raise it. Requires a host that feeds `Runtime::set_frame_window`; the
    /// player does that from its own `FramePacing` window. Off by default, and
    /// when off the governor above is exactly as it was.
    bool frame_policy = false;

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

/// Combat feedback as scene data (Phase 56).
///
/// A scene with a `Weapon:` line already publishes shots and impacts, but a
/// scene never asked for them to be *shown*. This component is the opt-in:
/// absent means the runtime draws nothing new and mixes no new sound, so every
/// level authored through 0.5.0 keeps the exact frame it had. A present line
/// turns the event streams into a muzzle flash, a tracer, a scorch decal and a
/// procedural one-shot.
///
/// The first enabled component wins, like `Sky:` and `PostProcess:`. Fields are
/// durations in seconds, sizes in metres and volumes in linear gain. A volume of
/// zero is "silent" (the cue is not produced); durations and sizes must stay
/// positive, because a zero lifetime would be an effect that is never seen.
struct FeedbackComponent {
    bool enabled = true;
    /// Procedural shot/impact one-shots. Off keeps the visuals.
    bool audio = true;

    /// Per-volley muzzle flash lifetime and cube edge length.
    float flash_seconds = 0.055f;
    float flash_size = 0.22f;

    /// Blast flash (Phase 59): the larger, slower flash an explosive impact
    /// spawns at the point of impact. One per blast, never one per splash
    /// victim.
    float blast_flash_seconds = 0.16f;
    float blast_flash_size = 0.9f;

    /// Per-pellet tracer beam lifetime, visible run and thickness.
    float tracer_seconds = 0.09f;
    float tracer_length = 9.0f;
    float tracer_width = 0.025f;

    /// Scorch decal lifetime and edge length on the hit surface.
    float impact_seconds = 6.0f;
    float impact_size = 0.14f;

    /// One-shot gains, before the SFX bus and master volume. `impact_volume` is
    /// the ballistic click; Phase 59 gave the explosive and fire channels their
    /// own gain so a blast can be loud without making every bullet loud.
    float shot_volume = 0.7f;
    float impact_volume = 0.45f;
    float impact_volume_explosive = 0.75f;
    float impact_volume_fire = 0.55f;

    /// Muzzle/blast light (Phase 59). Each live flash also contributes a
    /// transient point light, so a shot actually brightens the wall it is fired
    /// at instead of only adding an emissive cube. `flash_light=false` restores
    /// the 0.6.0 look exactly.
    bool flash_light = true;
    float flash_light_intensity = 3.0f;
    float flash_light_radius = 7.0f;
    float flash_light_r = 1.0f;
    float flash_light_g = 0.72f;
    float flash_light_b = 0.35f;

    /// Hard per-frame caps (Phase 59 made them scene data). The director drops
    /// the oldest effect when a list is full, deterministically.
    unsigned max_flashes = 48u;
    unsigned max_tracers = 96u;
    unsigned max_impacts = 192u;
    unsigned max_sounds = 16u;
    unsigned max_lights = 4u;
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

    // --- Aiming down sights (ADS) ----------------------------------------
    //
    // Holding the `aim` action pulls the camera in and slows the look. Off by
    // default: a scene that never authors it behaves exactly as it did, and
    // a scene that turns it on needs no new bindings — `aim` is already an
    // action a binding file can point at any key, pad button or mouse button.
    //
    // The maths is in NF/Runtime/PlayerLook.hpp (aim_blend_step and friends).

    /// Whether holding `aim` does anything.
    bool ads_enabled = false;

    /// The distance the camera settles at while aiming, in metres. Closer
    /// than `distance` is the whole point: it is what makes a target bigger.
    float ads_distance = 2.2f;

    /// Look sensitivity while aiming, as a MULTIPLIER on the authored
    /// sensitivity. 1.0 is "no change"; 0.55 is the usual scope feel.
    float ads_sensitivity = 0.55f;

    /// How fast the blend reaches its target, per second. 0 snaps.
    float ads_speed = 12.0f;

    // --- Recoil -----------------------------------------------------------
    //
    // A kick is an offset on the VIEW, not on the aim the player built with
    // the mouse. Scripts add one with nf.recoil(pitch, yaw); it recovers on
    // its own, and the view returns to exactly where the player was aiming.
    // The maths is in NF/Runtime/PlayerLook.hpp (recoil_add / recoil_recover).

    /// Fraction of the remaining kick recovered per second. 0 holds it.
    float recoil_recover = 8.0f;

    /// The largest kick this camera will show, in degrees per axis. A burst
    /// that adds more than this is clamped rather than pointing at the sky.
    float recoil_max = 12.0f;

    // --- Camera shake -----------------------------------------------------
    //
    // Trauma-based shake (NF/Rendering/CameraShake.hpp): scripts add trauma
    // with nf.shake(amount), it decays, and the visible offset scales with
    // trauma^2. These two fields are the scene's trim on that.

    /// Multiplier on the shake offset. 0 disables shake for this camera
    /// without touching the scripts that ask for it.
    float shake_strength = 1.0f;

    /// Trauma lost per second. Higher is a shorter, snappier hit.
    float shake_decay = 1.4f;
};

/// Ground the navigation mesh can actually walk on (Phase 63).
///
/// Until this existed the bake's height sampler was a FLAT floor at the volume's
/// own height, and the comment above said why: "terrain is not scene-authorable
/// (there is no Terrain component in .nfscene), so a sampler that invented hills
/// would describe a world the physics does not simulate". This is that
/// component. The same `rendering::HeightField` and `rendering::terrain_height`
/// that answer the navmesh's queries also build the mesh the player sees, so the
/// surface an NPC walks on is the surface on screen.
///
/// The height source is authored data: a `heightmap=` image when it decodes,
/// deterministic value noise otherwise. A `heightmap=` that cannot be read falls
/// back with a log line rather than failing the scene — the contract the
/// `--heightmap` flag keeps in the Basic3D sample.
struct TerrainComponent {
    bool enabled = true;

    /// Vertices per side of the mesh, and the world extent in X and Z. The
    /// terrain is centred on the entity's own Transform, so dragging the entity
    /// in the editor moves the ground with it.
    unsigned resolution = 64u;
    f32 area = 100.0f;

    /// Multiplies the 0..1 field. 0 is flat ground, which is the honest way to
    /// author a plane that still participates in navigation.
    f32 height_scale = 10.0f;
    f32 noise_frequency = 0.03f;
    unsigned octaves = 4u;
    unsigned seed = 1u;

    /// `content://Textures/*.png` (or any decodable image) when authored; empty
    /// means procedural noise.
    std::string heightmap;

    /// The material the ground is drawn with. Empty means the renderer's
    /// default, which is what a scene that only wanted collision-free hills
    /// gets. It is a path, not a handle, for the same reason `MeshComponent`
    /// stores one: the runtime resolves it once and the scene file stays text.
    std::string material;
};

/// One authored off-mesh link (Phase 63): a jump, a climb, a ladder, a teleport
/// pad. Stored on the navigation volume because it belongs to the mesh that
/// volume bakes, and consumed by `ai::NavMesh::build`, which drops a link whose
/// endpoint is not on the mesh and counts it rather than teleporting an agent
/// into the void.
struct NavLinkSpec {
    f32 start_x = 0.0f, start_y = 0.0f, start_z = 0.0f;
    f32 end_x = 0.0f, end_y = 0.0f, end_z = 0.0f;
    /// Both directions when true. A one-way link is a drop-down or a
    /// jump-up-only ledge: A* honours the direction, so an agent does not path
    /// back up a pit it jumped into.
    bool bidirectional = true;
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

    /// Authored off-mesh links (Phase 63): a jump, a climb, a ladder, a
    /// teleport pad. They belong to the mesh this volume bakes, which is why
    /// they live here rather than in a component of their own.
    std::vector<NavLinkSpec> links;

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

/// One authored destination on a patrol route (Phase 64). Position is world
/// space, like the agent's goal: a patrol is a fact about the level, not about
/// the entity's parent. `wait` is seconds to pause at THIS point; a negative
/// value means "use the route's own wait=" (the loader writes a bare
/// `NavPoint:` line when the point has no wait of its own).
struct NavPointSpec {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 wait = -1.0f;
};

/// When a patrol pauses for what its senses know (Phase 65).
///
/// This is deliberately a tiny vocabulary rather than a script hook: a guard
/// that stops walking when it spots someone is the one patrol behaviour every
/// game wants, and the frame already has the senses to answer it. A route that
/// never writes `hold=` keeps the Phase 64 behaviour exactly.
enum class NavRouteHoldPolicy : u8 {
    /// Never hold: the patrol is a pure walk, as it was in Phase 64.
    Off = 0,
    /// Stop while the walker's perception KNOWS a threat, including the linger
    /// window after the threat broke line of sight. This is "a guard stands
    /// still while it watches / remembers someone".
    Aware = 1,
    /// Stop only while a sense is reading a threat THIS frame. A remembered but
    /// unseen threat does not hold the patrol — this is "the guard pauses at
    /// the moment it hears or sees something, then keeps walking".
    Sensing = 2,
};

/// Stable technical name for the scene format and the logs.
[[nodiscard]] const char* nav_route_hold_name(NavRouteHoldPolicy hold) noexcept;
/// Parses the name the `NavRoute:` line carries. False when it is not one.
[[nodiscard]] bool nav_route_hold_from_name(std::string_view name,
                                            NavRouteHoldPolicy& out) noexcept;

/// An authored patrol route for the NavAgent on the same entity (Phase 64).
///
/// Without this component the agent walks to its `NavAgent:` goal exactly as it
/// did before — so a scene that never heard of patrols behaves and round-trips
/// byte for byte. With an enabled route of at least two points, the points are
/// the destinations in line order and the goal is ignored while the route
/// runs (a one-point route has nothing to patrol; the validator says so).
///
/// The live point index, direction and wait belong to the session, exactly like
/// the path: they are reset when the scene is adopted and never written to
/// file. Editing a point's position re-paths from the current one; changing the
/// route's shape restarts the patrol at the first point.
struct NavRouteComponent {
    /// Destinations in walk order. `NavPoint:` lines append here; the order is
    /// the route.
    std::vector<NavPointSpec> points;
    /// Seconds to pause at every point that does not override it.
    f32 wait = 0.0f;
    /// After the last point, walk back to the first: a closed patrol.
    bool loop = true;
    /// Reverse at the ends instead of wrapping: A-B-C-B-A. Wins over `loop`
    /// when both are set; the validator points out the redundant pair rather
    /// than silently choosing one.
    bool ping_pong = false;
    /// Off: the route stays authored and placed but the agent falls back to its
    /// goal, the same "switch it off, keep it" contract the other components
    /// keep.
    bool enabled = true;

    /// Perception gate (Phase 65). Off by default, so a route authored before
    /// this phase walks, saves and loads exactly as it did.
    NavRouteHoldPolicy hold = NavRouteHoldPolicy::Off;

    /// Seconds to KEEP holding after the gate last saw a threat. This is the
    /// hysteresis that stops a patrol from stuttering while a target flickers
    /// in and out of the sight cone. Zero resumes on the first frame the gate
    /// is clear.
    f32 resume = 0.0f;

    /// Seconds to search the last known position after a hold ends (Phase 66).
    /// The walker leaves the route, walks to where the threat was last known,
    /// looks around for this long, then continues the point it was on. Zero
    /// disables the detour, so a Phase 65 route is unchanged.
    f32 investigate = 0.0f;

    /// Squad token for shared searches (Phase 67). A non-empty token makes
    /// every patrol with the same token answer another member's investigation:
    /// when one walker starts searching a last known position, the others leave
    /// their own routes for the same point, scan for their own `investigate=`
    /// seconds, then resume. Empty keeps the solo Phase 66 behaviour.
    std::string squad;
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
    /// True while the agent is following an authored route rather than its
    /// `NavAgent:` goal. The route's live cursor is session state; the points
    /// themselves are scene data (NavRouteComponent).
    bool on_route = false;
    /// Index of the route point being walked to, or waited at.
    u32 route_point = 0;
    /// True while a perception gate (Phase 65) is keeping the patrol still.
    /// A held walker has a route and a goal but is not consuming either.
    bool route_held = false;
    /// True while the walker is walking to, or scanning, the last known
    /// position of the threat that held it (Phase 66).
    bool route_investigating = false;
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

/// A gun, as scene data (Phase 44).
///
/// The profile is the authored half — rate of fire, magazine, damage, spread,
/// recoil — and it round-trips through the file. The state is the session half:
/// rounds left, the cooldown clock, the burst queue, the spread bloom. It is
/// never written, because "seven rounds left" is not a property of a level.
///
/// The entity that carries this and is followed by the active camera is the
/// player's weapon: the runtime feeds it the `fire` and `reload` actions and
/// traces its shots from the camera. Every other weapon still ticks its clocks
/// (a reload finishes whether or not anyone is holding the gun) but fires only
/// when a script asks it to, which is what keeps an NPC's gun from shooting on
/// the player's trigger.
struct WeaponComponent {
    WeaponProfile profile{};
    /// Filled by the constructor from the profile, then stepped by the runtime.
    WeaponState state{};

    WeaponComponent() : state(weapon_state(profile)) {}
    explicit WeaponComponent(const WeaponProfile& p) : profile(p), state(weapon_state(p)) {}
};

/// An NPC that fights (Phase 45).
///
/// The agent names its target rather than carrying a team: a scene says
/// `CombatAgent: target=Player`, and that is the whole social model. It is
/// deliberately smaller than a team/faction system, because "who is hostile"
/// is a game's decision and a scene format that guesses it would be wrong for
/// every game that disagrees.
///
/// The entity needs a `WeaponComponent` (what it shoots with), a Transform
/// (where it stands) and the target needs a `HealthComponent` and a collider —
/// the collider is what the visibility ray hits, so a target without one is a
/// target nobody can see.
struct CombatAgentComponent {
    /// The name of the entity to shoot at. Empty = nothing to shoot.
    ///
    /// Ignored when `auto_target` is true: the agent then chooses its own
    /// hostile from the factions in the scene.
    std::string target;

    /// Choose the nearest visible entity on a hostile team instead of reading
    /// `target` (Phase 47). Requires a non-empty `Faction: team=` on the agent;
    /// without a side there is no "hostile" to look for.
    ///
    /// The chosen entity is held while it stays alive, in range and visible.
    /// Switching targets resets the reaction clock, so an agent cannot snap
    /// from one enemy to another and fire in the same frame.
    bool auto_target = false;

    /// Eye height above the entity's origin, in metres. The muzzle is here:
    /// an agent whose origin is at its feet shoots from its head.
    f32 eye_height = 1.5f;

    /// Where the target is aimed at, above ITS origin. 1.0 is roughly a chest;
    /// aiming at the feet makes every agent shoot the floor.
    f32 target_height = 1.0f;

    CombatAimConfig config{};

    /// Runtime state: the aim between frames. Never saved — a loaded scene
    /// starts with the agent facing wherever its Transform faces.
    CombatAimState state{};

    /// Runtime state for auto-targeting: the entity currently held, so the
    /// agent does not flicker between two equidistant enemies every frame.
    /// Never saved; a copied agent starts with no lock.
    ecs::Entity locked_target{};

    /// Runtime target motion for projectile lead (Phase 48). The position is
    /// the aim point from the previous frame, and the velocity is derived from
    /// its change. Session data: never saved, reset when the target changes.
    Vec3 last_target_position{0.0f, 0.0f, 0.0f};
    Vec3 target_velocity{0.0f, 0.0f, 0.0f};
    bool has_target_history = false;

    /// Fight back against whoever last damaged this entity (Phase 55). The
    /// ledger supplies the attacker; the agent still requires it to be alive,
    /// in range, visible and allowed by the faction rules. Off by default so a
    /// scene that named a target keeps that target until it opts in.
    bool retaliate = false;

    /// Runtime suppression from near misses (Phase 55). Never saved; a loaded
    /// or copied agent starts calm.
    SuppressionState suppression{};

    bool enabled = true;
};

/// An entity's affiliation (Phase 46).
///
/// Deliberately a separate component rather than a field on Health or Weapon:
/// a team belongs to the actor, not to its hit points, and a crate with no gun
/// may still be owned by a side. The rules live in Factions.hpp; this struct is
/// only what the scene stores.
///
/// No component, or an empty `team`, is neutral. That is the pre-Phase-46
/// behaviour and the reason adding this component does not silently change
/// damage in every existing scene.
struct FactionComponent {
    /// Team name. Free text, case-sensitive, and empty means "no affiliation".
    /// It is not an entity name: two unrelated scenes can both call a side
    /// "Blue" without sharing anything at runtime.
    std::string team;

    /// The attacker's licence to hurt its own team. It is read from the
    /// SHOOTER, never the victim: a target cannot make itself shootable.
    bool friendly_fire = false;

    /// Off is the same as having no faction at all: neutral to everyone.
    bool enabled = true;
};

/// What an entity can notice (Phase 60, design §45 "AI Core: perception").
///
/// `ai::Perception` was written and tested in Phase 22 — four senses, a
/// time-integrated detection meter per sense, a linger window, a last known
/// position — and no scene could reach any of it. The audit that produced
/// Phase 25 named the reason and left it open: "individual AI (perception,
/// machines, utility) stayed outside the frame on purpose: it needs boards
/// bound to entities, which is a design, not a wiring job". This component is
/// that design's first half. The second half is the frame, which now feeds it
/// the scene's cast and its own combat events.
///
/// The component declares a SENSOR. What there is to sense is not authored here:
/// every entity with a Transform and something alive about it (hit points, a
/// weapon, a brain or a faction) becomes a target, and the frame's shot and
/// impact events become the noises and the pain.
///
/// Sight is what an author tunes, so it carries its own five fields. The other
/// senses are ranges only, on the module's documented defaults: hearing and
/// touch are omnidirectional, damage is neither attenuated nor gradual (a hit
/// from a sniper you never saw still tells you someone is out there), and all
/// three share sight's gain and loss. A range of 0 disables a sense outright — a
/// deaf NPC is `hearing=0`, not a flag on every sound in the world.
struct PerceptionComponent {
    bool enabled = true;

    /// Sight: how far it reaches, how wide the cone is (half angle, in degrees),
    /// how fast the meter fills, how fast it drains once the target is gone, and
    /// how many seconds a target stays KNOWN after every sense has gone quiet.
    /// The linger is the search window: "it went behind that wall" is knowledge
    /// for `linger` seconds, not for one frame.
    f32 sight_range = 30.0f;
    f32 sight_half_fov_deg = 60.0f;
    f32 sight_gain = 2.0f;
    f32 sight_loss = 0.6f;
    f32 sight_linger = 3.0f;

    /// Range of the other senses, in metres. 0 disables one outright.
    f32 hearing_range = 18.0f;
    f32 touch_range = 1.5f;

    /// The damage sense: notice whoever hurt you, from anywhere, with no
    /// attenuation. On by default because being shot is not something an NPC
    /// should have to roll for.
    bool pain = true;

    /// Continuous noise this entity emits, in 0..1 — footsteps, an engine, a
    /// fire. Discrete noises are the frame's own shot events, which is why this
    /// is a rate and not a flag.
    f32 noise = 0.0f;

    /// Multiplier on how easily this entity is SEEN, 0..1. Camouflage slows
    /// detection rather than preventing it: a staring guard still fills the
    /// meter, which is the behaviour a partially concealed flank should have.
    f32 visibility = 1.0f;

    /// Body radius, in metres, added to a sensor's own for the range tests — a
    /// tall actor is visible a little before its centre clears a corner.
    f32 radius = 0.5f;

    /// Where the eyes are, in metres above the entity's origin. The sensor looks
    /// FROM here, and a sight ray starts here: a guard whose origin is at its
    /// feet sees over the crate its body is behind.
    f32 eye_height = 1.5f;

    /// When true and the entity also carries a `CombatAgent:`, the agent may
    /// only engage entities its own senses are aware of, and aims at the last
    /// known position while it knows of a target it can no longer see. False
    /// keeps the pre-Phase-60 rule exactly: an agent with a clear ray fires.
    bool governs_combat = true;
};

/// Squad radio (Phase 68): share the contacts this entity personally senses
/// with same-token members inside range.
///
/// A radio is deliberately narrower than the Phase 67 squad search board: it
/// carries **contacts**, not searches. Every frame each enabled member reports
/// the live targets its own senses are reading, with the last position it knows.
/// A receiver gets awareness and that position — never a sense, never detection
/// — so it can hold a patrol, investigate, or turn towards the report, but it
/// cannot fire at what it has not sensed and it cannot relay the report onward.
/// That anti-amplification rule is what keeps a chain of radios from spreading
/// one glimpse across the whole level.
///
/// The token is one case-sensitive word of at most 64 characters. An empty
/// token is inert and writes no scene line, exactly like an empty `Faction:`
/// team. `radio=0` keeps the component authored but shares nothing.
///
/// Phase 69 adds a final callout: when a contact this member PERSONALLY sensed
/// stops being sensed, it reports the last position it held once, with `lost`
/// seconds of memory on the receiver. That report is still derived from a
/// personal sighting, never from radio traffic, so the anti-relay rule does not
/// change. `lost=0` keeps the Phase 68 behaviour exactly.
///
/// Phase 71 adds a directional antenna: `cone` is the horizontal half-angle the
/// antenna can talk and listen through, measured around the owner's facing. A
/// delivery now needs BOTH ends to face each other as well as to reach, exactly
/// like the range test: a directional scout cannot hear a mate behind it, and a
/// mate behind cannot hear it. `cone=0` keeps the omnidirectional Phase 70
/// antenna byte for byte. Direction changes delivery only; a received report
/// remains knowledge and is still never relayed.
struct SquadComponent {
    /// Who hears whom. Two radios talk when the token matches exactly.
    std::string token;

    /// Link range in metres. A delivery needs the distance to fit **both**
    /// radios, so a short-range scout cannot shout past its own antenna.
    f32 radio = 30.0f;

    /// Seconds of memory a lost-contact callout leaves on a receiver, and the
    /// reason the callout happens at all. Zero (the default) sends none: the
    /// radio shares only what is being sensed this frame, as in Phase 68. The
    /// receiver's own `linger` is still a floor, so a longer personal search
    /// window is never shortened.
    f32 lost = 0.0f;

    /// Horizontal antenna half-angle in degrees, `[0, 180]`. Zero (the
    /// default) is omnidirectional and preserves the Phase 70 radio. A positive
    /// value makes this end directional: the other radio must lie inside this
    /// cone around the owner's facing, so a link needs both cones to agree.
    /// The facing is the entity's Transform yaw, or a `CombatAgent`'s current
    /// aim when it has one — the same rule its senses follow.
    f32 cone = 0.0f;

    /// Off keeps the component authored and inert, the same contract every
    /// other component's enabled flag keeps.
    bool enabled = true;
};

/// A radio jammer (Phase 70).
///
/// One optional `Jammer: radius=<metres> strength=<fraction>` line on any
/// entity turns its world position into a field that degrades every squad
/// radio inside it. An antenna inside a field loses `strength` of its reach,
/// and a delivery needs BOTH antennas to reach, so a field over either end of
/// a link can cut it. The strongest field covering an antenna wins; fields do
/// not add up. There is no team filter: a jammer jams its owner's radio too,
/// which makes placement a choice rather than a free buff.
///
/// `radius=0` or `strength=0` is inert, and a scene with no line is exactly
/// the Phase 69 radio. Jamming is radio-only: it never touches the Phase 67
/// `squad=` search board, a personal sense, or the lost-callout bookkeeping —
/// it only decides whether this frame's delivery arrives.
struct JammerComponent {
    /// Reach of the field in metres, `[0, 10000]`. 0 jams nothing.
    f32 radius = 12.0f;

    /// Fraction of an antenna's radio range removed inside the field, `[0, 1]`.
    /// 1 silences a radio completely; 0 is inert.
    f32 strength = 1.0f;

    /// Off keeps the field authored and inert.
    bool enabled = true;
};

/// What a brain action reads (Phase 61).
///
/// The frame fills one number per input from the world it already has — the
/// entity's own senses, hit points, magazine and suppression, and the distance
/// to whoever it is dealing with. A name this enum does not cover is refused by
/// the loader rather than scored as a zero forever: an action that can never win
/// is a typo, not a strategy.
enum class BrainInput : u8 {
    /// 1 when the entity's perception knows its current target, else 0.
    Aware = 0,
    /// 1 when a sense is reading that target THIS frame, else 0.
    Sensing = 1,
    /// Metres to the target; 0 when there is none.
    Distance = 2,
    /// Hit points as a fraction of the maximum, 0..1.
    Health = 3,
    /// Rounds in the magazine as a fraction of its capacity, 0..1.
    Ammo = 4,
    /// Suppression from near misses, 0..1.
    Threat = 5,
    /// How many actors the entity is currently aware of, 0..N.
    Enemies = 6,
};

/// Stable technical name for the scene format and the logs.
[[nodiscard]] const char* brain_input_name(BrainInput input) noexcept;
/// Parses the name the `BrainAction:` line carries. False when it is not one.
[[nodiscard]] bool brain_input_from_name(std::string_view name, BrainInput& out) noexcept;

/// How many inputs the vocabulary has. The frame's per-entity input vector is
/// indexed by BrainInput, so this is the array bound.
inline constexpr u32 kBrainInputCount = 7u;

/// One consideration: "I want to <name> this much".
///
/// The desirability is the module's own arithmetic —
/// `weight * clamp(output_offset + slope * (input - input_offset)^exponent)` —
/// so `ai::UtilitySystem` is doing the choosing and this struct is only the
/// scene's copy of it. The four curve fields are what a designer turns; the
/// `curve=` preset on the line is a shorthand that sets them, and an explicit
/// field always wins over the preset.
struct BrainAction {
    /// What the decision is called. Free text; what Lua and the editor read, and
    /// what the log says. Not interpreted by the engine.
    std::string name;

    BrainInput input = BrainInput::Health;

    /// ResponseCurve, field for field. `slope` negative inverts the curve
    /// (distance becomes "closer is worse", which is what fleeing wants);
    /// `exponent` 2 is "distance barely matters until it does"; a large slope
    /// with an input offset is a step.
    f32 slope = 1.0f;
    f32 exponent = 1.0f;
    f32 input_offset = 0.0f;
    f32 output_offset = 0.0f;

    /// Flat multiplier on the shaped value, so "this creature cares twice as
    /// much about food" stays separable from the curve's shape.
    f32 weight = 1.0f;

    /// While this action is the winner, the entity's combat agent holds its
    /// trigger. Aiming continues — a guard that has decided not to shoot still
    /// tracks what it is looking at.
    bool hold_fire = false;
};

/// One state of a scene machine (Phase 62). A state is a marker with an effect:
/// the engine does not invent what "patrol" does, it publishes the current state
/// and applies the one thing a level needs from it — `hold_fire`.
struct StateSpec {
    /// Free text, one token, and what Lua and the editor read back.
    std::string name;
    /// While this state is current, the entity's combat agent holds its trigger.
    bool hold_fire = false;
};

/// "When in `from` and `when` holds, go to `to`."
///
/// The guard is one of the brain's inputs compared against `at`, rather than an
/// expression language: a scene format that could compute anything would need a
/// parser, a scope and an error model, and every one of those is a place for a
/// level to be subtly wrong. `when=aware` (at 0.5), `when=health below=true
/// at=0.3` and `when=distance at=6 below=true` are the four sentences a patrol
/// actually needs.
struct TransitionSpec {
    std::string from;
    std::string to;
    BrainInput when = BrainInput::Aware;
    /// The threshold the input is compared against.
    f32 at = 0.5f;
    /// "value <= at" instead of "value >= at". Distance and hit points read
    /// better this way round; awareness and enemy count read better the other.
    bool below = false;
};

/// A finite state machine as scene data (Phase 62, design §45 "AI Core: state
/// machines").
///
/// The third and last member of the trio this engine kept writing and never
/// wiring: `ai::StateMachine` has been unit-tested since Phase 22 (16 cases) and
/// reachable from nothing. Utility scoring answers "which consideration pulls
/// hardest right now"; a machine answers "what am I doing", where the SEQUENCE
/// is the behaviour — patrol, suspicious, chase, search, return.
///
/// The inputs are the same seven the brain uses, computed by the same function,
/// so the two layers read one world through one vocabulary.
struct StateMachineComponent {
    bool enabled = true;

    /// Seconds between ticks. 0 ticks every frame, which a machine can afford:
    /// one comparison per transition.
    f32 rate = 0.0f;

    /// The state the machine starts in. Empty means the first declared one, so a
    /// scene that lists its states in order does not have to name it twice.
    std::string initial;

    std::vector<StateSpec> states;
    /// Evaluated in line order, and the FIRST guard that holds wins — the module
    /// documents authoring order as the tie-break, so the order is behaviour.
    std::vector<TransitionSpec> transitions;
};

/// The decision layer (Phase 61, design §45 "AI Core: utility AI").
///
/// Perception (Phase 60) answers "what do I know"; this answers "what do I want
/// to do about it". `ai::UtilitySystem` and its response curves have been
/// unit-tested since Phase 22 and reachable from nothing, the same gap the
/// perception half had: no scene could name a consideration, and no frame turned
/// the world into the numbers a consideration is a function of.
///
/// A `Brain:` line declares the entity's decision maker and one `BrainAction:`
/// line per consideration. The frame computes the inputs, scores every action in
/// insertion order (ties to the earlier one) and remembers the winner, which the
/// game reads through Lua or the runtime — the engine does not invent what
/// "advance" means, it publishes the choice.
struct BrainComponent {
    bool enabled = true;

    /// Seconds between evaluations. 0 evaluates every frame. A brain that
    /// re-decides every frame is a brain that flickers between two nearly equal
    /// utilities, and the rate is the cheapest cure there is.
    f32 rate = 0.25f;

    /// The considerations, in insertion order — which is also the tie-break
    /// order, so the list is part of the behaviour and not a set.
    std::vector<BrainAction> actions;
};

} // namespace runtime
} // namespace nf
