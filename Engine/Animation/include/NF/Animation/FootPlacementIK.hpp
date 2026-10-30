#pragma once

#include <NF/Animation/IK.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <algorithm>
#include <cmath>

namespace nf::animation {

/// Information about ground contact below a foot
struct GroundHit {
    Vec3 point{0.0f, 0.0f, 0.0f};  ///< Contact position on ground surface
    Vec3 normal{0.0f, 1.0f, 0.0f}; ///< Surface normal (upward facing)
    bool has_ground = false;       ///< True if ground was detected
};

/// Configuration for foot placement and ground alignment
struct FootPlacementConfig {
    f32 max_step_height = 0.5f;       ///< Maximum vertical adjustment distance
    f32 max_ground_angle_deg = 45.0f; ///< Maximum slope angle for ankle alignment
    f32 interp_speed = 18.0f;         ///< Exponential smoothing speed
    f32 foot_height_offset = 0.05f;   ///< Sole thickness/offset above terrain
    bool enable_pelvis_drop = true;   ///< Automatically drop pelvis when stepping down
    f32 pelvis_drop_scale = 1.0f;     ///< Multiplier for pelvis vertical drop
};

/// Target and state for a single foot
struct FootPlacementTarget {
    Vec3 animated_pos{0.0f, 0.0f, 0.0f}; ///< Nominal animation position
    Vec3 target_pos{0.0f, 0.0f, 0.0f};   ///< Solved IK target position
    Quat ankle_rotation_offset = Quat::identity(); ///< Procedural rotation to align with slope
    f32 height_offset = 0.0f;            ///< Smoothed vertical offset
    f32 raw_target_height = 0.0f;        ///< Raw target offset from ground hit
};

/// Three-joint leg chain for two-bone IK
struct LegChain {
    Vec3 hip_pos{0.0f, 1.0f, 0.0f};
    Vec3 knee_pos{0.0f, 0.5f, 0.0f};
    Vec3 ankle_pos{0.0f, 0.0f, 0.0f};
    Vec3 pole_hint{0.0f, 0.0f, 1.0f};
    TwoBoneIKResult ik_result;
};

/// Two-legged character foot placement solver
class FootPlacementSolver {
public:
    explicit FootPlacementSolver(FootPlacementConfig config = {});

    /// Update single foot target based on ground hit
    void evaluate_foot(FootPlacementTarget& foot, const GroundHit& hit, f32 dt);

    /// Solve biped foot placement for left and right legs
    void solve_biped(
        LegChain& left_leg,
        LegChain& right_leg,
        FootPlacementTarget& left_foot,
        FootPlacementTarget& right_foot,
        const GroundHit& left_hit,
        const GroundHit& right_hit,
        f32 dt,
        f32* out_pelvis_drop = nullptr
    );

    /// Current smoothed pelvis drop offset
    [[nodiscard]] f32 current_pelvis_drop() const noexcept { return m_pelvis_drop; }

    /// Access configuration
    [[nodiscard]] const FootPlacementConfig& config() const noexcept { return m_config; }
    void set_config(FootPlacementConfig cfg) noexcept { m_config = cfg; }

    /// Reset internal smoothing history
    void reset() noexcept;

private:
    FootPlacementConfig m_config;
    f32 m_pelvis_drop = 0.0f;
};

} // namespace nf::animation
