// NF/Scene2D/Light2D.cpp — 2D lights, shadow geometry and baked lighting.
// Design doc Section 54 (2D Engine): "2D lights", "Shadows".

#include <NF/Scene2D/Light2D.hpp>

#include <algorithm>
#include <cmath>

namespace nf::scene2d {

namespace {

/// Segment/segment intersection at a parameter strictly inside both. `t` is
/// the hit distance along (p0, p1), so a wall that sits *at* the query point
/// (t ~= 1) does not occlude it — the point is on the wall's surface, which is
/// the boundary case a lit edge should count as lit.
bool segments_cross(Vec2 p0, Vec2 p1, Vec2 q0, Vec2 q1) {
    const Vec2 r = p1 - p0;
    const Vec2 s = q1 - q0;
    const f32 denom = r.x * s.y - r.y * s.x;
    if (std::abs(denom) < EPSILON) return false;   // parallel or degenerate
    const Vec2 qp = q0 - p0;
    const f32 t = (qp.x * s.y - qp.y * s.x) / denom;
    const f32 u = (qp.x * r.y - qp.y * r.x) / denom;
    return t > EPSILON && t < 1.0f - EPSILON && u > EPSILON && u < 1.0f - EPSILON;
}

/// Point inside a clockwise-wound convex quad (y-down space): on the
/// non-negative side of every directed edge.
bool point_in_quad(const ShadowQuad& q, Vec2 p) {
    for (usize i = 0u; i < 4u; ++i) {
        if (signed_area_2x(q.v[i], q.v[(i + 1u) & 3u], p) < 0.0f) return false;
    }
    return true;
}

} // namespace

f32 Light2D::attenuation(Vec2 world_point) const {
    const Vec2 d = world_point - position;
    const f32 dist = d.length();
    if (dist > radius) return 0.0f;

    const f32 t = radius > EPSILON ? 1.0f - dist / radius : 1.0f;
    f32 a;
    switch (falloff) {
        case LightFalloff2D::Linear:    a = t; break;
        case LightFalloff2D::Quadratic: a = t * t; break;
        case LightFalloff2D::Smooth:
        default:                        a = t * t * (3.0f - 2.0f * t); break;
    }

    if (is_cone() && dist > EPSILON) {
        // The arc test is a dot against the cone axis: cheaper than atan2 and
        // exact, since the cone boundary is a constant angle from that axis.
        const Vec2 dir{std::cos(to_radians(direction_deg)),
                       std::sin(to_radians(direction_deg))};
        const f32 cos_half = std::cos(to_radians(spread_deg) * 0.5f);
        if ((d / dist).dot(dir) < cos_half) return 0.0f;
    }
    return a;
}

f32 Light2D::flicker(f32 time) const {
    if (flicker_amount <= 0.0f) return 1.0f;
    // Hash-seeded phase rather than an RNG: two runs of the same timeline
    // strobe identically, which is what a recorded replay needs (Section 114).
    const f32 phase = hash_to_unit(flicker_phase) * TWO_PI;
    return 1.0f + flicker_amount * std::sin(TWO_PI * flicker_speed * time + phase);
}

void Lighting2D::set_occluders_from_rects(const std::vector<Rect>& rects) {
    std::vector<OccluderSegment> segs;
    segs.reserve(rects.size() * 4u);
    for (const Rect& r : rects) {
        const Vec2 tl{r.x, r.y};
        const Vec2 tr{r.x + r.w, r.y};
        const Vec2 br{r.x + r.w, r.y + r.h};
        const Vec2 bl{r.x, r.y + r.h};
        // Clockwise in y-down space, so each edge's outward side is the one the
        // shadow bake projects into.
        segs.push_back({tl, tr});
        segs.push_back({tr, br});
        segs.push_back({br, bl});
        segs.push_back({bl, tl});
    }
    set_occluders(std::move(segs));
}

void Lighting2D::bake(f32 time) {
    m_time = time;
    m_baked = true;
    m_shadows.assign(m_lights.size(), {});

    for (usize i = 0u; i < m_lights.size(); ++i) {
        const Light2D& l = m_lights[i];
        if (!l.cast_shadows) continue;

        const f32 r2 = l.radius * l.radius;
        for (const OccluderSegment& s : m_occluders) {
            // Cull edges the lit disc cannot see behind: a shadowed point lies
            // beyond the edge along its ray from the light, so an edge whose
            // nearest approach is already past the radius shadows nothing in
            // the disc. This is what keeps a large map's worth of distant
            // terrain edges out of every light's wedge list.
            const Vec2 near_pt = closest_point_on_segment(s.a, s.b, l.position);
            if ((near_pt - l.position).length_sq() > r2) continue;

            const Vec2 da = s.a - l.position;
            const Vec2 db = s.b - l.position;
            const f32 la = da.length();
            const f32 lb = db.length();
            if (la <= EPSILON || lb <= EPSILON) continue;   // edge on the light

            // Project both endpoints out to the radius: the lit region is the
            // disc, so a wedge truncated at the radius covers every shadowed
            // point that could still be lit.
            ShadowQuad q;
            q.v[0] = s.a;
            q.v[1] = s.b;
            q.v[2] = l.position + db * (l.radius / lb);
            q.v[3] = l.position + da * (l.radius / la);

            // Wind the quad clockwise in y-down space so the single-sign
            // interior test applies. Reversing the vertex order traverses the
            // same simple polygon, so this never changes the shape — only its
            // orientation. A zero-area quad is an edge seen exactly side-on,
            // which casts no shadow at all.
            f32 area = 0.0f;
            for (usize k = 0u; k < 4u; ++k) {
                area += q.v[k].x * q.v[(k + 1u) & 3u].y - q.v[k].y * q.v[(k + 1u) & 3u].x;
            }
            if (area <= EPSILON && area >= -EPSILON) continue;
            if (area < 0.0f) {
                std::swap(q.v[0], q.v[3]);
                std::swap(q.v[1], q.v[2]);
            }
            m_shadows[i].push_back(q);
        }
    }
}

usize Lighting2D::shadow_quad_count(usize light_index) const {
    if (light_index >= m_shadows.size()) return 0u;
    return m_shadows[light_index].size();
}

usize Lighting2D::total_shadow_quad_count() const {
    usize total = 0u;
    for (const std::vector<ShadowQuad>& quads : m_shadows) {
        total += quads.size();
    }
    return total;
}

bool Lighting2D::in_shadow(usize light_index, Vec2 world_point) const {
    if (light_index >= m_shadows.size()) return false;
    for (const ShadowQuad& q : m_shadows[light_index]) {
        if (point_in_quad(q, world_point)) return true;
    }
    return false;
}

bool Lighting2D::is_occluded(Vec2 from, Vec2 to) const {
    for (const OccluderSegment& s : m_occluders) {
        if (segments_cross(from, to, s.a, s.b)) return true;
    }
    return false;
}

Vec3 Lighting2D::sample(Vec2 world_point) const {
    Vec3 acc = m_ambient;
    for (usize i = 0u; i < m_lights.size(); ++i) {
        const Light2D& l = m_lights[i];
        f32 a = l.attenuation(world_point);
        if (a <= 0.0f) continue;
        a *= l.flicker(m_time);
        if (a <= EPSILON) continue;

        if (l.cast_shadows) {
            // The bake and the ray test agree wherever the light reaches; the
            // bake is just the cheaper one, and it is the one a per-sprite
            // query wants. They differ only on the boundary: a ray grazing a
            // wall corner, or a point on a wall surface, where the ray test's
            // strictly-inside crossing and the bake's inclusive wedge interior
            // make different calls. Neither answer is wrong — a zero-width
            // shadow is not a thing a sprite centre lands on.
            const bool shadowed = m_baked ? in_shadow(i, world_point)
                                          : is_occluded(l.position, world_point);
            if (shadowed) continue;
        }
        acc = acc + l.color * (a * l.intensity);
    }
    return acc;
}

f32 Lighting2D::sample_luminance(Vec2 world_point) const {
    return sample(world_point).dot(Vec3{0.2126f, 0.7152f, 0.0722f});
}

Vec3 unpack_color_rgb(u32 packed) {
    return Vec3{static_cast<f32>(packed & 0xFFu) / 255.0f,
                static_cast<f32>((packed >> 8) & 0xFFu) / 255.0f,
                static_cast<f32>((packed >> 16) & 0xFFu) / 255.0f};
}

} // namespace nf::scene2d
