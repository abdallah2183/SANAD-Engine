#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace nf::animation {

/// Angular clamp limits in degrees for procedural aim / look-at
struct LookAtLimits {
    f32 yaw_min_deg = -75.0f;       ///< Left rotation boundary (negative)
    f32 yaw_max_deg = 75.0f;        ///< Right rotation boundary (positive)
    f32 pitch_min_deg = -50.0f;     ///< Downward rotation boundary (negative)
    f32 pitch_max_deg = 50.0f;      ///< Upward rotation boundary (positive)
    f32 deadzone_angle_deg = 0.5f;  ///< Angular deviation threshold below which rotation stays locked
};

/// Joint within a hierarchical look-at chain (e.g. spine_01, spine_02, neck, head)
struct LookAtBone {
    std::string name;
    f32 weight = 1.0f;              ///< Relative distribution weight for this bone
    Vec3 local_forward{0.0f, 0.0f, 1.0f}; ///< Local forward axis
    Vec3 local_up{0.0f, 1.0f, 0.0f};      ///< Local up axis
    Quat current_offset = Quat::identity(); ///< Solved local rotation offset applied to this bone
};

/// Configuration for look-at solver
struct LookAtConfig {
    f32 global_weight = 1.0f;              ///< Master blend factor [0, 1]
    f32 smoothing_speed = 12.0f;           ///< Exponential smoothing rate per second
    f32 max_angular_velocity_deg = 360.0f; ///< Maximum rotation speed in degrees per second
    LookAtLimits limits;
};

/// Result of evaluating look-at solver
struct LookAtResult {
    f32 target_yaw_deg = 0.0f;
    f32 target_pitch_deg = 0.0f;
    f32 current_yaw_deg = 0.0f;
    f32 current_pitch_deg = 0.0f;
    bool clamped = false;                ///< True if target was outside yaw/pitch limits
    bool in_deadzone = false;            ///< True if target was within deadzone angle
    Vec3 current_look_dir{0.0f, 0.0f, 1.0f}; ///< Solved world look direction
};

/// Multi-joint procedural look-at and aiming IK solver
class LookAtSolver {
public:
    explicit LookAtSolver(LookAtConfig config = {});

    /// Add a bone to the look-at chain (e.g. root-to-tip order: spine -> neck -> head)
    void add_bone(std::string name, f32 weight, Vec3 local_forward = {0.0f, 0.0f, 1.0f}, Vec3 local_up = {0.0f, 1.0f, 0.0f});

    /// Clear all bones from the chain
    void clear_bones() noexcept;

    /// Number of bones in the chain
    [[nodiscard]] usize bone_count() const noexcept { return m_bones.size(); }

    /// Bone accessors
    [[nodiscard]] LookAtBone* get_bone(usize index);
    [[nodiscard]] const LookAtBone* get_bone(usize index) const;

    /// Update look-at orientation and distribute rotations across chain
    /// character_pos: world position of character pivot/eyes
    /// character_facing: world forward direction of character body (normalized)
    /// target_world_pos: world position of target
    /// dt: delta time in seconds
    LookAtResult update(
        const Vec3& character_pos,
        const Vec3& character_facing,
        const Vec3& target_world_pos,
        f32 dt
    );

    /// Instant reset to neutral forward orientation
    void reset() noexcept;

    /// Configuration access
    [[nodiscard]] const LookAtConfig& config() const noexcept { return m_config; }
    void set_config(LookAtConfig cfg) noexcept { m_config = cfg; }
    LookAtConfig& config() noexcept { return m_config; }

    /// Current evaluation result
    [[nodiscard]] const LookAtResult& result() const noexcept { return m_result; }

private:
    LookAtConfig m_config;
    std::vector<LookAtBone> m_bones;
    LookAtResult m_result;
    Quat m_current_rot = Quat::identity();
};

} // namespace nf::animation
