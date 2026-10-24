#include <NF/Runtime/CombatAI.hpp>
#include <NF/Runtime/Suppression.hpp>

#include <algorithm>
#include <cmath>

namespace nf::runtime {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kDegToRad = kPi / 180.0f;
constexpr f32 kRadToDeg = 180.0f / kPi;

f32 wrap_deg(f32 deg) noexcept {
    f32 d = std::fmod(deg, 360.0f);
    if (d <= -180.0f) {
        d += 360.0f;
    } else if (d > 180.0f) {
        d -= 360.0f;
    }
    return d;
}

} // namespace

Vec3 aim_direction(f32 yaw_deg, f32 pitch_deg) noexcept {
    const f32 yaw = yaw_deg * kDegToRad;
    const f32 pitch = pitch_deg * kDegToRad;
    const f32 cp = std::cos(pitch);
    // The same convention as follow_camera_direction, negated: that one points
    // from the pivot TO the camera, this one points where the agent looks.
    return Vec3{-std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp}.normalized();
}

void aim_angles(const Vec3& direction, f32& out_yaw_deg, f32& out_pitch_deg) noexcept {
    const f32 length = direction.length();
    if (!(length > 1e-6f) || !std::isfinite(length)) {
        out_yaw_deg = 0.0f;
        out_pitch_deg = 0.0f;
        return;
    }
    const Vec3 n = direction / length;
    out_pitch_deg = std::asin(std::clamp(n.y, -1.0f, 1.0f)) * kRadToDeg;
    out_yaw_deg = std::atan2(-n.x, -n.z) * kRadToDeg;
}

f32 angle_delta_deg(f32 from_deg, f32 to_deg) noexcept {
    return wrap_deg(to_deg - from_deg);
}

CombatAimOutput combat_aim_step(CombatAimState& state, const CombatAimConfig& config,
                                const CombatAimInput& input) {
    CombatAimOutput out;
    out.yaw_deg = state.yaw_deg;
    out.pitch_deg = state.pitch_deg;

    // A paused frame changes nothing: an enemy must not line up its shot while
    // the game is stopped.
    if (!(input.dt > 0.0f) || !std::isfinite(input.dt)) {
        return out;
    }

    // --- 1. Notice ------------------------------------------------------
    if (!input.has_target) {
        // The target is gone. The agent keeps pointing where it was (turning
        // to "straight ahead" the instant a player breaks line of sight reads
        // as a bug, and it also gives away that it stopped tracking).
        state.engaged = false;
        state.reaction_timer = 0.0f;
        state.trigger = false;
        state.burst_timer = 0.0f;
        return out;
    }
    if (!state.engaged) {
        state.engaged = true;
        state.reaction_timer = 0.0f;
        state.burst_timer = 0.0f;
        state.trigger = false;
    }
    state.reaction_timer += input.dt;

    // --- 2. Turn --------------------------------------------------------
    f32 want_yaw = 0.0f;
    f32 want_pitch = 0.0f;
    aim_angles(input.target - input.eye, want_yaw, want_pitch);
    const f32 lo = std::min(config.pitch_min, config.pitch_max);
    const f32 hi = std::max(config.pitch_min, config.pitch_max);
    want_pitch = std::clamp(want_pitch, lo, hi);

    const f32 suppression = std::clamp(std::isfinite(input.suppression) ? input.suppression
                                                                        : 0.0f,
                                       0.0f, 1.0f);
    const f32 speed =
        std::max(0.0f, config.aim_speed_deg_per_s) * suppression_aim_scale(suppression);
    const f32 max_step = speed * input.dt;
    const f32 yaw_step = std::clamp(angle_delta_deg(state.yaw_deg, want_yaw), -max_step, max_step);
    const f32 pitch_step = std::clamp(want_pitch - state.pitch_deg, -max_step, max_step);
    state.yaw_deg = wrap_deg(state.yaw_deg + yaw_step);
    state.pitch_deg = std::clamp(state.pitch_deg + pitch_step, lo, hi);

    // The error is measured AFTER the turn, so the frame an agent finishes
    // aiming is the frame it may fire — not the one after.
    const f32 rem_yaw = angle_delta_deg(state.yaw_deg, want_yaw);
    const f32 rem_pitch = want_pitch - state.pitch_deg;
    const f32 error = std::sqrt(rem_yaw * rem_yaw + rem_pitch * rem_pitch);

    // --- 3. Decide ------------------------------------------------------
    const f32 cone = std::max(0.0f, config.fire_cone_deg) * suppression_cone_scale(suppression);
    const bool aimed = error <= cone;
    const bool ready = state.reaction_timer >= std::max(0.0f, config.reaction_seconds);
    const bool may_fire =
        aimed && ready && input.visible && !suppression_blocks_fire(suppression);

    // --- 4. Discipline --------------------------------------------------
    if (!may_fire) {
        state.trigger = false;
        // Reset to "start of a burst", not to a full pause: the moment the
        // agent is allowed to fire again it fires, rather than serving out a
        // rest it never earned by shooting. (The rest phase below is what a
        // FINISHED burst leads into.)
        state.burst_timer = 0.0f;
    } else if (config.burst_on_seconds <= 0.0f) {
        // A zero on-time is the documented "hold the trigger while aimed".
        state.trigger = true;
        state.burst_timer = 0.0f;
    } else if (state.trigger) {
        state.burst_timer -= input.dt;
        if (state.burst_timer <= 0.0f) {
            state.trigger = false;
            state.burst_timer = std::max(0.0f, config.burst_off_seconds);
        }
    } else {
        state.burst_timer -= input.dt;
        if (state.burst_timer <= 0.0f) {
            state.trigger = true;
            state.burst_timer = config.burst_on_seconds;
        }
    }

    out.yaw_deg = state.yaw_deg;
    out.pitch_deg = state.pitch_deg;
    out.trigger = state.trigger;
    out.aimed = aimed;
    out.error_deg = error;
    return out;
}

} // namespace nf::runtime
