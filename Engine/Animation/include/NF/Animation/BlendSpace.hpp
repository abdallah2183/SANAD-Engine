#pragma once

#include <NF/Animation/AnimationClip.hpp>
#include <NF/Animation/Skeleton.hpp>
#include <NF/Core/Math.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>

namespace nf::animation {

/// A sample point in a 1D blend space.
struct BlendSample1D {
    std::string clip_name;
    f32 position = 0.0f;
};

/// Continuous 1D Blend Space (e.g. Speed: Idle (0) -> Walk (2) -> Run (5)).
class BlendSpace1D {
public:
    BlendSpace1D() = default;

    /// Adds a sample point. Samples are sorted automatically by position.
    void add_sample(const std::string& clip_name, f32 position);

    const std::vector<BlendSample1D>& samples() const { return m_samples; }
    size_t sample_count() const { return m_samples.size(); }

    /// Calculates active sample indices and normalized weights [0, 1] for a parameter value.
    void get_blend_weights(f32 param, std::vector<std::pair<size_t, f32>>& out_weights) const;

    /// Computes the blended clip duration at parameter value for phase synchronization.
    f32 get_synced_duration(f32 param, const std::unordered_map<std::string, const AnimationClip*>& clips) const;

    /// Evaluates and samples the blend space into out_pose.
    /// normalized_time is in [0, 1] representing the cycle phase.
    void evaluate(f32 param,
                   f32 normalized_time,
                   const Skeleton& skel,
                   const std::unordered_map<std::string, const AnimationClip*>& clips,
                   std::vector<LocalPose>& out_pose) const;

private:
    std::vector<BlendSample1D> m_samples;
};

/// A sample point in a 2D blend space.
struct BlendSample2D {
    std::string clip_name;
    Vec2 position{0.0f, 0.0f};
};

/// Continuous 2D Blend Space (e.g. Direction/Speed: Idle, WalkFwd, WalkBack, StrafeLeft, StrafeRight).
class BlendSpace2D {
public:
    BlendSpace2D() = default;

    void add_sample(const std::string& clip_name, const Vec2& position);

    const std::vector<BlendSample2D>& samples() const { return m_samples; }
    size_t sample_count() const { return m_samples.size(); }

    /// Computes blend weights using Inverse Distance Weighting (IDW).
    void get_blend_weights(const Vec2& param, std::vector<std::pair<size_t, f32>>& out_weights) const;

    /// Computes the blended clip duration at parameter position.
    f32 get_synced_duration(const Vec2& param, const std::unordered_map<std::string, const AnimationClip*>& clips) const;

    /// Evaluates and blends all active clips at the given normalized time phase.
    void evaluate(const Vec2& param,
                   f32 normalized_time,
                   const Skeleton& skel,
                   const std::unordered_map<std::string, const AnimationClip*>& clips,
                   std::vector<LocalPose>& out_pose) const;

private:
    std::vector<BlendSample2D> m_samples;
};

} // namespace nf::animation
