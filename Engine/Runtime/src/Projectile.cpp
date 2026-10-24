#include <NF/Runtime/Projectile.hpp>

#include <algorithm>
#include <cmath>

namespace nf::runtime {

namespace {

inline bool finite_vec(const Vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline bool finite_positive(f32 v) noexcept { return std::isfinite(v) && v > 0.0f; }

} // namespace

bool projectile_body_valid(const ProjectileBody& body) noexcept {
    return finite_positive(body.lifetime_seconds) && finite_vec(body.position) &&
           finite_vec(body.velocity) && std::isfinite(body.age_seconds) &&
           std::isfinite(body.distance_travelled) && body.age_seconds >= 0.0f &&
           body.distance_travelled >= 0.0f;
}

ProjectileAdvance projectile_advance(ProjectileBody& body, const Vec3& gravity,
                                     f32 dt) noexcept {
    ProjectileAdvance out;
    out.from = body.position;
    out.to = body.position;
    if (!projectile_body_valid(body) || !(dt > 0.0f) || !std::isfinite(dt)) {
        return out;
    }

    const f32 remaining = body.lifetime_seconds - body.age_seconds;
    if (!(remaining > 0.0f)) {
        out.expired = true;
        return out;
    }

    // A frame longer than the remaining flight only integrates the part that
    // exists. The segment therefore ends exactly at maximum range instead of
    // overshooting it and reporting a hit beyond the weapon's reach.
    const f32 step = std::min(dt, remaining);
    const Vec3 acceleration =
        finite_vec(gravity) ? gravity : Vec3{0.0f, 0.0f, 0.0f};

    // Constant acceleration, integrated exactly. This is a parabola, not an
    // Euler approximation, so the landing point does not depend on frame rate.
    const Vec3 next = body.position + body.velocity * step + acceleration * (0.5f * step * step);
    const Vec3 next_velocity = body.velocity + acceleration * step;

    out.from = body.position;
    out.to = next;
    out.step_seconds = step;
    out.moved = step > 0.0f;
    body.position = next;
    body.velocity = next_velocity;
    body.age_seconds = std::min(body.lifetime_seconds, body.age_seconds + step);
    body.distance_travelled += (next - out.from).length();

    if (body.age_seconds >= body.lifetime_seconds - 1e-6f) {
        body.age_seconds = body.lifetime_seconds;
        out.expired = true;
    }
    return out;
}

f32 projectile_lifetime_for_range(f32 range, f32 speed) noexcept {
    if (!finite_positive(range) || !finite_positive(speed)) {
        return 0.0f;
    }
    return range / speed;
}

f32 projectile_drop_at(f32 distance, f32 speed, f32 gravity) noexcept {
    if (!(distance > 0.0f) || !finite_positive(speed) || !std::isfinite(gravity) ||
        gravity <= 0.0f) {
        return 0.0f;
    }
    const f32 time = distance / speed;
    return 0.5f * gravity * time * time;
}

ProjectileIntercept projectile_intercept(Vec3 shooter, Vec3 target, Vec3 target_velocity,
                                         f32 speed) noexcept {
    ProjectileIntercept out;
    out.point = target;
    if (!finite_vec(shooter) || !finite_vec(target) || !finite_vec(target_velocity) ||
        !finite_positive(speed)) {
        return out;
    }

    const Vec3 delta = target - shooter;
    const f32 c = delta.dot(delta);
    if (c <= 1e-8f) {
        // Already at the muzzle. A zero flight time is the honest answer.
        out.valid = true;
        out.time_seconds = 0.0f;
        return out;
    }

    // |delta + v*t|^2 = (speed*t)^2, expanded into a quadratic in t.
    const f32 a = target_velocity.dot(target_velocity) - speed * speed;
    const f32 b = 2.0f * delta.dot(target_velocity);

    f32 t = -1.0f;
    if (std::fabs(a) < 1e-6f) {
        // Target speed equals projectile speed: the equation is linear. A
        // zero b means the motion is perpendicular (or absent), so the
        // projectile never closes the gap.
        if (std::fabs(b) > 1e-9f) {
            t = -c / b;
        }
    } else {
        const f32 discriminant = b * b - 4.0f * a * c;
        if (discriminant >= 0.0f) {
            const f32 root = std::sqrt(discriminant);
            const f32 t0 = (-b - root) / (2.0f * a);
            const f32 t1 = (-b + root) / (2.0f * a);
            const f32 lo = std::min(t0, t1);
            const f32 hi = std::max(t0, t1);
            t = lo > 1e-5f ? lo : hi;
        }
    }

    if (!(t > 1e-5f) || !std::isfinite(t)) {
        return out; // out.point is already the current position
    }
    out.valid = true;
    out.time_seconds = t;
    out.point = target + target_velocity * t;
    return out;
}

} // namespace nf::runtime
