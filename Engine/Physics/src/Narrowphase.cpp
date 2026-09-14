#include <NF/Physics/Narrowphase.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace nf::physics {

namespace {

constexpr f32 kAxisEpsilon = 1e-8f;

// World-space axes of an oriented box.
void box_axes(const Quat& q, Vec3 out[3]) {
    out[0] = q.rotate(Vec3(1.0f, 0.0f, 0.0f));
    out[1] = q.rotate(Vec3(0.0f, 1.0f, 0.0f));
    out[2] = q.rotate(Vec3(0.0f, 0.0f, 1.0f));
}

f32 component(const Vec3& v, int i) {
    return (i == 0) ? v.x : (i == 1) ? v.y : v.z;
}

/// How far the box extends along `n`: the sum of the half-extents projected onto
/// the axis. This is the SAT projection of a box, and it is what makes the test
/// independent of the box's orientation.
f32 project_radius(const Vec3& half_extents, const Vec3 axes[3], const Vec3& n) {
    return half_extents.x * std::fabs(n.dot(axes[0])) +
           half_extents.y * std::fabs(n.dot(axes[1])) +
           half_extents.z * std::fabs(n.dot(axes[2]));
}

/// Sutherland-Hodgman clip of a convex polygon against the half-space
/// `dot(n, p) <= d`. Returns the new vertex count.
int clip_polygon(const Vec3* in, int count, const Vec3& n, f32 d, Vec3* out) {
    int written = 0;
    for (int i = 0; i < count; ++i) {
        const Vec3& cur = in[i];
        const Vec3& nxt = in[(i + 1) % count];
        const f32 dc = n.dot(cur) - d;
        const f32 dn = n.dot(nxt) - d;

        if (dc <= 0.0f) {
            out[written++] = cur;
        }
        // Crossing the plane: insert the intersection so the clipped polygon
        // keeps its shape instead of collapsing to the surviving vertices.
        if ((dc < 0.0f && dn > 0.0f) || (dc > 0.0f && dn < 0.0f)) {
            const f32 t = dc / (dc - dn);
            out[written++] = cur + (nxt - cur) * t;
        }
    }
    return written;
}

/// The four corners of the face of a box whose outward normal is `face_axis`,
/// in winding order around the face.
void face_corners(const Vec3& pos, const Vec3 axes[3], const Vec3& half_extents,
                  int face_axis, f32 sign, Vec3 out[4]) {
    const int j = (face_axis + 1) % 3;
    const int k = (face_axis + 2) % 3;
    const Vec3 n = axes[face_axis] * (sign * component(half_extents, face_axis));
    const Vec3 u = axes[j] * component(half_extents, j);
    const Vec3 v = axes[k] * component(half_extents, k);
    const Vec3 c = pos + n;
    out[0] = c - u - v;
    out[1] = c + u - v;
    out[2] = c + u + v;
    out[3] = c - u + v;
}

/// Centre of the box edge running along `edge_axis`, chosen on the side that
/// faces `toward`.
Vec3 edge_centre(const Vec3& pos, const Vec3 axes[3], const Vec3& half_extents,
                 int edge_axis, const Vec3& toward) {
    const int j = (edge_axis + 1) % 3;
    const int k = (edge_axis + 2) % 3;
    const f32 sj = (toward.dot(axes[j]) >= 0.0f) ? 1.0f : -1.0f;
    const f32 sk = (toward.dot(axes[k]) >= 0.0f) ? 1.0f : -1.0f;
    return pos + axes[j] * (sj * component(half_extents, j)) +
           axes[k] * (sk * component(half_extents, k));
}

/// Closest points between two segments. Ericson, Real-Time Collision Detection
/// §5.1.9 — the standard clamped-parameter solution.
void closest_points_segments(const Vec3& p1, const Vec3& d1, f32 l1,
                             const Vec3& p2, const Vec3& d2, f32 l2,
                             Vec3& out1, Vec3& out2) {
    const Vec3 r = p1 - p2;
    const f32 a = d1.dot(d1); // 1 (unit axes)
    const f32 e = d2.dot(d2); // 1
    const f32 f = d2.dot(r);

    f32 s = 0.0f;
    f32 t = 0.0f;

    if (a <= kAxisEpsilon && e <= kAxisEpsilon) {
        out1 = p1;
        out2 = p2;
        return;
    }
    if (a <= kAxisEpsilon) {
        t = std::clamp(f / e, -l2, l2);
    } else {
        const f32 c = d1.dot(r);
        if (e <= kAxisEpsilon) {
            s = std::clamp(-c / a, -l1, l1);
        } else {
            const f32 b = d1.dot(d2);
            const f32 denom = a * e - b * b;
            if (denom > kAxisEpsilon) {
                s = std::clamp((b * f - c * e) / denom, -l1, l1);
            } else {
                s = 0.0f; // parallel
            }
            t = (b * s + f) / e;
            if (t < -l2) {
                t = -l2;
                s = std::clamp((b * t - c) / a, -l1, l1);
            } else if (t > l2) {
                t = l2;
                s = std::clamp((b * t - c) / a, -l1, l1);
            }
        }
    }

    out1 = p1 + d1 * s;
    out2 = p2 + d2 * t;
}

} // namespace

