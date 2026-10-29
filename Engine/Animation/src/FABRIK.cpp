#include <NF/Animation/FABRIK.hpp>
#include <algorithm>
#include <cmath>

namespace nf::animation {

FABRIKResult solve_fabrik(const std::vector<Vec3>& initial_joints,
                         const Vec3& target,
                         f32 tolerance,
                         uint32_t max_iterations) {
    FABRIKResult res;
    if (initial_joints.size() < 2) {
        res.solved_positions = initial_joints;
        res.reached = true;
        return res;
    }

    size_t n = initial_joints.size();
    res.solved_positions = initial_joints;

    std::vector<f32> lengths(n - 1);
    f32 total_length = 0.0f;
    for (size_t i = 0; i < n - 1; ++i) {
        Vec3 diff = initial_joints[i + 1] - initial_joints[i];
        lengths[i] = diff.length();
        total_length += lengths[i];
    }

    Vec3 root_pos = initial_joints[0];
    Vec3 root_to_target = target - root_pos;
    f32 dist_to_target = root_to_target.length();

    // Target is unreachable
    if (dist_to_target > total_length) {
        Vec3 dir = (dist_to_target > 0.0001f) ? (root_to_target / dist_to_target) : Vec3(0, 0, 1);
        for (size_t i = 0; i < n - 1; ++i) {
            res.solved_positions[i + 1] = res.solved_positions[i] + dir * lengths[i];
        }
        res.reached = false;
        res.iterations = 1;
        res.final_error = dist_to_target - total_length;
        return res;
    }

    // Reachable
    uint32_t iter = 0;
    f32 err = (res.solved_positions[n - 1] - target).length();

    while (err > tolerance && iter < max_iterations) {
        // Forward pass: end effector to base
        res.solved_positions[n - 1] = target;
        for (size_t i = n - 1; i > 0; --i) {
            size_t prev = i - 1;
            Vec3 diff = res.solved_positions[prev] - res.solved_positions[i];
            f32 r = diff.length();
            if (r > 0.00001f) {
                f32 lambda = lengths[prev] / r;
                res.solved_positions[prev] = res.solved_positions[i] + diff * lambda;
            } else {
                res.solved_positions[prev] = res.solved_positions[i] + Vec3(0, 0, lengths[prev]);
            }
        }

        // Backward pass: base to end effector
        res.solved_positions[0] = root_pos;
        for (size_t i = 0; i < n - 1; ++i) {
            size_t next = i + 1;
            Vec3 diff = res.solved_positions[next] - res.solved_positions[i];
            f32 r = diff.length();
            if (r > 0.00001f) {
                f32 lambda = lengths[i] / r;
                res.solved_positions[next] = res.solved_positions[i] + diff * lambda;
            } else {
                res.solved_positions[next] = res.solved_positions[i] + Vec3(0, 0, lengths[i]);
            }
        }

        err = (res.solved_positions[n - 1] - target).length();
        iter++;
    }

    res.reached = (err <= tolerance);
    res.iterations = iter;
    res.final_error = err;
    return res;
}

LookAtIKResult solve_look_at(const Vec3& head_pos,
                             const Quat& head_rot,
                             const Vec3& target_pos,
                             const Vec3& forward_axis,
                             const Vec3& up_axis,
                             f32 max_yaw_deg,
                             f32 max_pitch_deg,
                             f32 weight) {
    (void)up_axis;
    LookAtIKResult res;
    if (weight <= 0.0001f) {
        return res;
    }

    Vec3 dir_to_target = target_pos - head_pos;
    f32 dist = dir_to_target.length();
    if (dist < 0.0001f) {
        return res;
    }
    dir_to_target = dir_to_target / dist;

    Vec3 current_fwd = head_rot.rotate(forward_axis);
    f32 fwd_len = current_fwd.length();
    if (fwd_len > 0.0001f) {
        current_fwd = current_fwd / fwd_len;
    } else {
        current_fwd = Vec3(0, 0, 1);
    }

    f32 dot = std::clamp(current_fwd.dot(dir_to_target), -1.0f, 1.0f);
    f32 angle_rad = std::acos(dot);
    f32 angle_deg = angle_rad * (180.0f / 3.1415926535f);

    f32 max_allowed_deg = std::max(max_yaw_deg, max_pitch_deg);
    if (angle_deg > max_allowed_deg) {
        res.clamped = true;
        angle_rad = max_allowed_deg * (3.1415926535f / 180.0f);
    }

    Vec3 axis = current_fwd.cross(dir_to_target);
    f32 axis_len = axis.length();
    if (axis_len < 0.0001f) {
        return res;
    }
    axis = axis / axis_len;

    Quat full_rot = Quat::from_axis_angle(axis, angle_rad);
    res.delta_rotation = Quat::slerp(Quat::identity(), full_rot, std::clamp(weight, 0.0f, 1.0f));
    return res;
}

} // namespace nf::animation
