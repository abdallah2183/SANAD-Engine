#include <NF/Physics/Shapes.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace nf::physics {

namespace {

// A plane is unbounded; the AABB is capped rather than infinite so callers can
// do arithmetic on it without producing NaN. Anything that reaches this bound is
// already past the point where the broadphase should have special-cased planes.
constexpr f32 kUnboundedExtent = 1.0e9f;

} // namespace

Shape Shape::make_sphere(f32 radius) {
    Shape s;
    s.type = ShapeType::Sphere;
    // A zero or negative radius would make the inertia tensor singular and the
    // support function divide by zero. Clamping here means every downstream
    // consumer can assume a positive radius.
    s.sphere.radius = (radius > 1e-4f) ? radius : 1e-4f;
    return s;
}

Shape Shape::make_box(const Vec3& half_extents) {
    Shape s;
    s.type = ShapeType::Box;
    s.box.half_extents = half_extents.abs().max(Vec3(1e-4f));
    return s;
}

Shape Shape::make_plane(const Vec3& normal) {
    Shape s;
    s.type = ShapeType::Plane;
    s.plane.normal = normal.normalized();
    // A degenerate normal would make the plane's collision direction arbitrary.
    if (s.plane.normal.length_sq() < 0.5f) {
        s.plane.normal = Vec3::up;
    }
    return s;
}

f32 Shape::volume() const {
    switch (type) {
        case ShapeType::Sphere: {
            const f32 r = sphere.radius;
            return (4.0f / 3.0f) * PI * r * r * r;
        }
        case ShapeType::Box: {
            const Vec3 h = box.half_extents;
            return 8.0f * h.x * h.y * h.z;
        }
        case ShapeType::Plane:
        default:
            return 0.0f;
    }
}

Vec3 Shape::inertia_diagonal(f32 mass) const {
    switch (type) {
        case ShapeType::Sphere: {
            // Solid sphere: I = 2/5 m r², the same about every axis.
            const f32 r = sphere.radius;
            const f32 i = 0.4f * mass * r * r;
            return {i, i, i};
        }
        case ShapeType::Box: {
            // Box with half-extents h: I_xx = m/3 (h_y² + h_z²).
            const Vec3 h = box.half_extents;
            const f32 k = mass / 3.0f;
            return {
                k * (h.y * h.y + h.z * h.z),
                k * (h.x * h.x + h.z * h.z),
                k * (h.x * h.x + h.y * h.y),
            };
        }
        case ShapeType::Plane:
        default:
            return Vec3::zero;
    }
}

void Shape::compute_aabb(const Vec3& position, const Quat& orientation,
                         Vec3& out_min, Vec3& out_max) const {
    switch (type) {
        case ShapeType::Sphere: {
            const Vec3 r(sphere.radius);
            out_min = position - r;
            out_max = position + r;
            return;
        }
        case ShapeType::Box: {
            // Rotating the box and taking the AABB of the result is the same as
            // projecting the half-extents onto the world axes: the world-space
            // extent along axis i is |R_i . h| summed over the local axes.
            const Vec3 h = box.half_extents;
            const Vec3 ex = orientation.rotate(Vec3(h.x, 0.0f, 0.0f));
            const Vec3 ey = orientation.rotate(Vec3(0.0f, h.y, 0.0f));
            const Vec3 ez = orientation.rotate(Vec3(0.0f, 0.0f, h.z));
            const Vec3 extent = ex.abs() + ey.abs() + ez.abs();
            out_min = position - extent;
            out_max = position + extent;
            return;
        }
        case ShapeType::Plane:
        default: {
            const Vec3 r(kUnboundedExtent);
            out_min = position - r;
            out_max = position + r;
            return;
        }
    }
}

Vec3 Shape::support(const Vec3& position, const Quat& orientation, const Vec3& direction) const {
    switch (type) {
        case ShapeType::Sphere: {
            const Vec3 d = direction.normalized();
            return position + d * sphere.radius;
        }
        case ShapeType::Box: {
            // The box's support point is the corner whose local sign matches the
            // direction's sign on each axis.
            const Vec3 local_dir = orientation.conjugate().rotate(direction);
            const Vec3 h = box.half_extents;
            const Vec3 local{
                local_dir.x >= 0.0f ? h.x : -h.x,
                local_dir.y >= 0.0f ? h.y : -h.y,
                local_dir.z >= 0.0f ? h.z : -h.z,
            };
            return position + orientation.rotate(local);
        }
        case ShapeType::Plane:
        default:
            return position;
    }
}

f32 Shape::bounding_radius() const {
    switch (type) {
        case ShapeType::Sphere:
            return sphere.radius;
        case ShapeType::Box:
            return box.half_extents.length();
        case ShapeType::Plane:
        default:
            return kUnboundedExtent;
    }
}

} // namespace nf::physics
