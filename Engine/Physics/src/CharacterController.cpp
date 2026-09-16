// NF/Physics/CharacterController.cpp — dynamic-body character mover.

#include <NF/Physics/CharacterController.hpp>

#include <algorithm>
#include <cmath>

namespace nf::physics {

CharacterController::CharacterController(PhysicsWorld& world, const CharacterConfig& config,
                                         Vec3 spawn)
    : m_world(&world), m_config(config) {
    BodyDesc desc;
    desc.type = BodyType::Dynamic;
    desc.shape = Shape::make_sphere(config.radius > 0.05f ? config.radius : 0.4f);
    desc.position = spawn;
    desc.mass = config.mass > 0.0f ? config.mass : 80.0f;
    desc.friction = config.friction;
    desc.restitution = 0.0f; // characters never bounce
    desc.allow_sleep = false;
    m_body = world.add_body(desc);
}

CharacterController::~CharacterController() {
    if (m_world && m_body.valid()) {
        m_world->remove_body(m_body);
    }
}

void CharacterController::move(Vec3 wish_dir, bool jump, f32 dt) {
    if (!m_world || !m_body.valid() || !m_world->is_alive(m_body)) return;
    wish_dir.y = 0.0f; // horizontal intent only; gravity owns Y
    const float wish_len = wish_dir.length();
    Vec3 wish = wish_len > 1e-6f ? wish_dir / wish_len : Vec3{};
    const float target_speed = std::min(wish_len, 1.0f) * m_config.max_speed;

    BodyState st = m_world->state(m_body);
    const float accel = m_config.acceleration * (m_grounded ? 1.0f : m_config.air_control);
    Vec3 hv{st.linear_velocity.x, 0.0f, st.linear_velocity.z};
    const Vec3 target{wish.x * target_speed, 0.0f, wish.z * target_speed};
    Vec3 delta = target - hv;
    const float max_delta = accel * (dt > 0.0f ? dt : 0.0f);
    const float delta_len = delta.length();
    if (delta_len > max_delta && delta_len > 1e-9f) {
        delta = delta / delta_len * max_delta;
    }
    hv = hv + delta;
    Vec3 v{hv.x, st.linear_velocity.y, hv.z};
    if (jump && m_grounded) {
        v.y = m_config.jump_speed;
        m_grounded = false; // leaving the ground this tick
    }
    m_world->set_velocity(m_body, v, Vec3{});
}

void CharacterController::post_step() {
    m_grounded = false;
    if (!m_world || !m_body.valid() || !m_world->is_alive(m_body)) return;
    const float cos_limit = std::cos(m_config.slope_limit_deg * 3.14159265358979323846f / 180.0f);
    for (const Manifold& m : m_world->last_manifolds()) {
        if (m.point_count == 0) continue;
        // Manifold normal points A -> B: the ground pushes the character
        // along -normal when we are A, +normal when we are B.
        const bool we_are_a = (m.body_a == m_body.index);
        const bool we_are_b = (m.body_b == m_body.index);
        if (!we_are_a && !we_are_b) continue;
        const float up = we_are_a ? -m.normal.y : m.normal.y;
        if (up >= cos_limit) {
            m_grounded = true;
            return;
        }
    }
}

Vec3 CharacterController::position() const {
    if (!m_world || !m_body.valid()) return {};
    return m_world->state(m_body).position;
}

Vec3 CharacterController::velocity() const {
    if (!m_world || !m_body.valid()) return {};
    return m_world->state(m_body).linear_velocity;
}

} // namespace nf::physics
