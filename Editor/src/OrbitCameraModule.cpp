// NF/Editor/OrbitCameraModule.cpp — a worked example of the gameplay API (Phase 10)

#include <NF/Editor/OrbitCameraModule.hpp>

#include <NF/ECS/ECS.hpp>
#include <NF/Gameplay/GameplayModuleRegistry.hpp>
#include <NF/Runtime/RuntimeSceneTypes.hpp>
#include <NF/Scene/Transform.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace nf::editor {

namespace {

/// Just under 90 degrees: at exactly 90 the look direction is parallel to the
/// up axis and the aim becomes undefined, which shows up as a camera that flips.
constexpr f32 kMaxPitchDegrees = 89.0f;

constexpr f32 to_radians(f32 degrees) { return degrees * DEG_TO_RAD; }

/// First entity carrying a camera, or an invalid entity when the scene has none.
ecs::Entity find_camera(const ecs::World& world) {
    const std::vector<ecs::Entity> cameras = world.query<runtime::CameraComponent>();
    if (cameras.empty()) return ecs::Entity{};
    return cameras.front();
}

/// First entity that is not `exclude`. Used as the orbit pivot when the settings
/// do not name one, so the module does something visible in a scene that has not
/// been configured yet.
ecs::Entity first_other(const ecs::World& world, ecs::Entity exclude) {
    for (ecs::Entity e : world.all_entities()) {
        if (e != exclude) return e;
    }
    return ecs::Entity{};
}

Vec3 world_position_of(const ecs::World& world, ecs::Entity e) {
    if (const auto* t = world.get<scene::Transform>(e)) {
        return Vec3{t->world_x, t->world_y, t->world_z};
    }
    return Vec3{0.0f, 0.0f, 0.0f};
}

} // namespace

void OrbitCameraModule::on_update(gameplay::GameplayContext& ctx) {
    // Edit mode owns the camera: the editor's viewport navigation writes this
    // same Transform every frame, so placing here would snap each drag straight
    // back and the viewport would read as frozen. Gameplay drives only when
    // playing (Runtime mirrors the editor's play state into ctx.playing).
    if (!ctx.playing) return;
    if (ctx.world == nullptr) return;

    const ecs::Entity camera = find_camera(*ctx.world);
    if (!camera.valid()) return;  // nothing to place

    // The pivot is never the camera itself. Reading a position out of the same
    // transform this module writes would feed last frame's result back in as
    // this frame's input, and the camera would drift instead of orbiting.
    ecs::Entity target{};
    if (settings.target_entity != u32_max) {
        ecs::Entity candidate;
        candidate.id = settings.target_entity;
        // A generation-less reference cannot be verified, so liveness is the
        // only check available; a stale id simply falls through to the default.
        if (ctx.world->is_alive(candidate) && candidate != camera) {
            target = candidate;
        }
    }
    if (!target.valid()) {
        target = first_other(*ctx.world, camera);
    }

    const Vec3 pivot = target.valid() ? world_position_of(*ctx.world, target) : Vec3{0.0f, 0.0f, 0.0f};

    // Input, when there is a source. A headless run has none, and the module must
    // still place the camera from its settings rather than doing nothing.
    if (settings.use_input && ctx.input != nullptr && ctx.dt > 0.0f) {
        const f32 look = ctx.input->action_axis("look");
        const f32 zoom = ctx.input->action_axis("zoom");
        settings.yaw_degrees += look * settings.yaw_speed * ctx.dt;
        settings.radius -= zoom * settings.zoom_speed * ctx.dt;
    }

    settings.pitch_degrees = std::clamp(settings.pitch_degrees, -kMaxPitchDegrees, kMaxPitchDegrees);
    settings.radius = std::clamp(settings.radius, settings.min_radius, settings.max_radius);

    const f32 yaw = to_radians(settings.yaw_degrees);
    const f32 pitch = to_radians(settings.pitch_degrees);
    const f32 cos_pitch = std::cos(pitch);

    const Vec3 offset{
        settings.radius * cos_pitch * std::sin(yaw),
        settings.radius * std::sin(pitch),
        settings.radius * cos_pitch * std::cos(yaw),
    };
    const Vec3 position = pivot + offset;

    if (auto* t = ctx.world->get<scene::Transform>(camera)) {
        t->local_x = position.x;
        t->local_y = position.y;
        t->local_z = position.z;

        // Aim inward. With the engine's R = Ry * Rx * Rz and a camera that looks
        // along +Z, the direction to the pivot is (pitch, yaw + 180) — the
        // derivation is: Rx(pitch) * (0,0,1) = (0, -sin p, cos p), and yawing
        // that by 180 degrees flips the horizontal component onto the pivot.
        t->rot_x = settings.pitch_degrees;
        t->rot_y = settings.yaw_degrees + 180.0f;
        t->rot_z = 0.0f;

        ++m_placements;
        m_last_target = target.valid() ? target.id : u32_max;
        m_last_position = position;
    }
}

// Registered so `Runtime::init_gameplay` instantiates it and the inspector's
// dropdown offers it, with no engine code naming this type.
NF_GAMEPLAY_MODULE(OrbitCameraModule, "OrbitCamera")

} // namespace nf::editor