// --- Manifold ---------------------------------------------------------------

void Manifold::add_point(const Vec3& position, f32 penetration, u32 feature_id) {
    if (point_count < kMaxPoints) {
        points[point_count++] = ContactPoint{position, penetration, 0.0f, {0.0f, 0.0f}, feature_id};
        return;
    }
    // Full: evict the shallowest. A box resting on a face needs its four deepest
    // corners; keeping the first four encountered makes it tip.
    u32 shallowest = 0;
    for (u32 i = 1; i < point_count; ++i) {
        if (points[i].penetration < points[shallowest].penetration) {
            shallowest = i;
        }
    }
    if (penetration > points[shallowest].penetration) {
        points[shallowest] = ContactPoint{position, penetration, 0.0f, {0.0f, 0.0f}, feature_id};
    }
}

f32 Manifold::max_penetration() const {
    f32 best = 0.0f;
    for (u32 i = 0; i < point_count; ++i) {
        best = std::max(best, points[i].penetration);
    }
    return best;
}

// --- Sphere vs sphere -------------------------------------------------------

bool collide_sphere_sphere(f32 radius_a, const Vec3& pos_a,
                           f32 radius_b, const Vec3& pos_b,
                           Manifold& out) {
    const Vec3 delta = pos_b - pos_a;
    const f32 distance = delta.length();
    const f32 combined = radius_a + radius_b;
    if (distance >= combined) {
        return false;
    }

    // Coincident centres have no defined normal; pick up so the pair separates
    // instead of producing NaNs that poison every later step.
    const Vec3 normal = (distance > kAxisEpsilon) ? delta / distance : Vec3(0.0f, 1.0f, 0.0f);
    const f32 penetration = combined - distance;

    // Midpoint of the overlap, so the contact sits between the two surfaces.
    const Vec3 point = pos_a + normal * (radius_a - penetration * 0.5f);
    out.normal = normal;
    out.add_point(point, penetration, 0);
    return true;
}

// --- Sphere vs box ----------------------------------------------------------

bool collide_sphere_box(f32 radius_a, const Vec3& pos_a,
                        const Vec3& half_extents_b, const Vec3& pos_b, const Quat& rot_b,
                        Manifold& out) {
    // Work in the box's local frame: the box becomes axis-aligned and the sphere
    // becomes a point, which turns the problem into a clamp.
    const Vec3 local = rot_b.conjugate().rotate(pos_a - pos_b);
    const Vec3 closest{
        std::clamp(local.x, -half_extents_b.x, half_extents_b.x),
        std::clamp(local.y, -half_extents_b.y, half_extents_b.y),
        std::clamp(local.z, -half_extents_b.z, half_extents_b.z),
    };

    const Vec3 delta = local - closest;
    const f32 distance = delta.length();

    Vec3 local_normal;
    f32 penetration;

    if (distance > kAxisEpsilon) {
        if (distance >= radius_a) {
            return false;
        }
        // Centre outside the box: push along the vector to the closest surface
        // point.
        local_normal = delta / distance;
        penetration = radius_a - distance;
    } else {
        // Centre inside the box. The closest-point direction is undefined, so
        // exit through the nearest face instead — otherwise a sphere that
        // tunnels into a box gets pushed in an arbitrary direction.
        const Vec3 face_depth{
            half_extents_b.x - std::fabs(local.x),
            half_extents_b.y - std::fabs(local.y),
            half_extents_b.z - std::fabs(local.z),
        };
        int axis = 0;
        if (face_depth.y < component(face_depth, axis)) axis = 1;
        if (face_depth.z < component(face_depth, axis)) axis = 2;
        local_normal = Vec3::zero;
        const f32 sign = (component(local, axis) >= 0.0f) ? 1.0f : -1.0f;
        if (axis == 0) local_normal.x = sign;
        else if (axis == 1) local_normal.y = sign;
        else local_normal.z = sign;
        penetration = radius_a + component(face_depth, axis);
    }

    // The manifold normal points from A (sphere) towards B (box), so it is the
    // negation of the direction that pushes the sphere out of the box.
    const Vec3 normal = -rot_b.rotate(local_normal);
    out.normal = normal;

    const Vec3 contact = pos_a + normal * (radius_a - penetration * 0.5f);
    out.add_point(contact, penetration, 0);
    return true;
}

