// Samples/MedievalVillage/main.cpp — "Medieval Village: Harvest Run"
//
// A third-person collection game built inside NOVAForge on the Medieval Village
// MegaKit[Standard]. See DESIGN.md beside this file for the design rationale.
//
// The goal: the harvest festival needs its six supply crates brought to the
// wagon in the village square before dusk. Crates are scattered around and
// behind the four houses; the player carries at most three at a time, so the
// run is two trips. Doors swing open, the camera is a third-person orbit, and
// the HUD is the engine's own ui::GameFlow driven into an ImGui overlay.
//
// What this sample deliberately exercises, and why:
//   * Assets — the kit's glTF pieces go through Tools/import_kit.sh into the
//     engine's cooked mesh format and are loaded back through
//     assets::MeshAsset -> rendering::make_static_mesh -> MeshLibrary -> GPU.
//     Nothing is faked with primitives: every wall, roof, door and crate in the
//     village is a real kit mesh with its own BaseColor map.
//   * Physics — a real physics::CharacterController on a physics::PhysicsWorld,
//     with static box colliders generated from each placed piece's measured
//     world AABB, so walls actually stop the player.
//   * Gameplay — gameplay::QuestLog and gameplay::Inventory carry the objective
//     state, so the HUD's numbers come from the engine's gameplay layer.
//   * UI — ui::GameFlow / ui::Hud / ui::Menu own every screen; UiOverlay is the
//     missing runtime draw backend that turns their ui::DisplayList into pixels.
//
// Controls: WASD/arrows move, Shift sprint, Space jump, E interact,
//           right-mouse-drag or Q/E-camera... see --help.
//
// Run:
//   NFSampleMedievalVillage.exe [--frames N] [--validation] [--autoplay]
//                               [--lang en|ar] [--content <dir>]

#include "KitAssets.hpp"
#include "UiOverlay.hpp"
#include "Village.hpp"

#include <NF/Assets/MeshAsset.hpp>
#include <NF/Core/Logger.hpp>
#include <NF/Core/Time.hpp>
#include <NF/ECS/ECS.hpp>
#include <NF/Gameplay/Inventory.hpp>
#include <NF/Gameplay/Quest.hpp>
#include <NF/Physics/CharacterController.hpp>
#include <NF/Physics/PhysicsWorld.hpp>
#include <NF/Platform/InputSystem.hpp>
#include <NF/Platform/Platform.hpp>
#include <NF/Platform/Window.hpp>
#include <NF/RHI/RHI.hpp>
#include <NF/Rendering/Camera.hpp>
#include <NF/Rendering/Components.hpp>
#include <NF/Rendering/MeshLibrary.hpp>
#include <NF/Rendering/Renderer3D.hpp>
#include <NF/Rendering/StaticMesh.hpp>
#include <NF/Scene/Transform.hpp>
#include <NF/UI/GameUI.hpp>
#include <NF/UI/Localization.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace nf::sample::medieval {

#ifndef NF_MEDIEVAL_CONTENT_DIR
#define NF_MEDIEVAL_CONTENT_DIR ""
#endif
#ifndef NF_BASIC3D_SHADER_DIR
#define NF_BASIC3D_SHADER_DIR ""
#endif
#ifndef NF_EDITOR_IMGUI_SHADER_DIR
#define NF_EDITOR_IMGUI_SHADER_DIR ""
#endif

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

// --- tuning -----------------------------------------------------------------
constexpr int kCrateTotal = 6;         // the harvest needs six crates
constexpr int kCarryCapacity = 3;      // ... and the player carries three at a time
constexpr float kRoundSeconds = 240.0f;// dusk
constexpr float kInteractRange = 2.6f; // metres, player centre to interactable
constexpr float kSprintDrain = 28.0f;  // stamina per second
constexpr float kStaminaRegen = 18.0f;
constexpr float kWalkSpeed = 5.0f;
constexpr float kSprintSpeed = 8.6f;

// HUD widget ids. The canvas is looked up by id every frame rather than through
// a cached reference: Canvas::add_* returns a reference into a std::vector, and
// adding the next widget of the same kind reallocates it. A held reference to
// the first bar dangles the moment the second bar is added.
constexpr const char* kObjectiveId = "mv_objective";
constexpr const char* kDeliveredBarId = "mv_delivered";
constexpr const char* kClockBarId = "mv_clock";
constexpr const char* kCarryId = "mv_carry";
constexpr const char* kPromptId = "mv_prompt";
constexpr const char* kHelpId = "mv_help";

// --- options ----------------------------------------------------------------

struct Options {
    u32 width = 1280;
    u32 height = 720;
    u32 max_frames = 0;      // 0 = run until the window closes
    bool validation = false;
    bool autoplay = false;   // drives the player automatically (verification)
    bool english = false;
    std::string content_dir;
    std::string shader_dir;
    std::string imgui_shader_dir;
};

void print_help() {
    std::cout <<
        "NOVAForge — Medieval Village: Harvest Run\n"
        "  --frames N        Run N frames then exit (0 = until the window closes)\n"
        "  --validation      Enable Vulkan validation layers\n"
        "  --autoplay        Drive the player automatically (end-to-end check)\n"
        "  --lang en|ar      HUD language (default ar)\n"
        "  --content <dir>   Kit content directory (default: the source tree)\n"
        "  --help            This text\n";
}

/// Walks up from the working directory looking for the sample's content, so the
/// binary works both from build/<cfg>/bin and from the source tree.
std::filesystem::path find_content_dir() {
    namespace fs = std::filesystem;
    if (auto configured = std::string_view(NF_MEDIEVAL_CONTENT_DIR); !configured.empty()) {
        std::error_code ec;
        if (fs::exists(configured, ec) && !ec) {
            return fs::path(configured);
        }
    }
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    if (ec) {
        return {};
    }
    for (int i = 0; i < 6; ++i) {
        for (const char* rel : {"Samples/MedievalVillage/Content/Medieval",
                                "Content/Medieval"}) {
            const fs::path cand = dir / rel;
            if (fs::exists(cand / "MedievalKit.manifest", ec) && !ec) {
                return cand;
            }
        }
        if (!dir.has_parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }
    return {};
}

std::filesystem::path resolve_dir(const std::string& explicit_path, const char* configured,
                                  std::initializer_list<const char*> fallbacks,
                                  const char* probe_file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!explicit_path.empty()) {
        if (fs::exists(fs::path(explicit_path) / probe_file, ec) && !ec) {
            return explicit_path;
        }
    }
    if (const std::string_view cfg(configured); !cfg.empty()) {
        if (fs::exists(fs::path(cfg) / probe_file, ec) && !ec) {
            return fs::path(cfg);
        }
    }
    fs::path dir = fs::current_path(ec);
    for (int i = 0; i < 6 && !ec; ++i) {
        for (const char* rel : fallbacks) {
            if (fs::exists(dir / rel / probe_file, ec) && !ec) {
                return dir / rel;
            }
        }
        if (!dir.has_parent_path()) {
            break;
        }
        dir = dir.parent_path();
    }
    return {};
}

// --- gameplay components ----------------------------------------------------

/// Everything the game needs to know about an entity that is not already a
/// rendering or scene component.
struct GameTag {
    PlacementTag kind = PlacementTag::Structure;
    physics::BodyHandle body{};
    // Crates
    Vec3 home{};
    bool carried = false;
    // Doors: 0 = closed, 1 = fully open. The visual yaw is interpolated from
    // this, and the collider is dropped while the door is moving so a swinging
    // plank cannot shove the player.
    float door_t = 0.0f;
    float door_target = 0.0f;
    float door_closed_yaw = 0.0f;
    bool door_blocking = true;
};

