#include <NF/Scene2D/CameraFollower2D.hpp>

#include <algorithm>
#include <cmath>

namespace nf::scene2d {

namespace {

static float smooth_damp(float current, float target, float& current_velocity, float smooth_time, float dt, float max_speed) {
    smooth_time = std::max(0.0001f, smooth_time);
    const float omega = 2.0f / smooth_time;
    const float x = omega * dt;
    const float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);

    float change = current - target;
    const float original_to = target;

    const float max_change = max_speed * dt;
    change = std::clamp(change, -max_change, max_change);
    target = current - change;

    const float temp = (current_velocity + omega * change) * dt;
    current_velocity = (current_velocity - omega * temp) * exp;
    float output = target + (change + temp) * exp;

    if ((original_to - current > 0.0f) == (output > original_to)) {
        output = original_to;
        current_velocity = (output - original_to) / dt;
    }

    return output;
}

} // namespace

CameraFollower2D::CameraFollower2D(CameraFollower2DConfig config)
    : m_config(config) {}

void CameraFollower2D::snap_to(Vec2 pos) {
    m_current_pos = pos;
    m_target_focus = pos;
    m_velocity = Vec2{0.0f, 0.0f};
    m_lookahead_offset = Vec2{0.0f, 0.0f};
    m_lookahead_velocity = Vec2{0.0f, 0.0f};
    m_shake_offset = Vec2{0.0f, 0.0f};
    m_shake_roll = 0.0f;
}

void CameraFollower2D::add_trauma(f32 amount) noexcept {
    m_trauma = std::clamp(m_trauma + amount, 0.0f, 1.0f);
}

void CameraFollower2D::reset() {
    m_velocity = Vec2{0.0f, 0.0f};
    m_lookahead_offset = Vec2{0.0f, 0.0f};
    m_lookahead_velocity = Vec2{0.0f, 0.0f};
    m_trauma = 0.0f;
    m_shake_timer = 0.0f;
    m_shake_offset = Vec2{0.0f, 0.0f};
    m_shake_roll = 0.0f;
}

void CameraFollower2D::update(Vec2 target_pos, Vec2 target_vel, f32 dt) {
    if (dt <= 0.0f) {
        return;
    }

    // 1. Deadzone evaluation
    const f32 dx = target_pos.x - m_target_focus.x;
    if (dx > m_config.deadzone.x) {
        m_target_focus.x = target_pos.x - m_config.deadzone.x;
    } else if (dx < -m_config.deadzone.x) {
        m_target_focus.x = target_pos.x + m_config.deadzone.x;
    }

    const f32 dy = target_pos.y - m_target_focus.y;
    if (dy > m_config.deadzone.y) {
        m_target_focus.y = target_pos.y - m_config.deadzone.y;
    } else if (dy < -m_config.deadzone.y) {
        m_target_focus.y = target_pos.y + m_config.deadzone.y;
    }

    // 2. Velocity lookahead
    const Vec2 target_lookahead{
        target_vel.x * m_config.lookahead_factor.x,
        target_vel.y * m_config.lookahead_factor.y
    };

    m_lookahead_offset.x = smooth_damp(
        m_lookahead_offset.x,
        target_lookahead.x,
        m_lookahead_velocity.x,
        m_config.lookahead_smooth_time,
        dt,
        m_config.max_speed
    );

    m_lookahead_offset.y = smooth_damp(
        m_lookahead_offset.y,
        target_lookahead.y,
        m_lookahead_velocity.y,
        m_config.lookahead_smooth_time,
        dt,
        m_config.max_speed
    );

    // 3. Desired position
    Vec2 desired = m_target_focus + m_lookahead_offset;

    if (m_config.bounds_enabled) {
        desired.x = std::clamp(desired.x, m_config.bounds_min.x, m_config.bounds_max.x);
        desired.y = std::clamp(desired.y, m_config.bounds_min.y, m_config.bounds_max.y);
    }

    // 4. Critically damped position smoothing
    m_current_pos.x = smooth_damp(
        m_current_pos.x,
        desired.x,
        m_velocity.x,
        m_config.smooth_time.x,
        dt,
        m_config.max_speed
    );

    m_current_pos.y = smooth_damp(
        m_current_pos.y,
        desired.y,
        m_velocity.y,
        m_config.smooth_time.y,
        dt,
        m_config.max_speed
    );

    if (m_config.bounds_enabled) {
        m_current_pos.x = std::clamp(m_current_pos.x, m_config.bounds_min.x, m_config.bounds_max.x);
        m_current_pos.y = std::clamp(m_current_pos.y, m_config.bounds_min.y, m_config.bounds_max.y);
    }

    // 5. Trauma & Screen Shake
    m_shake_timer += dt * m_config.shake_frequency;
    m_trauma = std::max(0.0f, m_trauma - m_config.trauma_decay * dt);

    const f32 shake = m_trauma * m_trauma; // Non-linear quadratic falloff
    if (shake > 1e-4f) {
        const f32 nx = std::sin(m_shake_timer) * 0.7f + std::sin(m_shake_timer * 1.71f + 1.2f) * 0.3f;
        const f32 ny = std::cos(m_shake_timer * 1.33f + 0.4f) * 0.7f + std::sin(m_shake_timer * 0.89f + 2.1f) * 0.3f;
        const f32 nr = std::sin(m_shake_timer * 0.75f + 1.9f) * 0.7f + std::cos(m_shake_timer * 1.45f + 0.8f) * 0.3f;

        m_shake_offset.x = m_config.max_shake_offset.x * shake * nx;
        m_shake_offset.y = m_config.max_shake_offset.y * shake * ny;
        m_shake_roll = m_config.max_shake_roll_deg * shake * nr;
    } else {
        m_shake_offset = Vec2{0.0f, 0.0f};
        m_shake_roll = 0.0f;
    }
}

void CameraFollower2D::apply_to_camera(Camera2D& camera) const {
    camera.position = final_position();
    camera.rotation_deg += m_shake_roll;
}

} // namespace nf::scene2d
