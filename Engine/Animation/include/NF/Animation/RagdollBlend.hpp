#pragma once

#include <NF/Animation/Skeleton.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <vector>

namespace nf::animation {

/// High-level physics ragdoll simulation state
enum class RagdollState : u8 {
    Kinematic = 0,        ///< Standard keyframed animation drives bones
    BlendingToRagdoll,    ///< Transitioning from animation to ragdoll
    Ragdoll,              ///< Physics simulation controls bones
    BlendingToAnimation   ///< Smoothly recovering from ragdoll to get-up animation
};

/// Character orientation resting on the ground
enum class GroundOrientation : u8 {
    FaceUp = 0,    ///< Supine (chest facing sky, recover via back get-up)
    FaceDown,      ///< Prone (chest facing ground, recover via belly get-up)
    SideLeft,      ///< Resting on left side
    SideRight      ///< Resting on right side
};

/// Configuration for ragdoll blending
struct RagdollConfig {
    f32 blend_to_ragdoll_time = 0.2f;    ///< Default duration to blend into ragdoll (s)
    f32 blend_to_anim_time = 0.45f;      ///< Default duration to blend out of ragdoll (s)
    f32 settle_velocity_threshold = 0.15f; ///< Velocity threshold to declare ragdoll settled
    int32_t pelvis_bone_index = 0;       ///< Hip/pelvis bone index
    int32_t chest_bone_index = 1;        ///< Spine/chest bone index for orientation
};

/// Controller managing smooth transitions between animation and ragdoll physics
class RagdollBlendController {
public:
    RagdollBlendController() = default;
    explicit RagdollBlendController(usize bone_count);

    /// Resize bone arrays and reset weights to default (1.0)
    void resize(usize bone_count);

    /// Configuration access
    [[nodiscard]] const RagdollConfig& config() const noexcept { return m_config; }
    void set_config(const RagdollConfig& config) noexcept { m_config = config; }

    /// Current operational state
    [[nodiscard]] RagdollState state() const noexcept { return m_state; }

    /// Current global blend weight: 0.0 = full animation, 1.0 = full ragdoll
    [[nodiscard]] f32 global_weight() const noexcept { return m_global_weight; }

    /// Trigger ragdoll physics transition
    void start_ragdoll(f32 transition_duration = -1.0f);

    /// Trigger recovery get-up animation transition
    void start_get_up(f32 transition_duration = -1.0f);

    /// Force immediate kinematic animation mode
    void set_kinematic() noexcept;

    /// Update physical ragdoll bone poses
    void set_ragdoll_pose(std::span<const LocalPose> poses);

    /// Update target animated bone poses
    void set_animated_pose(std::span<const LocalPose> poses);

    /// Per-bone ragdoll influence weight [0.0 = strictly animated, 1.0 = full ragdoll]
    void set_bone_weight(usize bone_idx, f32 weight);
    [[nodiscard]] f32 bone_weight(usize bone_idx) const;

    /// Advance transition timers and interpolate blend weights
    void update(f32 dt);

    /// Evaluate final blended pose into out_pose
    void evaluate(std::vector<LocalPose>& out_pose) const;

    /// Detect resting orientation from pelvis/chest world rotation against gravity
    [[nodiscard]] static GroundOrientation detect_ground_orientation(
        const Quat& chest_world_rot,
        const Vec3& world_up = Vec3{0.0f, 1.0f, 0.0f}
    );

    /// Calculate the translation difference between ragdoll hip and animation root
    /// to avoid snapping position when standing up
    [[nodiscard]] static Vec3 compute_root_alignment_offset(
        const Vec3& ragdoll_pelvis_pos,
        const Vec3& anim_root_pos
    ) noexcept;

private:
    RagdollConfig m_config;
    RagdollState m_state = RagdollState::Kinematic;

    f32 m_global_weight = 0.0f;
    f32 m_transition_time = 0.0f;
    f32 m_transition_duration = 0.0f;

    std::vector<LocalPose> m_ragdoll_pose;
    std::vector<LocalPose> m_animated_pose;
    std::vector<f32> m_bone_weights;
};

} // namespace nf::animation