// --- third-person camera ----------------------------------------------------

struct CameraRig {
    float yaw = 0.0f;       // degrees; 0 looks down -Z
    float pitch = 22.0f;    // degrees above the horizon
    float distance = 7.5f;
};

rendering::Camera make_camera(const CameraRig& rig, const Vec3& target, float aspect) {
    const float yaw = rig.yaw * kDegToRad;
    const float pitch = rig.pitch * kDegToRad;
    const Vec3 dir{std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                   std::cos(yaw) * std::cos(pitch)};
    rendering::Camera cam{};
    cam.position = target + dir * rig.distance;
    cam.target = target;
    cam.aspect = aspect;
    cam.fov_y_rad = 58.0f * kDegToRad;
    cam.near_plane = 0.1f;
    cam.far_plane = 400.0f;
    rendering::update_camera(cam);
    return cam;
}

// --- render extraction ------------------------------------------------------

/// ECS -> RenderWorld with full TRS.
///
/// The engine ships runtime::extract_render_objects, but that one writes
/// `Mat4::translate(world_x, world_y, world_z)` — translation only. A village
/// built from modular pieces is nothing BUT rotation (every wall on three of the
/// four sides of a house is turned a quarter turn), so this sample composes the
/// matrix with scene::compose_trs_mat4, which is the same function the engine's
/// own Runtime::build_render_world uses.
///
/// Culling note: the renderer tests the bounding SPHERE first and keeps anything
/// that passes, so the sphere is the only bound that has to be right. A sphere
/// is rotation-invariant; scaling is handled explicitly below.
void extract_trs_render_world(const ecs::World& world, const rendering::MeshLibrary& meshes,
                              rendering::RenderWorld& out) {
    out.clear();
    for (ecs::Entity e : world.query<scene::Transform, rendering::MeshComponent>()) {
        const auto* tr = world.get<scene::Transform>(e);
        const auto* mc = world.get<rendering::MeshComponent>(e);
        if (tr == nullptr || mc == nullptr || !mc->visible || !mc->mesh.valid()) {
            continue;
        }
        const rendering::StaticMesh* mesh = meshes.get(mc->mesh);
        if (mesh == nullptr) {
            continue;
        }
        rendering::RenderObject ro;
        ro.id = e.id;
        ro.visible = true;
        ro.transform.x = tr->world_x;
        ro.transform.y = tr->world_y;
        ro.transform.z = tr->world_z;
        ro.world = scene::compose_trs_mat4(tr->world_x, tr->world_y, tr->world_z, tr->rot_x,
                                           tr->rot_y, tr->rot_z, tr->scale_x, tr->scale_y,
                                           tr->scale_z);
        ro.mesh_handle = mc->mesh;
        ro.material_handle = mc->material;
        ro.lod = mc->lod;
        ro.bounds = rendering::transform_aabb(mesh->bounds(), tr->world_x, tr->world_y, tr->world_z);
        ro.sphere =
            rendering::transform_sphere(mesh->bounding_sphere(), tr->world_x, tr->world_y, tr->world_z);
        const float smax = std::max({std::abs(tr->scale_x), std::abs(tr->scale_y),
                                     std::abs(tr->scale_z)});
        if (smax != 1.0f) {
            // Conservative on both: a bound that is too large only costs a
            // draw, a bound that is too small culls something visible.
            ro.sphere.radius *= smax;
            ro.bounds.min_x -= mesh->bounds().extent_x() * (smax - 1.0f);
            ro.bounds.max_x += mesh->bounds().extent_x() * (smax - 1.0f);
            ro.bounds.min_y -= mesh->bounds().extent_y() * (smax - 1.0f);
            ro.bounds.max_y += mesh->bounds().extent_y() * (smax - 1.0f);
            ro.bounds.min_z -= mesh->bounds().extent_z() * (smax - 1.0f);
            ro.bounds.max_z += mesh->bounds().extent_z() * (smax - 1.0f);
        }
        out.objects.push_back(std::move(ro));
    }
}

// --- world ------------------------------------------------------------------

struct World {
    ecs::World ecs;
    physics::PhysicsWorld physics;
    std::vector<ecs::Entity> crates;
    std::vector<ecs::Entity> doors;
    std::vector<ecs::Entity> carried_visuals; // crate meshes riding on the player
    ecs::Entity wagon = ecs::kInvalidEntity;
    ecs::Entity player_visual = ecs::kInvalidEntity;
    // World AABBs of the pieces that BLOCK movement (structures, fences, the
    // wagon) — the autoplay whisker steerer reads these. Crates and doors are
    // deliberately excluded: they are things the bot walks TO, not around, and
    // a whisker check against its own target would make it flinch forever.
    std::vector<WorldAabb> nav_boxes;
};

/// Adds a static box body matching a placed piece's world AABB.
physics::BodyHandle add_static_box(physics::PhysicsWorld& world, const WorldAabb& box) {
    const Vec3 size = box.size();
    if (size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f) {
        return {};
    }
    physics::BodyDesc desc;
    desc.type = physics::BodyType::Static;
    // Half extents with a hair of shrink: two pieces that share a face (a wall
    // and the corner column capping it) would otherwise start interpenetrating,
    // and the solver's first job on frame 1 would be to push the world apart.
    desc.shape = physics::Shape::make_box(size * 0.5f);
    desc.position = box.centre();
    desc.friction = 0.8f;
    return world.add_body(desc);
}

// --- autoplay whisker steering ----------------------------------------------
// The bot has no pathfinder; instead of leaning on the collision response
// (which produces the zigzag slide), it probes a point ahead of each candidate
// direction against the blocking pieces' world AABBs and picks the clearest
// direction closest to the one it wanted. Rotation uses the SAME convention as
// scene::compose_trs_mat4 / Village's rotate_y, so steering agrees with how
// the renderer places rotated pieces.

