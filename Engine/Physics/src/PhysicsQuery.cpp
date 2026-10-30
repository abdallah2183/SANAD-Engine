// NF/Physics/PhysicsQuery.cpp — Spatial Raycasting, Sweeping & Overlap Queries

#include <NF/Physics/PhysicsQuery.hpp>
#include <algorithm>
#include <cmath>

namespace nf::physics {

// --- Analytic Ray/Sphere Intersection ---
bool PhysicsQuery::intersect_ray_sphere(
    const Ray3D& ray,
    const Vec3& sphere_pos,
    f32 radius,
    f32& out_t,
    Vec3& out_normal
) {
    const Vec3 m = ray.origin - sphere_pos;
    const f32 b = m.dot(ray.direction);
    const f32 c = m.dot(m) - radius * radius;

    // If ray origin is outside sphere and pointing away, early return
    if (c > 0.0f && b > 0.0f) {
        return false;
    }

    const f32 discr = b * b - c;
    if (discr < 0.0f) {
        return false;
    }

    f32 t = -b - std::sqrt(discr);
    if (t < 0.0f) {
        t = -b + std::sqrt(discr); // Ray started inside sphere
    }

    if (t < 0.0f || t > ray.max_distance) {
        return false;
    }

    out_t = t;
    const Vec3 hit_point = ray.point_at(t);
    const Vec3 n = hit_point - sphere_pos;
    out_normal = (n.length_sq() > 1e-8f) ? n.normalized() : -ray.direction;
    return true;
}

// --- Analytic Ray/Box (OBB) Intersection ---
bool PhysicsQuery::intersect_ray_box(
    const Ray3D& ray,
    const Vec3& box_pos,
    const Quat& box_orient,
    const Vec3& half_extents,
    f32& out_t,
    Vec3& out_normal
) {
    // Transform ray to box local coordinates
    const Quat inv_rot = box_orient.conjugate();
    const Vec3 local_origin = inv_rot.rotate(ray.origin - box_pos);
    const Vec3 local_dir = inv_rot.rotate(ray.direction);

    f32 t_min = 0.0f;
    f32 t_max = ray.max_distance;
    Vec3 local_hit_normal = Vec3::up;

    const f32 orig[3] = { local_origin.x, local_origin.y, local_origin.z };
    const f32 dir[3]  = { local_dir.x, local_dir.y, local_dir.z };
    const f32 ext[3]  = { half_extents.x, half_extents.y, half_extents.z };

    for (int i = 0; i < 3; ++i) {
        if (std::abs(dir[i]) < 1e-8f) {
            // Ray is parallel to slab. No hit if origin not within slab
            if (orig[i] < -ext[i] || orig[i] > ext[i]) {
                return false;
            }
        } else {
            const f32 inv_d = 1.0f / dir[i];
            f32 t1 = (-ext[i] - orig[i]) * inv_d;
            f32 t2 = ( ext[i] - orig[i]) * inv_d;
            f32 sign = -1.0f;

            if (t1 > t2) {
                std::swap(t1, t2);
                sign = 1.0f;
            }

            if (t1 > t_min) {
                t_min = t1;
                local_hit_normal = Vec3::zero;
                if (i == 0) local_hit_normal.x = sign;
                else if (i == 1) local_hit_normal.y = sign;
                else local_hit_normal.z = sign;
            }

            t_max = std::min(t_max, t2);
            if (t_min > t_max) {
                return false;
            }
        }
    }

    if (t_min < 0.0f || t_min > ray.max_distance) {
        return false;
    }

    out_t = t_min;
    out_normal = box_orient.rotate(local_hit_normal).normalized();
    return true;
}

// --- Analytic Ray/Plane Intersection ---
bool PhysicsQuery::intersect_ray_plane(
    const Ray3D& ray,
    const Vec3& plane_pos,
    const Vec3& plane_normal,
    f32& out_t,
    Vec3& out_normal
) {
    const f32 denom = ray.direction.dot(plane_normal);
    if (std::abs(denom) < 1e-7f) {
        return false; // Ray is parallel to plane
    }

    const f32 t = (plane_pos - ray.origin).dot(plane_normal) / denom;
    if (t < 0.0f || t > ray.max_distance) {
        return false;
    }

    out_t = t;
    out_normal = (denom < 0.0f) ? plane_normal : -plane_normal;
    return true;
}

// --- Raycast Single ---
std::optional<RaycastHit> PhysicsQuery::raycast(
    const PhysicsWorld& world,
    const Ray3D& ray,
    u32 layer_mask
) {
    (void)layer_mask;
    std::optional<RaycastHit> best_hit = std::nullopt;
    f32 closest_dist = ray.max_distance;

    world.for_each_body([&](BodyHandle handle, const Shape& shape, const BodyState& state) {
        f32 t = 0.0f;
        Vec3 normal = Vec3::up;
        bool hit = false;

        switch (shape.type) {
            case ShapeType::Sphere:
                hit = intersect_ray_sphere(ray, state.position, shape.sphere.radius, t, normal);
                break;
            case ShapeType::Box:
                hit = intersect_ray_box(ray, state.position, state.orientation, shape.box.half_extents, t, normal);
                break;
            case ShapeType::Plane:
                hit = intersect_ray_plane(ray, state.position, shape.plane.normal, t, normal);
                break;
        }

        if (hit && t < closest_dist) {
            closest_dist = t;
            RaycastHit h;
            h.body = handle;
            h.point = ray.point_at(t);
            h.normal = normal;
            h.distance = t;
            h.fraction = (ray.max_distance > 1e-6f) ? (t / ray.max_distance) : 0.0f;
            h.has_hit = true;
            best_hit = h;
        }
    });

    return best_hit;
}

// --- Raycast All ---
std::vector<RaycastHit> PhysicsQuery::raycast_all(
    const PhysicsWorld& world,
    const Ray3D& ray,
    u32 layer_mask
) {
    (void)layer_mask;
    std::vector<RaycastHit> hits;

    world.for_each_body([&](BodyHandle handle, const Shape& shape, const BodyState& state) {
        f32 t = 0.0f;
        Vec3 normal = Vec3::up;
        bool hit = false;

        switch (shape.type) {
            case ShapeType::Sphere:
                hit = intersect_ray_sphere(ray, state.position, shape.sphere.radius, t, normal);
                break;
            case ShapeType::Box:
                hit = intersect_ray_box(ray, state.position, state.orientation, shape.box.half_extents, t, normal);
                break;
            case ShapeType::Plane:
                hit = intersect_ray_plane(ray, state.position, shape.plane.normal, t, normal);
                break;
        }

        if (hit && t <= ray.max_distance) {
            RaycastHit h;
            h.body = handle;
            h.point = ray.point_at(t);
            h.normal = normal;
            h.distance = t;
            h.fraction = (ray.max_distance > 1e-6f) ? (t / ray.max_distance) : 0.0f;
            h.has_hit = true;
            hits.push_back(h);
        }
    });

    std::sort(hits.begin(), hits.end(), [](const RaycastHit& a, const RaycastHit& b) {
        return a.distance < b.distance;
    });

    return hits;
}

// --- Sweep Sphere ---
std::optional<SweepHit> PhysicsQuery::sweep_sphere(
    const PhysicsWorld& world,
    const Vec3& start,
    const Vec3& end,
    f32 radius,
    u32 layer_mask
) {
    (void)layer_mask;
    const Vec3 delta = end - start;
    const f32 travel_dist = delta.length();
    if (travel_dist < 1e-6f) {
        return std::nullopt;
    }

    const Ray3D ray{ start, delta / travel_dist, travel_dist };
    std::optional<SweepHit> best_sweep = std::nullopt;
    f32 closest_t = travel_dist;

    world.for_each_body([&](BodyHandle handle, const Shape& shape, const BodyState& state) {
        f32 t = 0.0f;
        Vec3 normal = Vec3::up;
        bool hit = false;

        if (shape.type == ShapeType::Sphere) {
            // Swept sphere against stationary sphere: expand stationary sphere by swept radius
            hit = intersect_ray_sphere(ray, state.position, shape.sphere.radius + radius, t, normal);
        } else if (shape.type == ShapeType::Box) {
            // Swept sphere against box: expand box by swept radius
            const Vec3 expanded_extents = shape.box.half_extents + Vec3(radius);
            hit = intersect_ray_box(ray, state.position, state.orientation, expanded_extents, t, normal);
        } else if (shape.type == ShapeType::Plane) {
            // Offset plane by radius towards the ray start
            const Vec3 offset_pos = state.position + shape.plane.normal * radius;
            hit = intersect_ray_plane(ray, offset_pos, shape.plane.normal, t, normal);
        }

        if (hit && t < closest_t) {
            closest_t = t;
            SweepHit sh;
            sh.body = handle;
            sh.distance = t;
            sh.fraction = t / travel_dist;
            sh.normal = normal;
            sh.point = ray.point_at(t) - normal * radius;
            sh.has_hit = true;
            best_sweep = sh;
        }
    });

    return best_sweep;
}

// --- Overlap Helpers ---
bool PhysicsQuery::check_overlap_sphere_shape(
    const Vec3& sphere_pos,
    f32 sphere_radius,
    const Shape& shape,
    const Vec3& shape_pos,
    const Quat& shape_orient
) {
    if (shape.type == ShapeType::Sphere) {
        const f32 r_sum = sphere_radius + shape.sphere.radius;
        return (sphere_pos - shape_pos).length_sq() <= (r_sum * r_sum);
    }
    if (shape.type == ShapeType::Box) {
        // Transform sphere center to box local space
        const Quat inv_rot = shape_orient.conjugate();
        const Vec3 local_c = inv_rot.rotate(sphere_pos - shape_pos);
        const Vec3 clamped{
            std::clamp(local_c.x, -shape.box.half_extents.x, shape.box.half_extents.x),
            std::clamp(local_c.y, -shape.box.half_extents.y, shape.box.half_extents.y),
            std::clamp(local_c.z, -shape.box.half_extents.z, shape.box.half_extents.z)
        };
        return (local_c - clamped).length_sq() <= (sphere_radius * sphere_radius);
    }
    if (shape.type == ShapeType::Plane) {
        const f32 dist = std::abs((sphere_pos - shape_pos).dot(shape.plane.normal));
        return dist <= sphere_radius;
    }
    return false;
}

bool PhysicsQuery::check_overlap_box_shape(
    const Vec3& box_pos,
    const Vec3& box_half_extents,
    const Quat& box_orient,
    const Shape& shape,
    const Vec3& shape_pos,
    const Quat& shape_orient
) {
    if (shape.type == ShapeType::Sphere) {
        return check_overlap_sphere_shape(shape_pos, shape.sphere.radius, Shape::make_box(box_half_extents), box_pos, box_orient);
    }
    if (shape.type == ShapeType::Box) {
        // Approximate OBB overlap via bounding radius & SAT center distance
        const f32 r1 = box_half_extents.length();
        const f32 r2 = shape.box.half_extents.length();
        if ((box_pos - shape_pos).length_sq() > (r1 + r2) * (r1 + r2)) {
            return false;
        }

        // Project onto Box A axes
        const Quat inv_a = box_orient.conjugate();
        const Vec3 local_diff = inv_a.rotate(shape_pos - box_pos);
        const Quat rel_rot = inv_a * shape_orient;
        const Vec3 b_ext = shape.box.half_extents;
        const Vec3 r2_proj{
            b_ext.x * std::abs(rel_rot.rotate(Vec3(1, 0, 0)).x) + b_ext.y * std::abs(rel_rot.rotate(Vec3(0, 1, 0)).x) + b_ext.z * std::abs(rel_rot.rotate(Vec3(0, 0, 1)).x),
            b_ext.x * std::abs(rel_rot.rotate(Vec3(1, 0, 0)).y) + b_ext.y * std::abs(rel_rot.rotate(Vec3(0, 1, 0)).y) + b_ext.z * std::abs(rel_rot.rotate(Vec3(0, 0, 1)).y),
            b_ext.x * std::abs(rel_rot.rotate(Vec3(1, 0, 0)).z) + b_ext.y * std::abs(rel_rot.rotate(Vec3(0, 1, 0)).z) + b_ext.z * std::abs(rel_rot.rotate(Vec3(0, 0, 1)).z)
        };

        if (std::abs(local_diff.x) > box_half_extents.x + r2_proj.x ||
            std::abs(local_diff.y) > box_half_extents.y + r2_proj.y ||
            std::abs(local_diff.z) > box_half_extents.z + r2_proj.z) {
            return false;
        }
        return true;
    }
    if (shape.type == ShapeType::Plane) {
        // Project box radius onto plane normal
        const Vec3 n = shape.plane.normal;
        const f32 proj = box_half_extents.x * std::abs(n.dot(box_orient.rotate(Vec3(1, 0, 0)))) +
                         box_half_extents.y * std::abs(n.dot(box_orient.rotate(Vec3(0, 1, 0)))) +
                         box_half_extents.z * std::abs(n.dot(box_orient.rotate(Vec3(0, 0, 1))));
        const f32 dist = std::abs((box_pos - shape_pos).dot(n));
        return dist <= proj;
    }
    return false;
}

// --- Overlap Sphere ---
std::vector<BodyHandle> PhysicsQuery::overlap_sphere(
    const PhysicsWorld& world,
    const Vec3& center,
    f32 radius,
    u32 layer_mask
) {
    (void)layer_mask;
    std::vector<BodyHandle> results;

    world.for_each_body([&](BodyHandle handle, const Shape& shape, const BodyState& state) {
        if (check_overlap_sphere_shape(center, radius, shape, state.position, state.orientation)) {
            results.push_back(handle);
        }
    });

    return results;
}

// --- Overlap Box ---
std::vector<BodyHandle> PhysicsQuery::overlap_box(
    const PhysicsWorld& world,
    const Vec3& center,
    const Vec3& half_extents,
    const Quat& orientation,
    u32 layer_mask
) {
    (void)layer_mask;
    std::vector<BodyHandle> results;

    world.for_each_body([&](BodyHandle handle, const Shape& shape, const BodyState& state) {
        if (check_overlap_box_shape(center, half_extents, orientation, shape, state.position, state.orientation)) {
            results.push_back(handle);
        }
    });

    return results;
}

} // namespace nf::physics
