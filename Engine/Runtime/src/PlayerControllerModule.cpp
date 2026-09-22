// NF/Runtime/PlayerControllerModule.cpp — arrow/WASD movement while playing.

#include <NF/Runtime/PlayerControllerModule.hpp>

#include <NF/ECS/ECS.hpp>
#include <NF/Gameplay/Components.hpp>
#include <NF/Gameplay/GameplayModuleRegistry.hpp>
#include <NF/Physics/Components.hpp>
#include <NF/Scene/Transform.hpp>

#include <cmath>

namespace nf::gameplay {

namespace {

ecs::Entity find_owner(const ecs::World& world, const char* module_name) {
    for (ecs::Entity e : world.query<GameplayModuleComponent>()) {
        const auto* comp = world.get<GameplayModuleComponent>(e);
        if (comp != nullptr && comp->module_name == module_name && comp->enabled) {
            return e;
        }
    }
    return ecs::Entity{};
}

} // namespace

void PlayerControllerModule::on_update(GameplayContext& ctx) {
    // Edit mode: never move. The editor owns Transforms for gizmos/picking
    // while editing; a module writing them would fight the viewport.
    if (!ctx.playing) {
        return;
    }
    if (ctx.world == nullptr || ctx.dt <= 0.0f) {
        return;
    }
    const ecs::Entity owner = find_owner(*ctx.world, name());
    if (!owner.valid() || !ctx.world->is_alive(owner)) {
        return;
    }
    auto* tr = ctx.world->get<scene::Transform>(owner);
    if (tr == nullptr) {
        return;
    }

    float mx = 0.0f;
    float mz = 0.0f;
    bool jump = false;
    bool sprint = false;
    if (settings.use_input && ctx.input != nullptr) {
        // Canonical actions the runtime keyboard source provides. Both the
        // long ("move_forward") and short ("forward") names are honoured so
        // hand-written sources keep working.
        const bool fwd = ctx.input->action_pressed("move_forward") ||
                         ctx.input->action_pressed("forward") || ctx.input->action_pressed("up");
        const bool back = ctx.input->action_pressed("move_back") ||
                          ctx.input->action_pressed("back") || ctx.input->action_pressed("down");
        const bool left = ctx.input->action_pressed("move_left") ||
                          ctx.input->action_pressed("left");
        const bool right = ctx.input->action_pressed("move_right") ||
                           ctx.input->action_pressed("right");
        if (fwd) {
            mz -= 1.0f;
        }
        if (back) {
            mz += 1.0f;
        }
        if (left) {
            mx -= 1.0f;
        }
        if (right) {
            mx += 1.0f;
        }
        // Analogue axes, when the source provides them (gamepad left stick).
        mx += ctx.input->action_axis("move_x");
        mz += ctx.input->action_axis("move_z");
        jump = ctx.input->action_pressed("jump");
        sprint = ctx.input->action_pressed("sprint");
    }
    if (mx != 0.0f || mz != 0.0f || jump) {
        const float len = std::sqrt(mx * mx + mz * mz);
        if (len > 1.0f) {
            mx /= len;
            mz /= len;
        }
        float speed = settings.speed * (sprint ? settings.sprint_multiplier : 1.0f);

        // Dynamic body: steer by velocity so the physics solver stays in
        // charge (contacts, gravity, stacking keep working). Anything else:
        // move the Transform directly.
        auto* rb = ctx.world->get<physics::RigidBodyComponent>(owner);
        if (rb != nullptr && rb->type == physics::BodyType::Dynamic && ctx.physics != nullptr &&
            ctx.physics->is_alive(rb->body)) {
            physics::BodyState st = ctx.physics->state(rb->body);
            st.linear_velocity.x = mx * speed;
            st.linear_velocity.z = mz * speed;
            if (jump && std::abs(st.linear_velocity.y) < 0.5f) {
                st.linear_velocity.y = settings.jump_velocity;
            }
            ctx.physics->set_state(rb->body, st);
            rb->linear_velocity = st.linear_velocity;
        } else {
            tr->local_x += mx * speed * ctx.dt;
            tr->local_z += mz * speed * ctx.dt;
            if (jump) {
                tr->local_y += settings.jump_velocity * ctx.dt;
                if (tr->local_y < 0.0f) {
                    tr->local_y = 0.0f;
                }
            }
            tr->dirty = true;
        }
        ++m_moves;
    }
}

NF_GAMEPLAY_MODULE(PlayerControllerModule, "PlayerController")

} // namespace nf::gameplay
