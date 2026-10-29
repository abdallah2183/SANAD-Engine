#pragma once

#include <NF/Core/Math.hpp>
#include <vector>
#include <cstdint>

namespace nf::animation {

/// Result of a multi-joint FABRIK IK solve.
struct FABRIKResult {
    std::vector<Vec3> solved_positions;
    bool reached = false;
    uint32_t iterations = 0;
    f32 final_error = 0.0f;
};

/// Solves an N-joint kinematic chain using Forward And Backward Reaching Inverse Kinematics (FABRIK).
/// initial_joints: positions in order from root (index 0) to end effector (index N-1).
FABRIKResult solve_fabrik(const std::vector<Vec3>& initial_joints,
                         const Vec3& target,
                         f32 tolerance = 0.001f,
                         uint32_t max_iterations = 20);

/// Result of a LookAt procedural aiming solve.
struct LookAtIKResult {
    Quat delta_rotation = Quat::identity();
    bool clamped = false;
};

/// Computes a head / neck / torso LookAt rotation aiming towards target_pos.
LookAtIKResult solve_look_at(const Vec3& head_pos,
                             const Quat& head_rot,
                             const Vec3& target_pos,
                             const Vec3& forward_axis = Vec3(0, 0, 1),
                             const Vec3& up_axis = Vec3(0, 1, 0),
                             f32 max_yaw_deg = 80.0f,
                             f32 max_pitch_deg = 60.0f,
                             f32 weight = 1.0f);

} // namespace nf::animation
