// NF/Runtime/CameraFollow.cpp — the follow camera's maths.

#include <NF/Runtime/CameraFollow.hpp>

#include <algorithm>
#include <cmath>

namespace nf::runtime {

namespace {

constexpr f32 kDegToRad = 3.14159265358979323846f / 180.0f;

} // namespace

Vec3 follow_camera_direction(float yaw_deg, float pitch_deg) {
    const f32 yaw = yaw_deg * kDegToRad;
    const f32 pitch = pitch_deg * kDegToRad;
    // The camera sits BEHIND the pivot and looks at it, so the direction from
    // the pivot to the camera is the negation of the view direction.
    const f32 cp = std::cos(pitch);
    return Vec3{std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp}.normalized();
}

Vec3 follow_camera_position(const Vec3& pivot, float yaw_deg, float pitch_deg, float distance) {
    const f32 d = std::max(0.0f, distance);
    return pivot + follow_camera_direction(yaw_deg, pitch_deg) * d;
}

float follow_camera_blend(float smoothing, float dt) {
    if (smoothing <= 0.0f || dt <= 0.0f) {
        // No smoothing, or no time passed: the camera is exactly where the
        // geometry says.
        return smoothing <= 0.0f ? 1.0f : 0.0f;
    }
    // 1 - e^(-speed * dt): the fraction of the remaining gap covered in dt
    // seconds. Exponential, so the fraction covered per SECOND is the same
    // whatever the frame rate.
    const f32 factor = 1.0f - std::exp(-smoothing * dt);
    return std::clamp(factor, 0.0f, 1.0f);
}

Vec3 follow_camera_avoid(const Vec3& pivot, const Vec3& desired, float hit_distance,
                         float radius, float min_distance) {
    if (hit_distance < 0.0f) {
        return desired; // nothing in the way
    }
    const Vec3 delta = desired - pivot;
    const f32 length = delta.length();
    if (length <= 1e-6f) {
        return desired;
    }
    // Stop short of whatever was hit, by the camera's own radius, and never
    // closer to the pivot than the minimum: a camera inside the character's
    // head is worse than one that grazes a wall.
    f32 allowed = hit_distance - std::max(0.0f, radius);
    allowed = std::max(allowed, std::max(0.0f, min_distance));
    allowed = std::min(allowed, length);
    return pivot + (delta / length) * allowed;
}

} // namespace nf::runtime
