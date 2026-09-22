// NF/Animation/RootMotion.cpp — root motion extraction from clips.

#include <NF/Animation/RootMotion.hpp>

#include <cmath>

namespace nf::animation {

namespace {

const AnimationTrack* find_root_track(const AnimationClip& clip) {
    for (const auto& track : clip.tracks) {
        if (track.bone_index == 0) {
            return &track;
        }
    }
    return nullptr;
}

Keyframe root_key_at(const AnimationClip& clip, f32 time) {
    const AnimationTrack* track = find_root_track(clip);
    if (track == nullptr) {
        return Keyframe{};
    }
    // sample_track clamps to the keyframe range; clip-level loop wrapping is
    // handled by the caller (extract_root_motion), which knows both times.
    return clip.sample_track(*track, time);
}

} // namespace

RootMotionDelta extract_root_motion(const AnimationClip& clip, const Skeleton& /*skel*/,
                                    f32 from_time, f32 to_time) {
    RootMotionDelta out;

    // Spans the loop wrap: end-of-cycle segment composed with the
    // start-of-cycle segment, so a looping walk accumulates without a hitch.
    const bool wraps = clip.looping && clip.duration > 0.0f && from_time > to_time;
    if (wraps) {
        const Keyframe kf_from = root_key_at(clip, from_time);
        const Keyframe kf_end = root_key_at(clip, clip.duration);
        const Keyframe kf_start = root_key_at(clip, 0.0f);
        const Keyframe kf_to = root_key_at(clip, to_time);

        out.translation = (kf_end.translation - kf_from.translation) +
                          (kf_to.translation - kf_start.translation);
        out.rotation = (kf_from.rotation.inverse() * kf_end.rotation) *
                       (kf_start.rotation.inverse() * kf_to.rotation);
        return out;
    }

    const Keyframe kf_from = root_key_at(clip, from_time);
    const Keyframe kf_to = root_key_at(clip, to_time);

    out.translation = kf_to.translation - kf_from.translation;
    out.rotation = kf_from.rotation.inverse() * kf_to.rotation;
    return out;
}

} // namespace nf::animation
