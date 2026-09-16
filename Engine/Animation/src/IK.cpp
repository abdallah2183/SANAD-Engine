// NF/Animation/IK.cpp — analytic two-bone IK.

#include <NF/Animation/IK.hpp>

#include <algorithm>
#include <cmath>

namespace nf::animation {

namespace {

// Shortest-arc rotation taking unit vector `from` onto unit vector `to`.
// Degenerate inputs (zero/opposite vectors) yield identity, never NaN.
Quat quat_from_unit_vectors(Vec3 from, Vec3 to) {
    const float d = std::clamp(from.dot(to), -1.0f, 1.0f);
    if (d > 1.0f - 1e-6f) return Quat::identity();
    if (d < -1.0f + 1e-6f) {
        // Opposite: rotate 180 degrees about any perpendicular axis.
        Vec3 axis = from.cross(Vec3{1.0f, 0.0f, 0.0f});
        if (axis.length_sq() < 1e-8f) axis = from.cross(Vec3{0.0f, 1.0f, 0.0f});
        return Quat::from_axis_angle(axis.normalized(), 3.14159265358979323846f);
    }
    const Vec3 axis = from.cross(to);
    const float s = std::sqrt((1.0f + d) * 2.0f);
    const float inv = 1.0f / s;
    Quat q{axis.x * inv, axis.y * inv, axis.z * inv, s * 0.5f};
    return q.normalized();
}

} // namespace

TwoBoneIKResult solve_two_bone_ik(Vec3 root_pos, Vec3 mid_pos, Vec3 end_pos, Vec3 target,
                                  Vec3 pole_hint) {
    TwoBoneIKResult out;
    const float l1 = (mid_pos - root_pos).length();
    const float l2 = (end_pos - mid_pos).length();
    if (l1 < 1e-9f || l2 < 1e-9f) {
        out.new_mid = mid_pos;
        out.new_end = end_pos;
        return out; // degenerate chain: hold still
    }

    Vec3 to_target = target - root_pos;
    float dist = to_target.length();
    const float max_reach = l1 + l2;
    const float min_reach = std::abs(l1 - l2) + 1e-6f;
    if (dist < 1e-9f) {
        out.new_mid = mid_pos;
        out.new_end = end_pos;
        return out; // target on the root: hold still
    }
    Vec3 dir = to_target / dist;
    if (dist > max_reach) {
        dist = max_reach;
        out.clamped = true;
    } else if (dist < min_reach) {
        dist = min_reach;
        out.clamped = true;
    }
    const Vec3 aim = root_pos + dir * dist;

    // Bend plane: pole hint projected off the aim axis.
    Vec3 pole = pole_hint - root_pos;
    pole = pole - dir * pole.dot(dir);
    if (pole.length_sq() < 1e-10f) {
        // Hint on the axis (or missing): reuse the current bend direction so
        // the elbow keeps bending the way it already was.
        pole = mid_pos - root_pos;
        pole = pole - dir * pole.dot(dir);
        if (pole.length_sq() < 1e-10f) pole = Vec3{0.0f, 1.0f, 0.0f} - dir * dir.y;
    }
    const Vec3 bend = pole.normalized();
    // Law of cosines: angle between aim axis and the upper bone.
    const float cos_a =
        std::clamp((l1 * l1 + dist * dist - l2 * l2) / (2.0f * l1 * dist), -1.0f, 1.0f);
    const float sin_a = std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));
    const Vec3 upper_dir = dir * cos_a + bend * sin_a;
    out.new_mid = root_pos + upper_dir * l1;
    Vec3 lower_dir = aim - out.new_mid;
    const float lower_len = lower_dir.length();
    if (lower_len < 1e-9f) {
        lower_dir = dir;
    } else {
        lower_dir = lower_dir / lower_len;
    }
    out.new_end = out.new_mid + lower_dir * l2;

    const Vec3 cur_upper = (mid_pos - root_pos) / l1;
    const Vec3 cur_lower = (end_pos - mid_pos) / l2;
    out.upper_delta = quat_from_unit_vectors(cur_upper, upper_dir.normalized());
    // Hierarchical composition: the lower bone rides on the rotated upper
    // bone, so its delta maps the ALREADY-rotated lower direction onto the
    // solved one. Then new_end == new_mid + lower*(upper*(end-mid)) exactly.
    out.lower_delta =
        quat_from_unit_vectors(out.upper_delta.rotate(cur_lower), lower_dir);
    return out;
}

} // namespace nf::animation