// --- Sphere vs plane --------------------------------------------------------

bool collide_sphere_plane(f32 radius_a, const Vec3& pos_a,
                          const Vec3& plane_normal_b, const Vec3& pos_b, const Quat& rot_b,
                          Manifold& out) {
    const Vec3 n = rot_b.rotate(plane_normal_b);
    const f32 signed_distance = (pos_a - pos_b).dot(n);
    if (signed_distance >= radius_a) {
        return false;
    }

    const f32 penetration = radius_a - signed_distance;
    // A -> B is from the sphere towards the plane, i.e. against the plane normal.
    out.normal = -n;

    const Vec3 sphere_surface = pos_a - n * radius_a;
    const Vec3 plane_surface = pos_a - n * signed_distance;
    out.add_point((sphere_surface + plane_surface) * 0.5f, penetration, 0);
    return true;
}

// --- Box vs plane -----------------------------------------------------------

bool collide_box_plane(const Vec3& half_extents_a, const Vec3& pos_a, const Quat& rot_a,
                       const Vec3& plane_normal_b, const Vec3& pos_b, const Quat& rot_b,
                       Manifold& out) {
    const Vec3 n = rot_b.rotate(plane_normal_b);
    Vec3 axes[3];
    box_axes(rot_a, axes);

    // Only the corners can penetrate a plane, so test all eight and keep the
    // ones behind it. `add_point` evicts the shallowest when full, which keeps
    // the four deepest corners — exactly what a resting box needs.
    out.normal = -n;
    bool touching = false;
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            for (int sz = -1; sz <= 1; sz += 2) {
                const Vec3 corner = pos_a +
                    axes[0] * (static_cast<f32>(sx) * half_extents_a.x) +
                    axes[1] * (static_cast<f32>(sy) * half_extents_a.y) +
                    axes[2] * (static_cast<f32>(sz) * half_extents_a.z);
                const f32 signed_distance = (corner - pos_b).dot(n);
                if (signed_distance >= 0.0f) {
                    continue;
                }
                touching = true;
                const Vec3 projected = corner - n * signed_distance;
                const u32 feature = static_cast<u32>((sx > 0 ? 4 : 0) + (sy > 0 ? 2 : 0) + (sz > 0 ? 1 : 0));
                out.add_point((corner + projected) * 0.5f, -signed_distance, feature);
            }
        }
    }
    return touching;
}

// --- Box vs box -------------------------------------------------------------

