#pragma once

#include <NF/Animation/Skeleton.hpp>
#include <cstdint>
#include <vector>

namespace nf::animation {

/// A single keyframe for one bone. Stores the timestamp (in seconds, from
/// the start of the clip) and the local transform at that time.
struct Keyframe {
    f32 time = 0.0f;
    Vec3 translation;
    Quat rotation = Quat::identity();
    Vec3 scale = {1.0f, 1.0f, 1.0f};
};

/// A track is the animation data for a single bone: an ordered array of
/// keyframes. Keyframes must be sorted by ascending `time`.
struct AnimationTrack {
    int32_t bone_index = 0;
    std::vector<Keyframe> keyframes;
};

/// An animation clip: a collection of tracks plus a duration. The duration
/// may be longer than the last keyframe's time (for clips with trailing
/// silence). Sampling is pure — no mutable state.
struct AnimationClip {
    std::string name;
    std::vector<AnimationTrack> tracks;
    f32 duration = 0.0f;
    bool looping = true;

    /// Sample the clip at `time` into `out_local`. Bones without a track
    /// keep their rest-pose from the skeleton. `time` is clamped to [0, duration].
    void sample(f32 time, const Skeleton& skel, std::vector<LocalPose>& out_local) const;

    /// Sample a single track at `time`, returning the interpolated keyframe.
    /// If the track has no keyframes, returns an identity keyframe.
    Keyframe sample_track(const AnimationTrack& track, f32 time) const;
};

/// N-way blend: blends multiple local poses with the given weights.
/// Weights are normalized internally. `poses` and `weights` must have the
/// same count, and each pose must have the same number of bones.
void blend_poses(const std::vector<const std::vector<LocalPose>*>& poses,
                 const std::vector<f32>& weights,
                 std::vector<LocalPose>& out);

/// Additive blend: `base + (additive - additive_base) * weight`.
/// All three poses must have the same number of bones.
void blend_additive(const std::vector<LocalPose>& base,
                    const std::vector<LocalPose>& additive,
                    const std::vector<LocalPose>& additive_base,
                    f32 weight,
                    std::vector<LocalPose>& out);

/// A declarative description of a procedurally generated clip.
///
/// This exists because the animation import pipeline is an explicit Phase 9
/// non-goal (see `Docs/Phase9_Plan.md` §7), but a scene still has to be able to
/// carry real, visible motion. Without it the loader has no way to fill a clip
/// and every `Animation:` line in a scene silently samples the rest pose.
///
/// Deliberately tiny and deterministic: the same spec always produces the same
/// keyframes, so animation stays reproducible across runs and machines.
struct ProceduralClipSpec {
    enum class Kind {
        Spin, ///< Rotate the root bone about `axis`.
        Bob,  ///< Translate the root bone along `axis`, ±`amplitude`.
    };

    Kind kind = Kind::Spin;
    Vec3 axis = {0.0f, 1.0f, 0.0f};
    f32 turns = 1.0f;     ///< Spin: full revolutions over `duration`.
    f32 amplitude = 0.5f; ///< Bob: metres either side of the rest position.
    f32 duration = 2.0f;
    u32 keyframes = 16;
};

/// Build a clip that drives bone 0 (the root) from `spec`. Bones with no track
/// keep their rest pose when sampled, so a one-bone skeleton is enough.
///
/// The first keyframe reproduces the rest pose exactly and the last one equals
/// it, so the clip is safe to cross-fade against rest and loops without a hitch.
/// `keyframes` is clamped to [2, 256]: below two there is nothing to
/// interpolate between, and an unbounded count would let a scene file allocate
/// without limit.
AnimationClip make_procedural_clip(const std::string& name, const Skeleton& skel,
                                   const ProceduralClipSpec& spec);

} // namespace nf::animation
