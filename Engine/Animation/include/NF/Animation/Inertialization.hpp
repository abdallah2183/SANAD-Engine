#pragma once

#include <NF/Animation/Skeleton.hpp>
#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <algorithm>
#include <vector>

namespace nf::animation {

/// State tracking bone offset, velocity, and decay dynamics during an inertial transition.
struct BoneInertialState {
    Vec3 transOffset{0.0f, 0.0f, 0.0f};
    Vec3 transVelocity{0.0f, 0.0f, 0.0f};

    Vec3 rotAngleAxis{0.0f, 0.0f, 0.0f};    // Axis * angle (radians) offset
    Vec3 rotAngularVelocity{0.0f, 0.0f, 0.0f}; // Angular velocity vector (radians/sec)

    Vec3 scaleOffset{0.0f, 0.0f, 0.0f};
    Vec3 scaleVelocity{0.0f, 0.0f, 0.0f};
};

/// High-performance inertializer for seamless, pop-free animation transitions.
/// Based on the quintic Hermitian polynomial decay function D(t) = (1 - t/t0)^3 * (A + B*t + C*t^2).
class Inertializer {
public:
    Inertializer() = default;

    /// Evaluates whether an inertial transition is currently active and decaying.
    [[nodiscard]] bool is_active() const { return m_elapsedTime < m_duration && m_duration > 0.0f; }

    /// Total duration of the active transition in seconds.
    [[nodiscard]] float duration() const { return m_duration; }

    /// Elapsed time since the transition was triggered.
    [[nodiscard]] float elapsed_time() const { return m_elapsedTime; }

    /// Fraction of transition completed [0, 1].
    [[nodiscard]] float progress() const {
        if (m_duration <= 0.0f) return 1.0f;
        return std::clamp(m_elapsedTime / m_duration, 0.0f, 1.0f);
    }

    /// Number of tracked bones in the active transition state.
    [[nodiscard]] usize bone_count() const { return m_boneStates.size(); }

    /// Triggers an inertial transition from current outgoing pose to target incoming pose.
    void transition(
        const std::vector<LocalPose>& prevPose,
        const std::vector<LocalPose>& prevVel,
        const std::vector<LocalPose>& targetPose,
        const std::vector<LocalPose>& targetVel,
        float blendDuration
    );

    /// Simplified overload where velocities are approximated from previous frame delta.
    void transition_from_previous(
        const std::vector<LocalPose>& currentPose,
        const std::vector<LocalPose>& previousPose,
        const std::vector<LocalPose>& targetPose,
        float dt,
        float blendDuration
    );

    /// Updates the decay timer by dt and blends the target incoming pose with decaying inertial offsets.
    /// Returns true if blending was applied; false if transition is complete.
    bool update_and_blend(
        float dt,
        const std::vector<LocalPose>& targetPose,
        std::vector<LocalPose>& outPose
    );

    /// Computes the quintic polynomial decay value for scalar offset x0 and velocity v0 over duration t0.
    [[nodiscard]] static float decay_quintic(float x0, float v0, float t, float t0);

    /// Computes the 3D vector quintic decay.
    [[nodiscard]] static Vec3 decay_vec3(const Vec3& x0, const Vec3& v0, float t, float t0);

    /// Resets / cancels any active transition.
    void reset();

private:
    float m_duration = 0.0f;
    float m_elapsedTime = 0.0f;
    std::vector<BoneInertialState> m_boneStates;
};

} // namespace nf::animation
