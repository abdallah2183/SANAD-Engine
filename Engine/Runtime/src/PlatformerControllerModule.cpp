#include <NF/Runtime/PlatformerControllerModule.hpp>

#include <NF/ECS/ECS.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModuleRegistry.hpp>
#include <NF/Physics/Components.hpp>

#include <algorithm>
#include <cmath>

namespace nf::gameplay {

void PlatformerControllerModule::on_update(GameplayContext& ctx) {
    if (!ctx.playing || ctx.dt <= 0.0f || !ctx.world || !ctx.physics) return;
    ecs::Entity owner{};
    for (const ecs::Entity e : ctx.world->query<GameplayModuleComponent>()) {
        const auto* module = ctx.world->get<GameplayModuleComponent>(e);
        if (module && module->enabled && module->module_name == name()) { owner = e; break; }
    }
    if (!owner.valid()) return;
    auto* body = ctx.world->get<physics::RigidBodyComponent>(owner);
    if (!body || body->type != physics::BodyType::Dynamic || !ctx.physics->is_alive(body->body)) return;

    float axis = ctx.input ? ctx.input->action_axis("move_x") : 0.0f;
    if (!std::isfinite(axis)) axis = 0.0f;
    axis = std::clamp(axis, -1.0f, 1.0f);
    const bool jump = ctx.input && ctx.input->action_pressed("jump");
    bool grounded = false;
    for (const auto& contact : ctx.physics->last_manifolds()) {
        if (contact.point_count == 0) continue;
        const bool a = contact.body_a == body->body.index;
        const bool b = contact.body_b == body->body.index;
        if ((a && -contact.normal.y >= 0.65f) || (b && contact.normal.y >= 0.65f)) {
            grounded = true;
            break;
        }
    }
    auto current = ctx.physics->state(body->body);
    const float speed = std::isfinite(settings.speed) ? std::max(0.0f, settings.speed) : 0.0f;
    const float jump_velocity = std::isfinite(settings.jump_velocity) ? std::max(0.0f, settings.jump_velocity) : 0.0f;
    current.linear_velocity.x = axis * speed;
    current.linear_velocity.z = 0.0f;
    if (jump && !m_jump_held && grounded) current.linear_velocity.y = jump_velocity;
    m_jump_held = jump;
    // Prevent a side-view character from rolling away from the camera.
    current.angular_velocity = {};
    current.asleep = false;
    ctx.physics->set_state(body->body, current);
    body->linear_velocity = current.linear_velocity;
    body->angular_velocity = current.angular_velocity;
}

NF_GAMEPLAY_MODULE(PlatformerControllerModule, "PlatformerController")

} // namespace nf::gameplay
