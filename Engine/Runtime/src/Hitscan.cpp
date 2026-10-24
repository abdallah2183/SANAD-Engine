#include <NF/Runtime/Hitscan.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace nf::runtime {

bool ray_box(const Vec3& origin, const Vec3& direction, const physics::Aabb& box,
             f32& out_distance, Vec3& out_normal) {
    // Slab method. Each axis contributes an interval of t for which the ray is
    // between the two planes; the entry is the largest of the three "near"
    // values and the exit is the smallest of the "far" ones. A ray that misses
    // has entry > exit.
    f32 t_near = -std::numeric_limits<f32>::infinity();
    f32 t_far = std::numeric_limits<f32>::infinity();
    int near_axis = -1;

    const f32 o[3] = {origin.x, origin.y, origin.z};
    const f32 d[3] = {direction.x, direction.y, direction.z};
    const f32 lo[3] = {box.min.x, box.min.y, box.min.z};
    const f32 hi[3] = {box.max.x, box.max.y, box.max.z};

    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(d[axis]) < 1e-8f) {
            // Parallel to this pair of planes: either the ray is between them
            // for its whole length, or it can never be.
            if (o[axis] < lo[axis] || o[axis] > hi[axis]) {
                return false;
            }
            continue;
        }
        const f32 inv = 1.0f / d[axis];
        f32 t0 = (lo[axis] - o[axis]) * inv;
        f32 t1 = (hi[axis] - o[axis]) * inv;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        if (t0 > t_near) {
            t_near = t0;
            near_axis = axis;
        }
        t_far = std::min(t_far, t1);
        if (t_near > t_far) {
            return false;
        }
    }

    // Behind the origin, or beyond the caller's reach.
    if (t_far < 0.0f) {
        return false;
    }

    if (t_near < 0.0f) {
        // The origin is INSIDE the box. A bullet fired from inside a wall hits
        // it at zero distance; there is no face to name, so the normal points
        // back along the ray, which is the direction a decal would face.
        out_distance = 0.0f;
        out_normal = -direction;
        return true;
    }

    out_distance = t_near;
    out_normal = Vec3{0.0f, 0.0f, 0.0f};
    if (near_axis == 0) {
        out_normal.x = d[0] > 0.0f ? -1.0f : 1.0f;
    } else if (near_axis == 1) {
        out_normal.y = d[1] > 0.0f ? -1.0f : 1.0f;
    } else if (near_axis == 2) {
        out_normal.z = d[2] > 0.0f ? -1.0f : 1.0f;
    } else {
        // Degenerate box (zero extent on every axis) and the ray started on
        // it: keep the function total rather than returning an uninitialised
        // normal.
        out_normal = -direction;
    }
    return true;
}

HitscanHit hitscan_nearest(const std::vector<HitscanTarget>& targets, Vec3 origin,
                           Vec3 direction, f32 max_distance, ecs::Entity ignore) {
    HitscanHit best;
    if (!(max_distance > 0.0f) || !std::isfinite(max_distance)) {
        return best;
    }
    const f32 length = direction.length();
    if (!(length > 1e-8f) || !std::isfinite(length)) {
        return best;
    }
    const Vec3 dir = direction / length;

    for (const HitscanTarget& target : targets) {
        if (target.entity.id == ignore.id && target.entity.generation == ignore.generation) {
            continue;
        }
        f32 distance = 0.0f;
        Vec3 normal{0.0f, 0.0f, 0.0f};
        if (!ray_box(origin, dir, target.bounds, distance, normal)) {
            continue;
        }
        if (distance > max_distance) {
            continue;
        }
        const bool closer = !best.hit || distance < best.distance;
        const bool tie = best.hit && distance == best.distance;
        // A tie is not hypothetical: two boxes that share a face are hit at the
        // same t, and "whichever the vector happened to hold first" would make
        // the result depend on how the scene was walked. The lower id wins, on
        // every run.
        if (closer || (tie && target.entity.id < best.entity.id)) {
            best.hit = true;
            best.entity = target.entity;
            best.distance = distance;
            best.position = origin + dir * distance;
            best.normal = normal;
        }
    }
    return best;
}

} // namespace nf::runtime
