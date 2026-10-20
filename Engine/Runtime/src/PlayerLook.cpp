// NF/Runtime/PlayerLook.cpp — aiming the camera, as maths.

#include <NF/Runtime/PlayerLook.hpp>

#include <algorithm>
#include <cmath>

namespace nf::runtime {

namespace {

constexpr f32 kDegToRad = 3.14159265358979323846f / 180.0f;
constexpr f32 kRadToDeg = 180.0f / 3.14159265358979323846f;

/// The vertical delta for one frame of input, honouring `invert_y`.
///
/// Both the mouse and the stick want the same thing: positive means "look
/// down". The axis is inverted once, here, instead of in two places that
/// would eventually disagree.
[[nodiscard]] f32 vertical_delta(f32 dy, const LookSettings& settings) {
    return settings.invert_y ? dy : -dy;
}

} // namespace

f32 look_wrap_deg(f32 degrees) {
    if (!std::isfinite(degrees)) {
        return 0.0f;
    }
    f32 wrapped = std::fmod(degrees, 360.0f);
    // fmod keeps the sign of the input, so a negative angle has to be
    // brought forward rather than assumed to be in range.
    if (wrapped <= -180.0f) {
        wrapped += 360.0f;
    } else if (wrapped > 180.0f) {
        wrapped -= 360.0f;
    }
    return wrapped;
}

f32 look_angle_delta(f32 from, f32 to) {
    return look_wrap_deg(to - from);
}

LookState look_apply_mouse(LookState state, f32 dx_pixels, f32 dy_pixels,
                           const LookSettings& settings) {
    if (!std::isfinite(dx_pixels) || !std::isfinite(dy_pixels)) {
        return state;
    }
    // Moving the pointer right turns the camera right. yaw grows
    // counter-clockwise, so turning right SUBTRACTS from it.
    state.yaw -= dx_pixels * settings.mouse_sensitivity;
    state.pitch += vertical_delta(dy_pixels, settings) * settings.mouse_sensitivity;
    return look_clamp(state, settings);
}

LookState look_apply_stick(LookState state, f32 sx, f32 sy, f32 dt,
                           const LookSettings& settings) {
    if (dt <= 0.0f || !std::isfinite(sx) || !std::isfinite(sy)) {
        return state;
    }
    const f32 step = settings.stick_sensitivity * dt;
    state.yaw -= std::clamp(sx, -1.0f, 1.0f) * step;
    state.pitch += vertical_delta(std::clamp(sy, -1.0f, 1.0f), settings) * step;
    return look_clamp(state, settings);
}

LookState look_clamp(LookState state, const LookSettings& settings) {
    // A scene may author a range that is upside down (min above max). The
    // order is repaired rather than rejected: a camera that cannot move is
    // worse than one that moves less than asked.
    f32 lo = std::min(settings.pitch_min, settings.pitch_max);
    f32 hi = std::max(settings.pitch_min, settings.pitch_max);
    // Beyond the poles the orbit flips. Clamping just inside them keeps
    // "up" up.
    lo = std::max(lo, -89.9f);
    hi = std::min(hi, 89.9f);
    if (!std::isfinite(state.pitch)) {
        state.pitch = 0.0f;
    }
    state.pitch = std::clamp(state.pitch, lo, hi);
    state.yaw = look_wrap_deg(state.yaw);
    return state;
}

f32 look_yaw_for_direction(f32 dir_x, f32 dir_z, f32 fallback) {
    const f32 len2 = dir_x * dir_x + dir_z * dir_z;
    if (!std::isfinite(len2) || len2 <= 1e-8f) {
        return fallback;
    }
    // The camera looks along (-sin yaw, -cos yaw); being BEHIND a movement
    // direction means looking the same way it is going.
    return look_wrap_deg(std::atan2(-dir_x, -dir_z) * kRadToDeg);
}

f32 look_align(f32 yaw, f32 desired, f32 rate, f32 dt) {
    if (rate <= 0.0f || dt <= 0.0f) {
        return yaw;
    }
    const f32 delta = look_angle_delta(yaw, desired);
    const f32 max_step = rate * dt;
    // The shortest way round, capped: a large frame time must not let the
    // camera jump the whole way in one frame, and a small one must not
    // overshoot and oscillate.
    const f32 step = std::clamp(delta, -max_step, max_step);
    return look_wrap_deg(yaw + step);
}

f32 follow_zoom(f32 distance, f32 wheel, f32 step, f32 min_distance, f32 max_distance) {
    if (!std::isfinite(distance)) {
        distance = 0.0f;
    }
    if (!std::isfinite(wheel) || !std::isfinite(step)) {
        return distance;
    }
    f32 lo = std::max(0.0f, std::min(min_distance, max_distance));
    f32 hi = std::max(lo, std::max(min_distance, max_distance));
    // Forward on the wheel is "closer": the number the player turns towards
    // is the one they are looking at.
    const f32 wanted = distance - wheel * step;
    return std::clamp(wanted, lo, hi);
}

void move_relative(f32 move_x, f32 move_z, f32 camera_yaw_deg, f32& out_x, f32& out_z) {
    const f32 yaw = camera_yaw_deg * kDegToRad;
    const f32 c = std::cos(yaw);
    const f32 s = std::sin(yaw);
    // The camera's view direction is (-sin, -cos) and its right is
    // (cos, -sin). A forward press is move_z = -1, so the world vector is
    //   move_x * right + (-move_z) * forward.
    // Written out, that is the rotation below — a rotation, so it keeps the
    // input's length and a diagonal stays as fast as a straight line.
    out_x = move_x * c + move_z * s;
    out_z = -move_x * s + move_z * c;
}

void stick_deadzone(f32& x, f32& y, f32 deadzone) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        x = 0.0f;
        y = 0.0f;
        return;
    }
    const f32 dz = std::clamp(deadzone, 0.0f, 0.95f);
    const f32 magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= dz || magnitude <= 1e-6f) {
        x = 0.0f;
        y = 0.0f;
        return;
    }
    // Rescale what is left of the travel to the full range, so a stick
    // pushed just past the deadzone moves slowly instead of jumping.
    const f32 scaled = std::min((magnitude - dz) / (1.0f - dz), 1.0f) / magnitude;
    x *= scaled;
    y *= scaled;
}

} // namespace nf::runtime
