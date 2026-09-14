#include <NF/Animation/AnimationClip.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace nf::animation {

Keyframe AnimationClip::sample_track(const AnimationTrack& track, f32 time) const {
    if (track.keyframes.empty()) {
        return Keyframe{};
    }
    if (track.keyframes.size() == 1) {
        return track.keyframes[0];
    }

    // Clamp time to the keyframe range.
    if (time <= track.keyframes.front().time) {
        return track.keyframes.front();
    }
    if (time >= track.keyframes.back().time) {
        return track.keyframes.back();
    }

    // Binary search for the bracketing pair.
    usize lo = 0;
    usize hi = track.keyframes.size() - 1;
    while (lo + 1 < hi) {
        usize mid = (lo + hi) / 2;
        if (track.keyframes[mid].time <= time) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    const Keyframe& k0 = track.keyframes[lo];
    const Keyframe& k1 = track.keyframes[hi];
    f32 range = k1.time - k0.time;
    f32 t = (range > 1e-8f) ? (time - k0.time) / range : 0.0f;

    Keyframe result;
    result.time = time;
    result.translation = {
        k0.translation.x + (k1.translation.x - k0.translation.x) * t,
        k0.translation.y + (k1.translation.y - k0.translation.y) * t,
        k0.translation.z + (k1.translation.z - k0.translation.z) * t,
    };
    result.rotation = Quat::slerp(k0.rotation, k1.rotation, t);
    result.scale = {
        k0.scale.x + (k1.scale.x - k0.scale.x) * t,
        k0.scale.y + (k1.scale.y - k0.scale.y) * t,
        k0.scale.z + (k1.scale.z - k0.scale.z) * t,
    };
    return result;
}

void AnimationClip::sample(f32 time, const Skeleton& skel, std::vector<LocalPose>& out_local) const {
    const usize n = skel.bones.size();
    out_local.resize(n);

    // Initialize with rest pose.
    for (usize i = 0; i < n; ++i) {
        out_local[i].translation = skel.bones[i].rest_translation;
        out_local[i].rotation = skel.bones[i].rest_rotation;
        out_local[i].scale = skel.bones[i].rest_scale;
    }

    // Clamp time.
    if (duration > 0.0f) {
        if (looping) {
            time = std::fmod(time, duration);
            if (time < 0.0f) time += duration;
        } else {
            if (time < 0.0f) time = 0.0f;
            if (time > duration) time = duration;
        }
    }

    // Apply tracks.
    for (const auto& track : tracks) {
        if (track.bone_index < 0 || static_cast<usize>(track.bone_index) >= n) {
            continue;
        }
        Keyframe kf = sample_track(track, time);
        auto& lp = out_local[static_cast<usize>(track.bone_index)];
        lp.translation = kf.translation;
        lp.rotation = kf.rotation;
        lp.scale = kf.scale;
    }
}

void blend_poses(const std::vector<const std::vector<LocalPose>*>& poses,
                 const std::vector<f32>& weights,
                 std::vector<LocalPose>& out) {
    if (poses.empty() || poses[0] == nullptr) return;

    const usize n = poses[0]->size();
    out.resize(n);

    // Normalize weights.
    f32 total = 0.0f;
    for (f32 w : weights) total += w;
    if (total <= 1e-8f) {
        // Degenerate: just copy the first pose.
        out = *poses[0];
        return;
    }

    f32 inv_total = 1.0f / total;

    for (usize i = 0; i < n; ++i) {
        Vec3 translation = {0, 0, 0};
        Quat rotation = Quat::identity();
        Vec3 scale = {0, 0, 0};
        f32 first_weight = 1.0f;

        for (usize p = 0; p < poses.size(); ++p) {
            if (poses[p] == nullptr) continue;
            const auto& pose = *poses[p];
            if (i >= pose.size()) continue;

            f32 w = weights[p] * inv_total;
            if (w <= 0.0f) continue;

            translation = translation + pose[i].translation * w;
            scale = scale + pose[i].scale * w;

            // For rotation, accumulate weighted and normalize at the end.
            if (p == 0) {
                rotation = pose[i].rotation;
                first_weight = w;
            } else {
                // Slerp toward the accumulated rotation.
                rotation = Quat::slerp(rotation, pose[i].rotation, w / (first_weight + w));
                first_weight += w;
            }
        }
        out[i].translation = translation;
        out[i].rotation = rotation.normalized();
        out[i].scale = scale;
    }
}

void blend_additive(const std::vector<LocalPose>& base,
                    const std::vector<LocalPose>& additive,
                    const std::vector<LocalPose>& additive_base,
                    f32 weight,
                    std::vector<LocalPose>& out) {
    usize n = base.size();
    out.resize(n);

    for (usize i = 0; i < n; ++i) {
        const auto& b = base[i];
        const auto& a = (i < additive.size()) ? additive[i] : LocalPose{};
        const auto& ab = (i < additive_base.size()) ? additive_base[i] : LocalPose{};

        out[i].translation = b.translation + (a.translation - ab.translation) * weight;
        out[i].rotation = Quat::slerp(b.rotation, b.rotation * (ab.rotation.inverse() * a.rotation), weight);
        out[i].scale = b.scale + (a.scale - ab.scale) * weight;
    }
}

AnimationClip make_procedural_clip(const std::string& name, const Skeleton& skel,
                                   const ProceduralClipSpec& spec) {
    AnimationClip clip;
    clip.name = name;
    clip.looping = true;

    if (skel.bones.empty()) {
        // No rig, no motion. Leaving duration at 0 makes the empty result
        // obvious to a caller rather than looking like a clip that plays.
        return clip;
    }

    clip.duration = (spec.duration > 1e-4f) ? spec.duration : 1e-4f;

    u32 count = spec.keyframes;
    if (count < 2) count = 2;
    if (count > 256) count = 256;

    const Bone& root = skel.bones[0];

    // A zero-length axis would make from_axis_angle produce garbage; fall back
    // to the Y axis, which is the only sensible default for an upright rig.
    Vec3 axis = spec.axis;
    const f32 axis_len = axis.length();
    axis = (axis_len < 1e-6f) ? Vec3{0.0f, 1.0f, 0.0f} : axis * (1.0f / axis_len);

    AnimationTrack track;
    track.bone_index = 0;
    track.keyframes.reserve(count + 1);

    // count + 1 keyframes: the last one closes the loop, so sampling at
    // t == duration lands on an explicit keyframe instead of extrapolating.
    for (u32 i = 0; i <= count; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(count);

        Keyframe kf;
        kf.time = t * clip.duration;
        kf.translation = root.rest_translation;
        kf.rotation = root.rest_rotation;
        kf.scale = root.rest_scale;

        if (spec.kind == ProceduralClipSpec::Kind::Spin) {
            // Rest rotation first, then the delta: at t = 0 this reproduces the
            // rest pose exactly, which is what makes the clip safe to blend
            // against rest and to cross-fade from a rest-pose state.
            const f32 angle = t * spec.turns * TWO_PI;
            kf.rotation = root.rest_rotation * Quat::from_axis_angle(axis, angle);
        } else {
            // A full cosine cycle leaves rest at t = 0 and returns to it at
            // t = 1, so the loop has no positional discontinuity.
            const f32 offset = std::cos(t * TWO_PI) * spec.amplitude;
            kf.translation = root.rest_translation + axis * offset;
        }

        track.keyframes.push_back(kf);
    }

    clip.tracks.push_back(std::move(track));
    return clip;
}

} // namespace nf::animation