bool collide_box_box(const Vec3& half_extents_a, const Vec3& pos_a, const Quat& rot_a,
                     const Vec3& half_extents_b, const Vec3& pos_b, const Quat& rot_b,
                     Manifold& out) {
    Vec3 axes_a[3], axes_b[3];
    box_axes(rot_a, axes_a);
    box_axes(rot_b, axes_b);
    const Vec3 d = pos_b - pos_a;

    f32 best_overlap = std::numeric_limits<f32>::max();
    Vec3 best_axis(0.0f, 1.0f, 0.0f);
    // best_kind is the CATEGORY: 0 = a face of A, 3 = a face of B, 6 = an
    // edge-edge axis. The axis index is best_i (and best_j for the edge case).
    // These were once conflated, which pinned the reference face to axis 0 and
    // produced a correct normal with entirely wrong penetration depths.
    int best_kind = -1;
    int best_i = 0;
    int best_j = 0;

    auto test_axis = [&](const Vec3& raw, int kind, int i, int j) -> bool {
        const f32 len2 = raw.length_sq();
        if (len2 < kAxisEpsilon) {
            // Parallel edges give a zero cross product; there is no axis to test.
            return true;
        }
        Vec3 n = raw / std::sqrt(len2);
        // Orient A -> B so the manifold normal has a consistent meaning.
        if (n.dot(d) < 0.0f) {
            n = -n;
        }
        const f32 ra = project_radius(half_extents_a, axes_a, n);
        const f32 rb = project_radius(half_extents_b, axes_b, n);
        const f32 overlap = ra + rb - std::fabs(d.dot(n));
        if (overlap <= 0.0f) {
            return false; // separating axis
        }
        if (overlap < best_overlap) {
            best_overlap = overlap;
            best_axis = n;
            best_kind = kind;
            best_i = i;
            best_j = j;
        }
        return true;
    };

    for (int i = 0; i < 3; ++i) {
        if (!test_axis(axes_a[i], 0, i, 0)) return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (!test_axis(axes_b[i], 3, i, 0)) return false;
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (!test_axis(axes_a[i].cross(axes_b[j]), 6, i, j)) return false;
        }
    }

    out.normal = best_axis;

    // --- Edge-edge ----------------------------------------------------------
    if (best_kind >= 6) {
        const Vec3 ea = axes_a[best_i];
        const Vec3 eb = axes_b[best_j];
        const Vec3 ca = edge_centre(pos_a, axes_a, half_extents_a, best_i, best_axis);
        const Vec3 cb = edge_centre(pos_b, axes_b, half_extents_b, best_j, -best_axis);
        Vec3 pa, pb;
        closest_points_segments(ca, ea, component(half_extents_a, best_i),
                                cb, eb, component(half_extents_b, best_j), pa, pb);
        // A single point: adequate for a corner-first impact. A proper manifold
        // would need two, and the non-goals say so.
        out.add_point((pa + pb) * 0.5f, best_overlap, 1000 + static_cast<u32>(best_i * 3 + best_j));
        return true;
    }

    // --- Face contact: clip the incident face against the reference face -----
    const bool a_is_ref = (best_kind == 0);
    const Vec3* ref_h = a_is_ref ? &half_extents_a : &half_extents_b;
    const Vec3* inc_h = a_is_ref ? &half_extents_b : &half_extents_a;
    const Vec3 ref_pos = a_is_ref ? pos_a : pos_b;
    const Vec3 inc_pos = a_is_ref ? pos_b : pos_a;
    const Vec3* ref_axes = a_is_ref ? axes_a : axes_b;
    const Vec3* inc_axes = a_is_ref ? axes_b : axes_a;
    const int ref_axis = best_i; // the axis index, valid for either box's face

    // The reference face normal, oriented from the reference box towards the
    // incident one.
    //
    // best_axis always points A -> B. When the reference box is B its face must
    // point the other way — from B back towards A. Omitting that negation picks
    // B's FAR face as the reference, and the incident face is then chosen
    // against a normal pointing away from the contact, so the pair reports a
    // penetration the size of an entire box and launches the bodies apart.
    const Vec3 ref_dir = a_is_ref ? best_axis : -best_axis;
    const f32 ref_sign = (ref_dir.dot(ref_axes[ref_axis]) >= 0.0f) ? 1.0f : -1.0f;
    const Vec3 ref_normal = ref_axes[ref_axis] * ref_sign;

    // The incident face is the one whose outward normal points most directly
    // back at the reference box. Selecting it by the SMALLEST dot product of the
    // box's positive axes is wrong for axis-aligned boxes — every positive axis
    // is either perpendicular or aligned, never anti-parallel, so the search
    // settles on a perpendicular axis and clips the wrong face. The axis is
    // chosen by largest |dot| and the sign is then taken opposite to the
    // reference normal.
    int inc_axis = 0;
    f32 best_abs_dot = -1.0f;
    for (int i = 0; i < 3; ++i) {
        const f32 dp = std::fabs(inc_axes[i].dot(ref_normal));
        if (dp > best_abs_dot) {
            best_abs_dot = dp;
            inc_axis = i;
        }
    }
    const f32 inc_sign = (inc_axes[inc_axis].dot(ref_normal) > 0.0f) ? -1.0f : 1.0f;

    Vec3 incident[4];
    face_corners(inc_pos, inc_axes, *inc_h, inc_axis, inc_sign, incident);

    // Clip against the four side planes of the reference face.
    Vec3 buffer_a[8], buffer_b[8];
    int count = 4;
    std::copy(incident, incident + 4, buffer_a);

    const int side_axes[2] = {(ref_axis + 1) % 3, (ref_axis + 2) % 3};
    for (int s = 0; s < 2 && count > 0; ++s) {
        const int axis = side_axes[s];
        const f32 extent = component(*ref_h, axis);
        // Two planes per side axis: +extent and -extent.
        count = clip_polygon(buffer_a, count, ref_axes[axis], extent, buffer_b);
        std::copy(buffer_b, buffer_b + count, buffer_a);
        if (count > 0) {
            count = clip_polygon(buffer_a, count, -ref_axes[axis], extent, buffer_b);
            std::copy(buffer_b, buffer_b + count, buffer_a);
        }
    }

    const Vec3 ref_face_point = ref_pos + ref_normal * component(*ref_h, ref_axis);
    for (int i = 0; i < count; ++i) {
        const f32 separation = (buffer_a[i] - ref_face_point).dot(ref_normal);
        if (separation > 0.0f) {
            continue; // above the reference face, not a contact
        }
        out.add_point(buffer_a[i], -separation, static_cast<u32>(i));
    }

    return out.point_count > 0;
}

