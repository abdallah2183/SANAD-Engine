#pragma once

#include <NF/Animation/Skeleton.hpp>
#include <vector>
#include <cstdint>

namespace nf::animation {

/// Per-bone weighting mask for layered skeletal animation blending (e.g. Upper body aiming / Lower body running).
class BoneMask {
public:
    BoneMask() = default;
    explicit BoneMask(size_t bone_count, f32 default_weight = 0.0f);

    size_t bone_count() const { return m_weights.size(); }
    void resize(size_t bone_count, f32 default_weight = 0.0f);

    /// Sets weight for a single bone index [0.0, 1.0].
    void set_weight(int32_t bone_index, f32 weight);

    /// Gets weight for a bone index. Returns 0.0f if out of bounds.
    f32 get_weight(int32_t bone_index) const;

    /// Sets weight for root_bone and recursively for all its descendant bones in the skeleton.
    void set_weight_recursive(const Skeleton& skel, int32_t root_bone_index, f32 weight);

    /// Fills all bones with the given weight.
    void fill(f32 weight);

    /// Inverts all weights: w = 1.0f - w.
    void invert();

    const std::vector<f32>& weights() const { return m_weights; }

private:
    std::vector<f32> m_weights;
};

/// Blends an overlay pose onto a base pose using a per-bone weight mask.
/// All input containers must match the skeleton's bone count.
void blend_layered(const std::vector<LocalPose>& base,
                   const std::vector<LocalPose>& overlay,
                   const BoneMask& mask,
                   std::vector<LocalPose>& out_pose);

} // namespace nf::animation