Vec3 rotate_y_dir(const Vec3& v, float deg) {
    const float r = deg * kDegToRad;
    const float c = std::cos(r);
    const float s = std::sin(r);
    return Vec3{v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

/// True when the XZ point `p` sits inside any nav box inflated by `margin`,
/// counting only boxes that overlap the player's height band (roofs and
/// chimneys are Structure too, but 3 m above the ground they must not steer).
bool point_blocked(const std::vector<WorldAabb>& boxes, const Vec3& p, float margin) {
    for (const WorldAabb& b : boxes) {
        if (b.max.y < -margin || b.min.y > 2.0f + margin) {
            continue; // entirely below the ground or above head height
        }
        if (p.x >= b.min.x - margin && p.x <= b.max.x + margin &&
            p.z >= b.min.z - margin && p.z <= b.max.z + margin) {
            return true;
        }
    }
    return false;
}

/// Returns a walk direction: `desired` if the whisker ahead of it is clear,
/// otherwise the closest clear direction sweeping sideways (always preferring
/// the SAME side, so a house wall is rounded instead of zigzagged against).
/// Falls back to `desired` when every probe is blocked — the character
/// controller's own collision response is then the last resort.
Vec3 steer_around(const std::vector<WorldAabb>& boxes, const Vec3& pos, const Vec3& desired) {
    constexpr float kMargin = 0.45f; // a bit over the controller's radius
    static constexpr float kAngles[] = {0.0f,  25.0f,  -25.0f,  50.0f,  -50.0f,
                                        80.0f, -80.0f, 115.0f, -115.0f, 150.0f,
                                        -150.0f, 180.0f};
    if (std::abs(desired.x) < 1e-6f && std::abs(desired.z) < 1e-6f) {
        return desired;
    }
    const Vec3 want{desired.x, 0.0f, desired.z};
    // Two whiskers: the short one catches walls the player is already
    // touching, the long one commits only to directions with real clearance.
    if (!point_blocked(boxes, pos + want * 0.7f, kMargin) &&
        !point_blocked(boxes, pos + want * 1.5f, kMargin)) {
        return want;
    }
    for (const float a : kAngles) {
        if (a == 0.0f) {
            continue;
        }
        const Vec3 dir = rotate_y_dir(want, a);
        if (!point_blocked(boxes, pos + dir * 0.7f, kMargin) &&
            !point_blocked(boxes, pos + dir * 1.5f, kMargin)) {
            return dir;
        }
    }
    return want;
}

} // namespace

// ---------------------------------------------------------------------------

int run_game(const Options& opt) {
    const std::filesystem::path content = opt.content_dir.empty()
                                              ? find_content_dir()
                                              : std::filesystem::path(opt.content_dir);
    if (content.empty()) {
        NF_LOG_FATAL(LogCategory::Core,
                     "MedievalVillage: kit content not found. Pass --content <dir> or run "
                     "Samples/MedievalVillage/Tools/import_kit.sh first.");
        return 2;
    }
    const std::filesystem::path shaders =
        resolve_dir(opt.shader_dir, NF_BASIC3D_SHADER_DIR,
                    {"build/DebugNinja/Shaders/Basic3D", "build/debug/Shaders/Basic3D",
                     "Samples/Basic3D/shaders"},
                    "gbuffer_frag.spv");
    if (shaders.empty()) {
        NF_LOG_FATAL(LogCategory::Core, "MedievalVillage: Basic3D shaders not found");
        return 2;
    }
    const std::filesystem::path imgui_shaders =
        resolve_dir(opt.imgui_shader_dir, NF_EDITOR_IMGUI_SHADER_DIR,
                    {"build/DebugNinja/Shaders/EditorImGui", "build/debug/Shaders/EditorImGui",
                     "Editor/shaders"},
                    "imgui_vert.spv");
    if (imgui_shaders.empty()) {
        NF_LOG_FATAL(LogCategory::Core, "MedievalVillage: ImGui shaders not found");
        return 2;
    }

    // The engine's UI spine owns every screen; the language decides the labels.
    ui::set_language(opt.english ? ui::Language::English : ui::Language::Arabic);

    platform_init();
    InputSystem::instance().init();

    Window window;
    {
        WindowDesc wdesc{};
        wdesc.width = opt.width;
        wdesc.height = opt.height;
        wdesc.title = opt.english ? "NOVAForge - Medieval Village: Harvest Run"
                                  : "NOVAForge - قرية العصور الوسطى: سباق الحصاد";
        wdesc.vsync = true;
        if (!window.create(wdesc)) {
            NF_LOG_FATAL(LogCategory::Platform, "MedievalVillage: window creation failed");
            InputSystem::instance().shutdown();
            platform_shutdown();
            return 1;
        }
    }

    auto device = rhi::create_device();
    if (!device) {
        NF_LOG_FATAL(LogCategory::RHI, "MedievalVillage: no graphics device");
        window.destroy();
        InputSystem::instance().shutdown();
        platform_shutdown();
        return 1;
    }
    {
        rhi::DeviceDesc ddesc{};
        ddesc.window_handle = window.native_handle();
        ddesc.enable_validation = opt.validation;
        if (!device->init(ddesc)) {
            NF_LOG_FATAL(LogCategory::RHI, "MedievalVillage: device init failed");
            window.destroy();
            InputSystem::instance().shutdown();
            platform_shutdown();
            return 1;
        }
    }

    auto swapchain = device->create_swapchain(rhi::SwapchainDesc{
        .width = window.width(),
        .height = window.height(),
        .format = rhi::Format::B8G8R8A8_UNorm,
        .present = rhi::PresentMode::FIFO,
        .image_count = 2,
    });
    if (!swapchain) {
        NF_LOG_FATAL(LogCategory::RHI, "MedievalVillage: swapchain creation failed");
        device->shutdown();
        window.destroy();
        InputSystem::instance().shutdown();
        platform_shutdown();
        return 1;
    }

    rendering::Renderer3D renderer;
    if (!renderer.init(*device, shaders, window.width(), window.height())) {
        NF_LOG_FATAL(LogCategory::RHI, "MedievalVillage: Renderer3D init failed");
        device->shutdown();
        window.destroy();
        InputSystem::instance().shutdown();
        platform_shutdown();
        return 1;
    }

    rendering::MeshLibrary meshes;
    renderer.set_mesh_library(&meshes);

    // --- kit ------------------------------------------------------------------
    Kit kit;
    {
        std::string err;
        if (!kit.load(content, *device, renderer, meshes, err)) {
            NF_LOG_FATAL(LogCategory::Core, "MedievalVillage: kit load failed: {}", err);
            renderer.shutdown();
            device->shutdown();
            window.destroy();
            InputSystem::instance().shutdown();
            platform_shutdown();
            return 1;
        }
    }

    // --- UI -------------------------------------------------------------------
    ui::GameFlow flow;
    UiOverlay overlay;
    if (!overlay.init(*device, window.native_handle(), imgui_shaders)) {
        NF_LOG_FATAL(LogCategory::Core, "MedievalVillage: UI overlay init failed");
        renderer.shutdown();
        device->shutdown();
        window.destroy();
        InputSystem::instance().shutdown();
        platform_shutdown();
        return 1;
    }
    window.set_message_hook(&UiOverlay::handle_win32_message);
    if (!overlay.attach_swapchain(*swapchain)) {
        NF_LOG_FATAL(LogCategory::RHI, "MedievalVillage: UI overlay pass setup failed");
        overlay.shutdown();
        renderer.shutdown();
        device->shutdown();
        window.destroy();
        InputSystem::instance().shutdown();
        platform_shutdown();
        return 1;
    }

    // --- village --------------------------------------------------------------
    World w;
    const Village village = build_village(kit);

    // Ground: a plane collider plus one tiled grid mesh (see Village.cpp).
    {
        physics::BodyDesc ground;
        ground.type = physics::BodyType::Static;
        ground.shape = physics::Shape::make_plane(Vec3{0.0f, 1.0f, 0.0f});
        w.physics.add_body(ground);

        auto grid = make_ground_grid(village.ground_half_extent, 2.0f);
        const rendering::StaticMeshHandle grid_handle = meshes.add(std::move(grid));
        rendering::PBRMaterialParams gp{};
        gp.base_color[0] = 1.0f;
        gp.base_color[1] = 1.0f;
        gp.base_color[2] = 1.0f;
        gp.base_color[3] = 1.0f;
        gp.roughness = 0.9f;
        gp.use_base_color_texture = 1.0f;
        const rendering::MaterialHandle ground_mat =
            renderer.materials().create_instance(*renderer.gbuffer_material(), gp, "ground");
        // Reuse the kit's own floor map so the ground is village stone, not a
        // flat colour the kit never had.
        if (const KitPiece* stone = kit.piece("Floor_UnevenBrick"); stone != nullptr) {
            if (const rendering::MaterialEntry* entry = renderer.materials().get(stone->material)) {
                if (entry->albedo_view != nullptr && entry->sampler != nullptr) {
                    renderer.materials().set_albedo_texture(ground_mat, *entry->albedo_view,
                                                            *entry->sampler);
                }
            }
        }
        ecs::Entity e = w.ecs.create_entity();
        w.ecs.add<scene::Transform>(e, scene::Transform{});
        w.ecs.add<rendering::MeshComponent>(
            e, rendering::MeshComponent{grid_handle, ground_mat, true, 0});
    }

    // Placed pieces -> entities + static colliders.
    int colliders = 0;
    for (const PlacedPiece& placed : village.pieces) {
        const KitPiece* piece = kit.piece(placed.piece);
        if (piece == nullptr) {
            continue;
        }
        ecs::Entity e = w.ecs.create_entity();
        scene::Transform tr;
        tr.local_x = placed.position.x;
        tr.local_y = placed.position.y;
        tr.local_z = placed.position.z;
        tr.rot_y = placed.rot_y;
        tr.scale_x = placed.scale.x;
        tr.scale_y = placed.scale.y;
        tr.scale_z = placed.scale.z;
        w.ecs.add<scene::Transform>(e, tr);
        w.ecs.add<rendering::MeshComponent>(
            e, rendering::MeshComponent{piece->mesh, piece->material, true, 0});

        GameTag tag;
        tag.kind = placed.tag;
        if (placed.tag == PlacementTag::Door) {
            tag.door_closed_yaw = placed.rot_y;
            tag.door_blocking = true;
            w.doors.push_back(e);
        }
        if (placed.tag == PlacementTag::Crate) {
            tag.home = placed.position;
            w.crates.push_back(e);
        }
        if (placed.tag == PlacementTag::Wagon) {
            w.wagon = e;
        }
        const bool wants_collider = placed.tag == PlacementTag::Structure ||
                                    placed.tag == PlacementTag::Fence ||
                                    placed.tag == PlacementTag::Wagon ||
                                    placed.tag == PlacementTag::Crate ||
                                    placed.tag == PlacementTag::Door;
        if (wants_collider) {
            const WorldAabb box = placed_world_aabb(kit, placed);
            tag.body = add_static_box(w.physics, box);
            if (tag.body.valid()) {
                ++colliders;
            }
            if (placed.tag == PlacementTag::Structure ||
                placed.tag == PlacementTag::Fence ||
                placed.tag == PlacementTag::Wagon) {
                w.nav_boxes.push_back(box);
            }
        }
        w.ecs.add<GameTag>(e, tag);
    }
    scene::propagate_transforms(w.ecs);

    // --- player ---------------------------------------------------------------
    // The kit is an environment kit: it ships no character mesh, so the player is
    // a sphere of exactly the controller's radius — the thing you see is the
    // thing that collides. Carried crates use the kit's own crate mesh.
    physics::CharacterConfig char_cfg;
    char_cfg.radius = 0.45f;
    char_cfg.max_speed = kWalkSpeed;
    char_cfg.jump_speed = 6.2f;
    physics::CharacterController player(w.physics, char_cfg, Vec3{village.spawn.x, 1.2f, village.spawn.z});

    rendering::PBRMaterialParams pp{};
    pp.base_color[0] = 0.16f;
    pp.base_color[1] = 0.62f;
    pp.base_color[2] = 0.72f;
    pp.base_color[3] = 1.0f;
    pp.roughness = 0.35f;
    const rendering::MaterialHandle player_mat =
        renderer.materials().create_instance(*renderer.gbuffer_material(), pp, "player");
    {
        auto sphere = rendering::StaticMesh::create_sphere(char_cfg.radius, 20);
        const rendering::StaticMeshHandle handle = meshes.add(std::move(sphere));
        w.player_visual = w.ecs.create_entity();
        w.ecs.add<scene::Transform>(w.player_visual, scene::Transform{});
        w.ecs.add<rendering::MeshComponent>(
            w.player_visual, rendering::MeshComponent{handle, player_mat, true, 0});
    }
    if (!meshes.upload_all(*device)) {
        NF_LOG_WARN(LogCategory::Core, "MedievalVillage: a runtime-generated mesh failed to upload");
    }

    // --- gameplay state -------------------------------------------------------
    gameplay::QuestLog quests;
    gameplay::Inventory inventory(kCarryCapacity * 2);
    {
        gameplay::Quest harvest;
        harvest.id = "harvest";
        harvest.title = opt.english ? "The Harvest Festival" : "مهرجان الحصاد";
        harvest.state = gameplay::QuestState::Active;
        gameplay::QuestStage gather;
        gather.id = "gather";
        gather.label = opt.english ? "Bring the supplies to the wagon" : "أوصل المؤن إلى العربة";
        gather.objectives.push_back(gameplay::QuestObjective{
            "crates", opt.english ? "Crates delivered" : "صناديق مُسلَّمة", kCrateTotal, 0});
        harvest.stages.push_back(std::move(gather));
        quests.define(std::move(harvest));
        quests.start("harvest");
    }

    int delivered = 0;
    float stamina = 100.0f;
    float time_left = kRoundSeconds;
    bool round_over = false;
    bool victory = false;
    float elapsed = 0.0f;

    // --- HUD ------------------------------------------------------------------
    // The engine's Hud gives the health bar, the ammo readout, the reticle and
    // the message line. The objective, the delivery progress and the timer are
    // game-specific, so they are extra widgets on the same canvas — the HUD is
    // one canvas, and the snapshot the overlay draws is the whole of it.
    ui::Canvas& hud = flow.hud().canvas();
    const bool ar = !opt.english;
    hud.add_label(ui::Label{kObjectiveId, ui::UiRect{0.5f, 0.015f, 0.9f, 0.04f},
                            ar ? "أوصل ٦ صناديق مؤن إلى العربة قبل الغروب"
                               : "Deliver 6 supply crates to the wagon before dusk",
                            22.0f, ui::UiColor{1.0f, 0.93f, 0.72f, 1.0f}, ui::TextAlign::Center});
    {
        ui::ProgressBar delivered_bar;
        delivered_bar.id = kDeliveredBarId;
        delivered_bar.rect = ui::UiRect{0.03f, 0.105f, 0.22f, 0.028f};
        delivered_bar.fill = ui::UiColor{0.95f, 0.72f, 0.18f, 1.0f};
        delivered_bar.label_size = 14.0f;
        hud.add_bar(delivered_bar);

        ui::ProgressBar clock;
        clock.id = kClockBarId;
        clock.rect = ui::UiRect{0.76f, 0.105f, 0.21f, 0.028f};
        clock.fill = ui::UiColor{0.55f, 0.78f, 1.0f, 1.0f};
        clock.label_size = 14.0f;
        hud.add_bar(clock);
    }
    hud.add_label(ui::Label{kCarryId, ui::UiRect{0.03f, 0.14f, 0.4f, 0.03f}, "", 18.0f,
                            ui::UiColor{0.85f, 0.9f, 1.0f, 1.0f}, ui::TextAlign::Left});
    hud.add_label(ui::Label{kPromptId, ui::UiRect{0.5f, 0.845f, 0.9f, 0.035f}, "", 22.0f,
                            ui::UiColor{1.0f, 1.0f, 1.0f, 1.0f}, ui::TextAlign::Center});
    hud.add_label(ui::Label{kHelpId, ui::UiRect{0.5f, 0.965f, 0.9f, 0.03f},
                            ar ? "WASD حركة · Shift عدو · Space قفز · E تفاعل · زر الفأرة الأيمن دوران · Esc قائمة"
                               : "WASD move · Shift sprint · Space jump · E interact · RMB look · Esc menu",
                            15.0f, ui::UiColor{0.72f, 0.76f, 0.82f, 1.0f}, ui::TextAlign::Center});
    // The reticle is a shooter's sight; this game has no aim, so it stays off.
    if (ui::Reticle* reticle = hud.find_reticle("hud_reticle")) {
        reticle->visible = false;
    }

    // Menus: the game names its own screens through the flow's callbacks.
    bool want_quit = false;
    flow.on_start_game = [&]() {
        flow.hud().show_message(ar ? "اذهب! اجمع الصناديق" : "Go! Gather the crates", 3.0f);
    };
    flow.on_resume = [&]() { flow.hud().show_message("", 0.0f); };
    flow.on_quit = [&]() { want_quit = true; };

    auto restart_round = [&]() {
        delivered = 0;
        stamina = 100.0f;
        time_left = kRoundSeconds;
        round_over = false;
        victory = false;
        for (ecs::Entity e : w.crates) {
            GameTag* tag = w.ecs.get<GameTag>(e);
            if (tag == nullptr) {
                continue;
            }
            if (tag->carried) {
                tag->carried = false;
                if (!tag->body.valid()) {
                    physics::BodyDesc desc;
                    desc.type = physics::BodyType::Static;
                    desc.shape = physics::Shape::make_box(Vec3{0.54f, 0.54f, 0.54f});
                    desc.position = tag->home + Vec3{0.0f, 0.53f, 0.0f};
                    tag->body = w.physics.add_body(desc);
                }
            }
            scene::Transform* tr = w.ecs.get<scene::Transform>(e);
            if (tr != nullptr) {
                tr->local_x = tag->home.x;
                tr->local_y = tag->home.y;
                tr->local_z = tag->home.z;
                tr->rot_y = 0.0f;
            }
        }
        // Teleport through the physics world rather than by writing the
        // controller: the body is the authority on where the player is, and a
        // transform written behind its back is undone on the next step.
        physics::BodyState state;
        state.position = Vec3{village.spawn.x, 1.2f, village.spawn.z};
        w.physics.set_state(player.body(), state);

        quests = gameplay::QuestLog{};
        gameplay::Quest harvest;
        harvest.id = "harvest";
        harvest.title = ar ? "مهرجان الحصاد" : "The Harvest Festival";
        harvest.state = gameplay::QuestState::Active;
        gameplay::QuestStage gather;
        gather.id = "gather";
        gather.label = ar ? "أوصل المؤن إلى العربة" : "Bring the supplies to the wagon";
        gather.objectives.push_back(gameplay::QuestObjective{"crates", "", kCrateTotal, 0});
        harvest.stages.push_back(std::move(gather));
        quests.define(std::move(harvest));
        quests.start("harvest");
        flow.hud().show_message(ar ? "محاولة جديدة" : "New run", 2.5f);
    };
    flow.on_restart = restart_round;

    // --- frame resources ------------------------------------------------------
    auto image_available = device->create_semaphore();
    auto frame_fence = device->create_fence(true);
    auto cmd = device->create_command_buffer();
    std::vector<std::unique_ptr<rhi::Semaphore>> render_finished;
    for (u32 i = 0; i < swapchain->image_count(); ++i) {
        render_finished.push_back(device->create_semaphore());
    }

    renderer.set_directional_light(rendering::DirectionalLight{
        rendering::Vec3{-0.45f, -0.82f, -0.35f}, rendering::Vec3{1.0f, 0.96f, 0.88f}, 1.15f, true});
    renderer.set_ambient(0.28f);
    renderer.set_exposure(1.05f);

    // --- loop -----------------------------------------------------------------
    Timer timer;
    timer.start();
    rendering::RenderWorld render_world;
    CameraRig rig; // persists across frames; see the camera block below

    u32 frames = 0;
    u32 total_objects = 0;
    u32 total_draws = 0;
    u32 frames_with_geometry = 0;
    int picked_up = 0;
    int interactions = 0;
    int rounds_won = 0; // autoplay restarts rounds; the summary reports the total
    bool reached_goal = false;
    int ui_frames = 0;

    // Autoplay navigation state. The whisker steerer handles walls; what is
    // left here is giving up on a crate the bot cannot reach so it tries
    // another instead of leaning on the same wall forever.
    std::vector<Vec3> nav_skipped;           // crate homes given up on this run
    Vec3 nav_crate_home{0.0f, 0.0f, 0.0f};   // home of the crate being chased
    float nav_crate_timer = 0.0f;            // seconds spent chasing it
    const auto dist2 = [](const Vec3& a, const Vec3& b) {
        const float dx = a.x - b.x;
        const float dz = a.z - b.z;
        return dx * dx + dz * dz;
    };

    NF_LOG_INFO(LogCategory::Core,
                "MedievalVillage: entering loop ({} pieces, {} colliders, autoplay={})",
                village.pieces.size(), colliders, opt.autoplay);

    while (!window.should_close() && !want_quit) {
        float dt = static_cast<float>(timer.tick());
        if (dt > 0.05f) {
            dt = 0.05f;
        }
        elapsed += dt;

        InputSystem::instance().begin_frame();
        window.poll_events();
        if (window.should_close()) {
            break;
        }
        auto& input = InputSystem::instance();

        // --- input -> ui::Action (menus are the engine's state machine) -------
        if (input.is_key_pressed(KeyCode::Up) || input.is_key_pressed(KeyCode::W)) {
            flow.handle(ui::Action::Up);
        }
        if (input.is_key_pressed(KeyCode::Down) || input.is_key_pressed(KeyCode::S)) {
            flow.handle(ui::Action::Down);
        }
        if (input.is_key_pressed(KeyCode::Left)) {
            flow.handle(ui::Action::Left);
        }
        if (input.is_key_pressed(KeyCode::Right)) {
            flow.handle(ui::Action::Right);
        }
        if (input.is_key_pressed(KeyCode::Enter) || input.is_key_pressed(KeyCode::Space)) {
            flow.handle(ui::Action::Confirm);
        }
        if (input.is_key_pressed(KeyCode::Escape)) {
            flow.handle(ui::Action::Back);
        }
        // Autoplay drives the menus too: a run with no keyboard must still get
        // from the main menu into the world (and into a fresh round after a
        // game over), so Confirm is sent exactly the way a key press would be.
        // Without this an automated run sits on the main menu forever.
        if (opt.autoplay && (flow.screen() == ui::Screen::MainMenu ||
                             flow.screen() == ui::Screen::GameOver)) {
            flow.handle(ui::Action::Confirm);
        }

        const bool playing = flow.screen() == ui::Screen::Playing;

        // --- camera ----------------------------------------------------------
        // The rig persists across frames (a camera that snapped back to its
        // default every frame would be unusable), so it lives outside the loop.
        if (input.is_mouse_down(MouseButton::Right)) {
            rig.yaw -= input.mouse_delta_x() * 0.22f;
            rig.pitch += input.mouse_delta_y() * 0.18f;
        }
        if (input.is_key_down(KeyCode::Q)) {
            rig.yaw += 90.0f * dt;
        }
        if (input.is_key_down(KeyCode::Z)) {
            rig.yaw -= 90.0f * dt;
        }
        rig.pitch = std::clamp(rig.pitch, -8.0f, 62.0f);
        if (const float scroll = input.scroll_delta(); scroll != 0.0f) {
            rig.distance = std::clamp(rig.distance - scroll * 0.8f, 3.5f, 16.0f);
        }

        // --- movement ---------------------------------------------------------
        const Vec3 player_pos = player.position();
        Vec3 wish{0.0f, 0.0f, 0.0f};
        bool jump = false;
        bool interact = false;
        if (playing && !round_over) {
            const float yaw_rad = rig.yaw * kDegToRad;
            const Vec3 forward{-std::sin(yaw_rad), 0.0f, -std::cos(yaw_rad)};
            const Vec3 right{std::cos(yaw_rad), 0.0f, -std::sin(yaw_rad)};

            float mx = 0.0f, mz = 0.0f;
            if (input.is_key_down(KeyCode::W)) {
                mz += 1.0f;
            }
            if (input.is_key_down(KeyCode::S)) {
                mz -= 1.0f;
            }
            if (input.is_key_down(KeyCode::D)) {
                mx += 1.0f;
            }
            if (input.is_key_down(KeyCode::A)) {
                mx -= 1.0f;
            }

            // Autoplay drives the same wish vector the keys would, so the
            // automated run exercises the real controller rather than a
            // teleport.
            if (opt.autoplay) {
                Vec3 target = Vec3{village.wagon.x, 0.0f, village.wagon.z};
                int carried_now = 0;
                for (ecs::Entity e : w.crates) {
                    const GameTag* t = w.ecs.get<GameTag>(e);
                    if (t != nullptr && t->carried) {
                        ++carried_now;
                    }
                }
                const bool need_crates =
                    carried_now < kCarryCapacity && delivered + carried_now < kCrateTotal;
                bool chasing_crate = false;
                if (need_crates) {
                    float best = 1e30f;
                    for (ecs::Entity e : w.crates) {
                        const GameTag* t = w.ecs.get<GameTag>(e);
                        if (t == nullptr || t->carried) {
                            continue;
                        }
                        bool blacklisted = false;
                        for (const Vec3& s : nav_skipped) {
                            if (dist2(t->home, s) < 0.25f) {
                                blacklisted = true;
                                break;
                            }
                        }
                        if (blacklisted) {
                            continue;
                        }
                        const scene::Transform* tr = w.ecs.get<scene::Transform>(e);
                        if (tr == nullptr) {
                            continue;
                        }
                        const float d = (tr->world_x - player_pos.x) * (tr->world_x - player_pos.x) +
                                        (tr->world_z - player_pos.z) * (tr->world_z - player_pos.z);
                        if (d < best) {
                            best = d;
                            target = Vec3{tr->world_x, 0.0f, tr->world_z};
                            nav_crate_home = t->home;
                            chasing_crate = true;
                        }
                    }
                    if (best >= 1e29f && !nav_skipped.empty()) {
                        // Everything left is blacklisted: a corner case worth a
                        // second attempt rather than standing still forever.
                        nav_skipped.clear();
                        nav_crate_timer = 0.0f;
                    }
                }

                const Vec3 to{target.x - player_pos.x, 0.0f, target.z - player_pos.z};
                const float len = std::sqrt(to.x * to.x + to.z * to.z);
                // 2.0, not 1.1: a crate's own collider stops the player ~1.2 m
                // from its centre on a diagonal approach, so waiting to close
                // to 1.1 means leaning on the crate forever without ever
                // pressing E. 2.0 is comfortably inside kInteractRange (2.6).
                if (len > 2.0f) {
                    // Whisker steering: walk where the AABB probes say it is
                    // clear, sweeping sideways around houses and fences.
                    wish = steer_around(w.nav_boxes, player_pos,
                                        Vec3{to.x / len, 0.0f, to.z / len});
                    if (chasing_crate) {
                        nav_crate_timer += dt;
                        if (nav_crate_timer > 8.0f) {
                            nav_skipped.push_back(nav_crate_home);
                            nav_crate_timer = 0.0f;
                        }
                    }
                } else {
                    // Close enough: press E and keep a gentle push so the
                    // pickup radius (measured to the crate's surface) is
                    // entered rather than hovered at.
                    interact = true;
                    wish = Vec3{to.x / len, 0.0f, to.z / len} * 0.35f;
                }
                if (!chasing_crate) {
                    nav_crate_timer = 0.0f;
                }
            } else {
                wish = forward * mz + right * mx;
                const float len = std::sqrt(wish.x * wish.x + wish.z * wish.z);
                if (len > 0.001f) {
                    wish = Vec3{wish.x / len, 0.0f, wish.z / len};
                } else {
                    wish = Vec3{0.0f, 0.0f, 0.0f};
                }
                jump = input.is_key_pressed(KeyCode::Space);
                interact = input.is_key_pressed(KeyCode::E);
            }

            const bool want_sprint = !opt.autoplay &&
                                     (input.is_key_down(KeyCode::LeftShift) ||
                                      input.is_key_down(KeyCode::RightShift));
            const bool sprinting = want_sprint && stamina > 1.0f &&
                                   (wish.x != 0.0f || wish.z != 0.0f);
            if (sprinting) {
                stamina = std::max(0.0f, stamina - kSprintDrain * dt);
            } else {
                stamina = std::min(100.0f, stamina + kStaminaRegen * dt);
            }
            // CharacterController's cruise speed is fixed in its config, so
            // sprinting scales the wish vector instead: `move` targets
            // wish_dir * max_speed, and a wish of length > 1 asks for more than
            // cruise without touching the configuration the tests pin.
            const float speed_scale = sprinting ? (kSprintSpeed / kWalkSpeed) : 1.0f;
            player.move(wish * speed_scale, jump, dt);
        } else {
            player.move(Vec3{0.0f, 0.0f, 0.0f}, false, dt);
        }

        w.physics.step(dt);
        player.post_step();

        // --- doors ------------------------------------------------------------
        for (ecs::Entity e : w.doors) {
            GameTag* tag = w.ecs.get<GameTag>(e);
            scene::Transform* tr = w.ecs.get<scene::Transform>(e);
            if (tag == nullptr || tr == nullptr) {
                continue;
            }
            if (tag->door_t != tag->door_target) {
                const float step = dt * 1.8f;
                if (tag->door_t < tag->door_target) {
                    tag->door_t = std::min(tag->door_target, tag->door_t + step);
                } else {
                    tag->door_t = std::max(tag->door_target, tag->door_t - step);
                }
                // The collider follows the *closed* pose only: a door mid-swing
                // that still had a box would shove the player who opened it.
                const bool closed = tag->door_t < 0.15f;
                if (closed != tag->door_blocking) {
                    if (closed) {
                        if (!tag->body.valid()) {
                            physics::BodyDesc desc;
                            desc.type = physics::BodyType::Static;
                            desc.shape = physics::Shape::make_box(Vec3{0.6f, 1.1f, 0.1f});
                            desc.position = Vec3{tr->world_x, 1.1f, tr->world_z};
                            tag->body = w.physics.add_body(desc);
                        }
                    } else if (tag->body.valid()) {
                        w.physics.remove_body(tag->body);
                        tag->body = physics::BodyHandle{};
                    }
                    tag->door_blocking = closed;
                }
            }
            tr->rot_y = tag->door_closed_yaw - tag->door_t * 95.0f;
        }

        // --- interaction ------------------------------------------------------
        Vec3 prompt_anchor{0.0f, 0.0f, 0.0f};
        const char* prompt_text = "";
        if (playing && !round_over) {
            // Nearest interactable in range. Crates and doors first, because
            // the wagon sits in the middle of the square and would otherwise
            // swallow every crate the player walks past it with.
            float best = kInteractRange * kInteractRange;
            enum class Target { None, Crate, Door, Wagon } target = Target::None;
            ecs::Entity target_entity = ecs::kInvalidEntity;
            const auto consider = [&](ecs::Entity e, Target kind) {
                const scene::Transform* tr = w.ecs.get<scene::Transform>(e);
                if (tr == nullptr) {
                    return;
                }
                const float dx = tr->world_x - player_pos.x;
                const float dz = tr->world_z - player_pos.z;
                const float d = dx * dx + dz * dz;
                if (d < best) {
                    best = d;
                    target = kind;
                    target_entity = e;
                }
            };
            for (ecs::Entity e : w.crates) {
                const GameTag* t = w.ecs.get<GameTag>(e);
                if (t != nullptr && !t->carried) {
                    consider(e, Target::Crate);
                }
            }
            for (ecs::Entity e : w.doors) {
                consider(e, Target::Door);
            }
            if (w.wagon != ecs::kInvalidEntity) {
                const scene::Transform* tr = w.ecs.get<scene::Transform>(w.wagon);
                if (tr != nullptr) {
                    // The wagon is long, so its interaction radius is its own.
                    const float dx = tr->world_x - player_pos.x;
                    const float dz = tr->world_z - player_pos.z;
                    const float d = dx * dx + dz * dz;
                    const float wagon_range = 3.4f * 3.4f;
                    int carried_now = 0;
                    for (ecs::Entity e : w.crates) {
                        const GameTag* t = w.ecs.get<GameTag>(e);
                        if (t != nullptr && t->carried) {
                            ++carried_now;
                        }
                    }
                    if (carried_now > 0 && d < wagon_range) {
                        target = Target::Wagon;
                        target_entity = w.wagon;
                    }
                }
            }

            int carried_now = 0;
            for (ecs::Entity e : w.crates) {
                const GameTag* t = w.ecs.get<GameTag>(e);
                if (t != nullptr && t->carried) {
                    ++carried_now;
                }
            }

            switch (target) {
            case Target::Crate:
                prompt_text = ar ? "E — احمل الصندوق" : "E - pick up the crate";
                break;
            case Target::Door:
                prompt_text = ar ? "E — افتح/أغلق الباب" : "E - open / close the door";
                break;
            case Target::Wagon:
                prompt_text = ar ? "E — سلّم المؤن" : "E - deliver the supplies";
                break;
            case Target::None:
                prompt_text = (carried_now >= kCarryCapacity)
                                  ? (ar ? "يداك ممتلئتان — سلّم المؤن في العربة"
                                        : "Hands full - deliver to the wagon")
                                  : "";
                break;
            }
            if (target_entity != ecs::kInvalidEntity) {
                const scene::Transform* tr = w.ecs.get<scene::Transform>(target_entity);
                if (tr != nullptr) {
                    prompt_anchor = Vec3{tr->world_x, tr->world_y, tr->world_z};
                }
            }

            if (interact && target != Target::None) {
                ++interactions;
                switch (target) {
                case Target::Crate: {
                    GameTag* tag = w.ecs.get<GameTag>(target_entity);
                    if (tag != nullptr && !tag->carried) {
                        tag->carried = true;
                        ++picked_up;
                        inventory.add("crate", 1);
                        if (tag->body.valid()) {
                            w.physics.remove_body(tag->body);
                            tag->body = physics::BodyHandle{};
                        }
                        flow.hud().show_message(ar ? "حملت صندوقاً" : "Crate picked up", 1.4f);
                    }
                    break;
                }
                case Target::Door: {
                    GameTag* tag = w.ecs.get<GameTag>(target_entity);
                    if (tag != nullptr) {
                        tag->door_target = (tag->door_target > 0.5f) ? 0.0f : 1.0f;
                        flow.hud().show_message(
                            ar ? (tag->door_target > 0.5f ? "انفتح الباب" : "أُغلق الباب")
                               : (tag->door_target > 0.5f ? "Door opened" : "Door closed"),
                            1.2f);
                    }
                    break;
                }
                case Target::Wagon: {
                    int carried_now2 = 0;
                    for (ecs::Entity e : w.crates) {
                        const GameTag* t = w.ecs.get<GameTag>(e);
                        if (t != nullptr && t->carried) {
                            ++carried_now2;
                        }
                    }
                    if (carried_now2 > 0) {
                        for (ecs::Entity e : w.crates) {
                            GameTag* t = w.ecs.get<GameTag>(e);
                            if (t != nullptr && t->carried) {
                                t->carried = false;
                                // Delivered crates ride on the wagon deck.
                                scene::Transform* tr = w.ecs.get<scene::Transform>(e);
                                if (tr != nullptr) {
                                    const float slot = static_cast<float>(delivered) * 0.7f;
                                    tr->local_x = village.wagon.x - 0.6f + slot;
                                    tr->local_y = 1.35f;
                                    tr->local_z = village.wagon.z - 0.9f;
                                    tr->rot_y = 0.0f;
                                }
                            }
                        }
                        delivered += carried_now2;
                        inventory.remove("crate", static_cast<u32>(carried_now2));
                        quests.advance("harvest", "crates", static_cast<u32>(carried_now2));
                        char buf[96];
                        std::snprintf(buf, sizeof(buf),
                                      ar ? "سُلِّمت %d من %d" : "Delivered %d of %d", delivered,
                                      kCrateTotal);
                        flow.hud().show_message(buf, 2.0f);
                        if (delivered >= kCrateTotal) {
                            round_over = true;
                            victory = true;
                            reached_goal = true;
                            ++rounds_won;
                            flow.game_over_menu().set_title(
                                ar ? "اكتمل الحصاد — النصر!" : "Harvest complete - victory!");
                            flow.notify_game_over();
                            NF_LOG_INFO(LogCategory::Core,
                                        "MedievalVillage: VICTORY - all {} crates delivered in {:.1f}s",
                                        kCrateTotal, elapsed);
                        }
                    }
                    break;
                }
                case Target::None:
                    break;
                }
            }
        }

        // --- clock ------------------------------------------------------------
        if (playing && !round_over) {
            time_left -= dt;
            if (time_left <= 0.0f) {
                time_left = 0.0f;
                round_over = true;
                victory = false;
                flow.game_over_menu().set_title(ar ? "غروب — انتهى الوقت" : "Dusk - out of time");
                flow.notify_game_over();
                NF_LOG_INFO(LogCategory::Core,
                            "MedievalVillage: DEFEAT - dusk fell with {} of {} delivered", delivered,
                            kCrateTotal);
            }
        }

        // --- player visual ----------------------------------------------------
        {
            scene::Transform* tr = w.ecs.get<scene::Transform>(w.player_visual);
            if (tr != nullptr) {
                tr->local_x = player_pos.x;
                tr->local_y = player_pos.y;
                tr->local_z = player_pos.z;
                tr->rot_y = rig.yaw;
            }
            // Carried crates ride above the player's head, so "how much am I
            // carrying" is answered by the world and not only by a number.
            int carried_now = 0;
            for (ecs::Entity e : w.crates) {
                GameTag* tag = w.ecs.get<GameTag>(e);
                scene::Transform* ctr = w.ecs.get<scene::Transform>(e);
                if (tag == nullptr || ctr == nullptr) {
                    continue;
                }
                if (tag->carried) {
                    const float lift = char_cfg.radius * 2.0f + 0.55f +
                                       static_cast<float>(carried_now) * 1.08f;
                    ctr->local_x = player_pos.x;
                    ctr->local_y = player_pos.y + lift;
                    ctr->local_z = player_pos.z;
                    ctr->rot_y = rig.yaw;
                    ++carried_now;
                }
            }
        }

        scene::propagate_transforms(w.ecs);

        // --- HUD values -------------------------------------------------------
        {
            flow.hud().set_health(stamina, 100.0f);
            flow.hud().set_ammo(delivered, kCrateTotal);
            int carried_now = 0;
            for (ecs::Entity e : w.crates) {
                const GameTag* t = w.ecs.get<GameTag>(e);
                if (t != nullptr && t->carried) {
                    ++carried_now;
                }
            }
            if (ui::ProgressBar* bar = hud.find_bar(kDeliveredBarId)) {
                bar->fraction = static_cast<float>(delivered) / static_cast<float>(kCrateTotal);
                bar->label = ar ? "المؤن المُسلَّم" : "supplies delivered";
            }
            if (ui::ProgressBar* bar = hud.find_bar(kClockBarId)) {
                bar->fraction = std::clamp(time_left / kRoundSeconds, 0.0f, 1.0f);
                char buf[64];
                std::snprintf(buf, sizeof(buf), ar ? "الوقت %.0fث" : "time %.0fs", time_left);
                bar->label = buf;
            }
            if (ui::Label* label = hud.find_label(kCarryId)) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), ar ? "تحمل الآن: %d / %d" : "carrying: %d / %d",
                              carried_now, kCarryCapacity);
                label->text = buf;
            }
            if (ui::Label* label = hud.find_label(kPromptId)) {
                label->text = prompt_text;
            }
            flow.hud().update(dt);
        }

        // --- camera + render --------------------------------------------------
        const Vec3 look_target{player_pos.x, player_pos.y + 0.9f, player_pos.z};
        rendering::Camera cam =
            make_camera(rig, look_target, static_cast<float>(window.width()) /
                                             static_cast<float>(window.height()));
        extract_trs_render_world(w.ecs, meshes, render_world);

        frame_fence->wait();
        frame_fence->reset();
        const u32 image_index = swapchain->acquire_next_image(*image_available);
        if (image_index == u32_max) {
            break;
        }
        rhi::Texture* backbuffer = swapchain->get_texture(image_index);
        if (backbuffer == nullptr) {
            break;
        }

        // UI: the engine's flow decides the screen, the overlay draws it.
        overlay.begin_frame(dt);
        if (ui::Canvas* canvas = flow.active_canvas()) {
            overlay.draw_canvas(*canvas, static_cast<float>(window.width()),
                                static_cast<float>(window.height()));
            ++ui_frames;
        }
        overlay.end_frame();

        cmd->reset();
        cmd->begin();
        bool frame_ok = renderer.render(*cmd, render_world, cam, *backbuffer, true);
        if (frame_ok) {
            frame_ok = overlay.render(*cmd, *backbuffer, image_index, window.width(),
                                      window.height());
        }
        cmd->end();
        if (!frame_ok) {
            NF_LOG_ERROR(LogCategory::RHI, "MedievalVillage: frame {} render failed", frames);
            break;
        }

        const std::array<const rhi::Semaphore*, 1> wait_sems{image_available.get()};
        const std::array<rhi::PipelineStage, 1> wait_stages{rhi::PipelineStage::ColorAttachmentOutput};
        const std::array<const rhi::Semaphore*, 1> signal_sems{render_finished[image_index].get()};
        rhi::SubmitInfo submit{};
        submit.wait_semaphores = std::span<const rhi::Semaphore* const>(wait_sems);
        submit.wait_stages = std::span<const rhi::PipelineStage>(wait_stages);
        submit.signal_semaphores = std::span<const rhi::Semaphore* const>(signal_sems);
        submit.signal_fence = frame_fence.get();
        device->submit(*cmd, submit);
        swapchain->present(image_index, std::span<const rhi::Semaphore* const>(signal_sems));

        InputSystem::instance().end_frame();

        const rendering::Renderer3D::Stats& stats = renderer.last_stats();
        total_objects += static_cast<u32>(render_world.size());
        total_draws += stats.draw_calls;
        if (stats.draw_calls > 0) {
            ++frames_with_geometry;
        }

        ++frames;
        if (opt.max_frames != 0 && frames >= opt.max_frames) {
            break;
        }
    }

    device->wait_idle();

    const rendering::Renderer3D::Stats& stats = renderer.last_stats();
    const u32 rhi_errors = rhi::validation_error_count();
    NF_LOG_INFO(LogCategory::Core,
                "MedievalVillage: {} frames, {} kit pieces, {} colliders, {} draw calls last frame",
                frames, kit.report().pieces, colliders, stats.draw_calls);
    NF_LOG_INFO(LogCategory::Core,
                "MedievalVillage: gameplay - picked up {}, {} rounds won, last round delivered "
                "{} of {}, interactions {}",
                picked_up, rounds_won, delivered, kCrateTotal, interactions);
    NF_LOG_INFO(LogCategory::Core, "MedievalVillage: UI frames {}, font '{}'",
                ui_frames, overlay.font_description());

    const bool ok = frames > 0 && frames_with_geometry > 0 && rhi_errors == 0 &&
                    kit.report().failures.empty() && ui_frames > 0 &&
                    (!opt.autoplay || reached_goal);

    overlay.detach_swapchain();
    overlay.shutdown();
    renderer.shutdown();
    window.destroy();
    InputSystem::instance().shutdown();
    // NOTE: no explicit device->shutdown() here. The device outlives every GPU
    // resource in declaration order and is destroyed LAST at function exit, so
    // the kit's textures, the mesh library and the frame resources release
    // their Vulkan objects first. Calling shutdown() here, with them still
    // alive, is what produced the 345-object leak report.
    platform_shutdown();

    NF_LOG_INFO(LogCategory::Core, "MedievalVillage: {}", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}

} // namespace nf::sample::medieval

int main(int argc, char** argv) {
    using namespace nf;
    using namespace nf::sample::medieval;

    Logger::instance().add_sink(Logger::make_console_sink());
    Logger::instance().set_min_level(LogLevel::Info);

    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            opt.max_frames = static_cast<u32>(std::atoi(argv[++i]));
        } else if (arg.rfind("--frames=", 0) == 0) {
            opt.max_frames = static_cast<u32>(std::atoi(arg.substr(9).data()));
        } else if (arg == "--validation") {
            opt.validation = true;
        } else if (arg == "--autoplay") {
            opt.autoplay = true;
        } else if (arg == "--lang" && i + 1 < argc) {
            opt.english = std::string_view(argv[++i]) == "en";
        } else if (arg == "--content" && i + 1 < argc) {
            opt.content_dir = argv[++i];
        } else if (arg == "--shaders" && i + 1 < argc) {
            opt.shader_dir = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            print_help();
            return 2;
        }
    }
    return run_game(opt);
}