// --- Dispatcher -------------------------------------------------------------

bool collide(const Shape& shape_a, const Vec3& pos_a, const Quat& rot_a,
             const Shape& shape_b, const Vec3& pos_b, const Quat& rot_b,
             Manifold& out) {
    out.clear();

    switch (shape_a.type) {
        case ShapeType::Sphere:
            switch (shape_b.type) {
                case ShapeType::Sphere:
                    return collide_sphere_sphere(shape_a.sphere.radius, pos_a,
                                                 shape_b.sphere.radius, pos_b, out);
                case ShapeType::Box:
                    return collide_sphere_box(shape_a.sphere.radius, pos_a,
                                              shape_b.box.half_extents, pos_b, rot_b, out);
                case ShapeType::Plane:
                    return collide_sphere_plane(shape_a.sphere.radius, pos_a,
                                                shape_b.plane.normal, pos_b, rot_b, out);
            }
            break;

        case ShapeType::Box:
            switch (shape_b.type) {
                case ShapeType::Sphere: {
                    // Solve as sphere-vs-box, then flip: the manifold normal is
                    // defined as A -> B, so swapping the operands inverts it.
                    Manifold flipped;
                    if (!collide_sphere_box(shape_b.sphere.radius, pos_b,
                                            shape_a.box.half_extents, pos_a, rot_a, flipped)) {
                        return false;
                    }
                    out.normal = -flipped.normal;
                    for (u32 i = 0; i < flipped.point_count; ++i) {
                        out.add_point(flipped.points[i].position,
                                      flipped.points[i].penetration,
                                      flipped.points[i].feature_id);
                    }
                    return true;
                }
                case ShapeType::Box:
                    return collide_box_box(shape_a.box.half_extents, pos_a, rot_a,
                                           shape_b.box.half_extents, pos_b, rot_b, out);
                case ShapeType::Plane:
                    return collide_box_plane(shape_a.box.half_extents, pos_a, rot_a,
                                             shape_b.plane.normal, pos_b, rot_b, out);
            }
            break;

        case ShapeType::Plane:
            // A plane against anything else: solve the other way round and flip.
            // A plane-plane pair has no meaningful contact and reports none.
            if (shape_b.type == ShapeType::Plane) {
                return false;
            }
            {
                Manifold flipped;
                if (!collide(shape_b, pos_b, rot_b, shape_a, pos_a, rot_a, flipped)) {
                    return false;
                }
                out.normal = -flipped.normal;
                for (u32 i = 0; i < flipped.point_count; ++i) {
                    out.add_point(flipped.points[i].position,
                                  flipped.points[i].penetration,
                                  flipped.points[i].feature_id);
                }
                return true;
            }
    }
    return false;
}

} // namespace nf::physics
