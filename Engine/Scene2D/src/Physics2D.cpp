// NF/Scene2D/Physics2D.cpp — sequential-impulse 2D world.
// Pair generation sorts; contact order is by (min index, max index, then y, x).

#include <NF/Scene2D/Physics2D.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace nf::scene2d {

namespace {

constexpr f32 kSlop = 0.01f;
constexpr f32 kBaumgarte = 0.2f;
constexpr f32 kMaxCorrection = 0.2f;

i32 hash_cell(f32 v, f32 cell) {
    return static_cast<i32>(std::floor(v / cell));
}

u64 pack_cell(i32 cx, i32 cy) {
    return (static_cast<u64>(static_cast<u32>(cx)) << 32) |
           static_cast<u64>(static_cast<u32>(cy));
}

Vec2 rotate_local(Vec2 local, f32 angle) {
    const f32 c = std::cos(angle);
    const f32 s = std::sin(angle);
    // Clockwise in y-down: same convention as Mat3x2::rotation_deg.
    return Vec2{local.x * c - local.y * s, local.x * s + local.y * c};
}

Vec2 closest_on_obb(const Body2D& box, Vec2 world_point) {
    const Vec2 d = world_point - box.position;
    const f32 c = std::cos(-box.angle);
    const f32 s = std::sin(-box.angle);
    const Vec2 local{d.x * c - d.y * s, d.x * s + d.y * c};
    const Vec2 clamped{
        clamp(local.x, -box.shape.half.x, box.shape.half.x),
        clamp(local.y, -box.shape.half.y, box.shape.half.y)};
    return box.position + rotate_local(clamped, box.angle);
}

bool point_in_body(const Body2D& b, Vec2 p) {
    if (b.shape.kind == Shape2D::Kind::Circle) {
        return (p - b.position).length_sq() <= b.shape.radius * b.shape.radius;
    }
    const Vec2 d = p - b.position;
    const f32 c = std::cos(-b.angle);
    const f32 s = std::sin(-b.angle);
    const Vec2 local{d.x * c - d.y * s, d.x * s + d.y * c};
    return std::abs(local.x) <= b.shape.half.x && std::abs(local.y) <= b.shape.half.y;
}

bool ray_circle(Vec2 start, Vec2 dir, f32 max_d, Vec2 centre, f32 r,
                f32& t, Vec2& normal) {
    const Vec2 m = start - centre;
    const f32 b = m.dot(dir);
    const f32 c = m.dot(m) - r * r;
    const f32 disc = b * b - dir.dot(dir) * c;
    if (disc < 0.0f) return false;
    const f32 inv = 1.0f / dir.dot(dir);
    const f32 sdisc = std::sqrt(disc);
    f32 hit = (-b - sdisc) * inv;
    if (hit < 0.0f) hit = (-b + sdisc) * inv;
    if (hit < 0.0f || hit > max_d) return false;
    t = hit;
    const Vec2 p = start + dir * hit;
    normal = (p - centre).normalized();
    return true;
}

bool ray_aabb_local(Vec2 origin, Vec2 dir, Vec2 half, f32 max_d, f32& t, Vec2& n) {
    f32 tmin = 0.0f;
    f32 tmax = max_d;
    Vec2 hit_n{0.0f, 0.0f};
    const f32 orig[2] = {origin.x, origin.y};
    const f32 d[2] = {dir.x, dir.y};
    const f32 h[2] = {half.x, half.y};
    for (int i = 0; i < 2; ++i) {
        if (std::abs(d[i]) < EPSILON) {
            if (orig[i] < -h[i] || orig[i] > h[i]) return false;
            continue;
        }
        const f32 inv = 1.0f / d[i];
        f32 t1 = (-h[i] - orig[i]) * inv;
        f32 t2 = (h[i] - orig[i]) * inv;
        f32 sign = -1.0f;
        if (t1 > t2) {
            std::swap(t1, t2);
            sign = 1.0f;
        }
        if (t1 > tmin) {
            tmin = t1;
            hit_n = i == 0 ? Vec2{sign, 0.0f} : Vec2{0.0f, sign};
        }
        tmax = tmax < t2 ? tmax : t2;
        if (tmin > tmax) return false;
    }
    if (tmin < 0.0f || tmin > max_d) return false;
    t = tmin;
    n = hit_n;
    return true;
}

} // namespace

PhysicsWorld2D::Slot* PhysicsWorld2D::slot(BodyHandle handle) {
    if (handle.index >= m_slots.size()) return nullptr;
    Slot& s = m_slots[handle.index];
    if (!s.alive || s.generation != handle.generation) return nullptr;
    return &s;
}

const PhysicsWorld2D::Slot* PhysicsWorld2D::slot(BodyHandle handle) const {
    if (handle.index >= m_slots.size()) return nullptr;
    const Slot& s = m_slots[handle.index];
    if (!s.alive || s.generation != handle.generation) return nullptr;
    return &s;
}

BodyHandle PhysicsWorld2D::create_body(const Body2D& body) {
    Body2D copy = body;
    copy.update_mass_properties();
    u32 index = 0;
    if (!m_free.empty()) {
        index = m_free.back();
        m_free.pop_back();
        Slot& s = m_slots[index];
        s.body = copy;
        s.alive = true;
        ++s.generation;
        if (s.generation == 0) s.generation = 1;
    } else {
        index = static_cast<u32>(m_slots.size());
        Slot s;
        s.body = copy;
        s.alive = true;
        s.generation = 1;
        m_slots.push_back(s);
    }
    ++m_body_count;
    return BodyHandle{index, m_slots[index].generation};
}

void PhysicsWorld2D::destroy_body(BodyHandle handle) {
    Slot* s = slot(handle);
    if (!s) return;
    s->alive = false;
    ++s->generation;
    if (s->generation == 0) s->generation = 1;
    m_free.push_back(handle.index);
    --m_body_count;
}

Body2D* PhysicsWorld2D::body(BodyHandle handle) {
    Slot* s = slot(handle);
    return s ? &s->body : nullptr;
}

const Body2D* PhysicsWorld2D::body(BodyHandle handle) const {
    const Slot* s = slot(handle);
    return s ? &s->body : nullptr;
}

void PhysicsWorld2D::refresh_mass_properties(BodyHandle handle) {
    if (Body2D* b = body(handle)) b->update_mass_properties();
}

u32 PhysicsWorld2D::add_distance_constraint(const DistanceConstraint2D& c) {
    const u32 i = static_cast<u32>(m_distance.size());
    m_distance.push_back(c);
    return i;
}

u32 PhysicsWorld2D::add_revolute_constraint(const RevoluteConstraint2D& c) {
    const u32 i = static_cast<u32>(m_revolute.size());
    m_revolute.push_back(c);
    return i;
}

void PhysicsWorld2D::clear() {
    m_slots.clear();
    m_free.clear();
    m_body_count = 0;
    m_distance.clear();
    m_revolute.clear();
    m_contacts.clear();
    m_acc_normal.clear();
    m_acc_friction.clear();
    m_restitution_bias.clear();
}

void PhysicsWorld2D::apply_damping(f32 dt) {
    for (Slot& s : m_slots) {
        if (!s.alive || s.body.is_static) continue;
        Body2D& b = s.body;
        b.velocity *= std::exp(-b.linear_damping * dt);
        b.angular_velocity *= std::exp(-b.angular_damping * dt);
    }
}

void PhysicsWorld2D::integrate_bodies(f32 dt) {
    for (Slot& s : m_slots) {
        if (!s.alive || s.body.is_static) {
            if (s.alive) {
                s.body.force = Vec2{0.0f, 0.0f};
                s.body.torque = 0.0f;
            }
            continue;
        }
        Body2D& b = s.body;
        b.velocity += (b.force * b.inv_mass + m_gravity * b.gravity_scale) * dt;
        b.angular_velocity += b.torque * b.inv_inertia * dt;
        b.force = Vec2{0.0f, 0.0f};
        b.torque = 0.0f;
    }
    apply_damping(dt);
}

void PhysicsWorld2D::integrate_positions(f32 dt) {
    for (Slot& s : m_slots) {
        if (!s.alive || s.body.is_static) continue;
        s.body.position += s.body.velocity * dt;
        s.body.angle += s.body.angular_velocity * dt;
    }
}

std::vector<std::pair<u32, u32>> PhysicsWorld2D::broadphase_pairs() const {
    std::vector<std::pair<u32, u32>> pairs;
    f32 cell = 1.0f;
    for (const Slot& s : m_slots) {
        if (!s.alive) continue;
        const Vec2 h = s.body.shape.bounding_half();
        const f32 m = h.x > h.y ? h.x : h.y;
        if (m * 2.0f > cell) cell = m * 2.0f;
    }
    if (cell < 1.0f) cell = 1.0f;

    std::unordered_map<u64, std::vector<u32>> buckets;
    buckets.reserve(m_slots.size() * 2u);
    for (u32 i = 0; i < static_cast<u32>(m_slots.size()); ++i) {
        if (!m_slots[i].alive) continue;
        const Rect a = m_slots[i].body.aabb(0.05f);
        const i32 x0 = hash_cell(a.left(), cell);
        const i32 y0 = hash_cell(a.top(), cell);
        const i32 x1 = hash_cell(a.right(), cell);
        const i32 y1 = hash_cell(a.bottom(), cell);
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                buckets[pack_cell(x, y)].push_back(i);
            }
        }
    }

    for (auto& kv : buckets) {
        auto& list = kv.second;
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
        for (usize i = 0; i < list.size(); ++i) {
            for (usize j = i + 1; j < list.size(); ++j) {
                u32 a = list[i];
                u32 b = list[j];
                if (a > b) std::swap(a, b);
                // Two massless bodies cannot be resolved by the solver (both
                // impulse denominators are zero), so a static-static pair is
                // pure narrowphase cost. A tilemap injects one static body per
                // collision rect, so on a large map these pairs would otherwise
                // outnumber the dynamic ones. Triggers are exempt: a static
                // sensor overlapping a static body is a gameplay overlap query
                // the world is expected to report.
                if (m_slots[a].body.is_static && m_slots[b].body.is_static &&
                    !m_slots[a].body.is_trigger && !m_slots[b].body.is_trigger) {
                    continue;
                }
                if (!m_slots[a].body.aabb().overlaps(m_slots[b].body.aabb())) continue;
                pairs.emplace_back(a, b);
            }
        }
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs;
}

bool PhysicsWorld2D::circle_circle(const Body2D& a, const Body2D& b,
                                   ContactManifold& out) const {
    const Vec2 d = b.position - a.position;
    const f32 dist_sq = d.length_sq();
    const f32 r = a.shape.radius + b.shape.radius;
    if (dist_sq > r * r) return false;
    const f32 dist = std::sqrt(dist_sq);
    if (dist > EPSILON) {
        out.normal = d / dist;
        out.penetration = r - dist;
    } else {
        out.normal = Vec2{0.0f, 1.0f};
        out.penetration = r;
    }
    out.points[0] = a.position + out.normal * a.shape.radius;
    out.point_count = 1;
    return true;
}

bool PhysicsWorld2D::circle_box(const Body2D& a, const Body2D& b,
                                ContactManifold& out) const {
    const bool circle_first = a.shape.kind == Shape2D::Kind::Circle;
    const Body2D& circle = circle_first ? a : b;
    const Body2D& box = circle_first ? b : a;
    const Vec2 closest = closest_on_obb(box, circle.position);
    Vec2 d = circle.position - closest;
    f32 dist_sq = d.length_sq();
    const bool inside = point_in_body(box, circle.position);
    if (!inside && dist_sq > circle.shape.radius * circle.shape.radius) return false;

    Vec2 n;
    f32 pen = 0.0f;
    if (inside) {
        const Vec2 local_d = rotate_local(circle.position - box.position, -box.angle);
        const f32 dx = box.shape.half.x - std::abs(local_d.x);
        const f32 dy = box.shape.half.y - std::abs(local_d.y);
        if (dx < dy) {
            n = rotate_local(Vec2{local_d.x < 0.0f ? -1.0f : 1.0f, 0.0f}, box.angle);
            pen = dx + circle.shape.radius;
        } else {
            n = rotate_local(Vec2{0.0f, local_d.y < 0.0f ? -1.0f : 1.0f}, box.angle);
            pen = dy + circle.shape.radius;
        }
        // n is box -> circle. Manifold normal is A -> B.
        if (circle_first) n = -n;
    } else {
        const f32 dist = std::sqrt(dist_sq);
        n = dist > EPSILON ? d / dist : Vec2{0.0f, 1.0f};
        if (circle_first) n = -n;
        pen = circle.shape.radius - dist;
    }
    out.normal = n;
    out.penetration = pen;
    out.points[0] = closest;
    out.point_count = 1;
    return true;
}

bool PhysicsWorld2D::box_box(const Body2D& a, const Body2D& b,
                             ContactManifold& out) const {
    const f32 ca = std::cos(a.angle);
    const f32 sa = std::sin(a.angle);
    const f32 cb = std::cos(b.angle);
    const f32 sb = std::sin(b.angle);
    const Vec2 axes[4] = {
        Vec2{ca, sa}, Vec2{-sa, ca},
        Vec2{cb, sb}, Vec2{-sb, cb},
    };
    const Vec2 d = b.position - a.position;
    f32 min_pen = 1e30f;
    Vec2 best{0.0f, 1.0f};
    // Which body owns the minimum-penetration axis: axes 0-1 are a's, 2-3 are
    // b's. That body presents the reference face; the other one is incident.
    bool best_is_b = false;

    const auto project = [](const Body2D& body, Vec2 axis) {
        const Vec2 hx = rotate_local(Vec2{body.shape.half.x, 0.0f}, body.angle);
        const Vec2 hy = rotate_local(Vec2{0.0f, body.shape.half.y}, body.angle);
        return std::abs(hx.dot(axis)) + std::abs(hy.dot(axis));
    };

    for (int axis_index = 0; axis_index < 4; ++axis_index) {
        const Vec2 axis = axes[axis_index].normalized();
        const f32 ra = project(a, axis);
        const f32 rb = project(b, axis);
        const f32 overlap = ra + rb - std::abs(d.dot(axis));
        if (overlap <= 0.0f) return false;
        if (overlap < min_pen) {
            min_pen = overlap;
            best = d.dot(axis) < 0.0f ? -axis : axis;
            best_is_b = axis_index >= 2;
        }
    }

    // `best` always points a -> b; the solver and this function's callers
    // (queries, debug draws) all read the normal that way, so it is not
    // reoriented even though the clipping below works in reference space.
    out.normal = best;
    out.penetration = min_pen;

    // Clip the incident body's corners against the reference face. Depth is
    // measured from the reference body's face toward the incident body, so a
    // corner actually at the contact reads ~0 and one a body-width away reads
    // negative. Flipping that sign (or clipping the wrong body) makes the sort
    // pick the corners *farthest* from the contact, and the solver then pins
    // the body to phantom anchors — a box resting flat on a floor would slowly
    // spin up and walk off it, its centre velocity pinned at the contact
    // instead of the body.
    const Body2D& ref = best_is_b ? b : a;
    const Body2D& inc = best_is_b ? a : b;
    const Vec2 n = best_is_b ? -best : best;  // reference -> incident

    Vec2 corners[4] = {
        inc.position + rotate_local(Vec2{-inc.shape.half.x, -inc.shape.half.y}, inc.angle),
        inc.position + rotate_local(Vec2{inc.shape.half.x, -inc.shape.half.y}, inc.angle),
        inc.position + rotate_local(Vec2{inc.shape.half.x, inc.shape.half.y}, inc.angle),
        inc.position + rotate_local(Vec2{-inc.shape.half.x, inc.shape.half.y}, inc.angle),
    };
    f32 depths[4];
    for (int i = 0; i < 4; ++i) {
        depths[i] = project(ref, n) - (corners[i] - ref.position).dot(n);
    }
    int order[4] = {0, 1, 2, 3};
    std::sort(order, order + 4, [&](int i, int j) { return depths[i] > depths[j]; });
    // Two support points, corners barely outside still counting within the
    // solver's slop: a single off-centre point would let a flat rest tip.
    out.point_count = 0;
    if (depths[order[0]] >= -kSlop) {
        out.points[out.point_count++] = corners[order[0]];
        if (depths[order[1]] >= -kSlop) out.points[out.point_count++] = corners[order[1]];
    }
    if (out.point_count == 0) {
        out.points[0] = (a.position + b.position) * 0.5f;
        out.point_count = 1;
    }
    return true;
}

bool PhysicsWorld2D::collide(const Body2D& a, const Body2D& b,
                             ContactManifold& out) const {
    out = ContactManifold{};
    bool hit = false;
    if (a.shape.kind == Shape2D::Kind::Circle &&
        b.shape.kind == Shape2D::Kind::Circle) {
        hit = circle_circle(a, b, out);
    } else if (a.shape.kind == Shape2D::Kind::Box &&
               b.shape.kind == Shape2D::Kind::Box) {
        hit = box_box(a, b, out);
    } else {
        hit = circle_box(a, b, out);
    }
    if (!hit) return false;
    out.restitution = std::sqrt(a.restitution * b.restitution);
    out.friction = std::sqrt(a.friction * b.friction);
    out.is_trigger = a.is_trigger || b.is_trigger;
    return true;
}

bool PhysicsWorld2D::shapes_overlap(const Body2D& a, const Body2D& b) const {
    ContactManifold m;
    return collide(a, b, m);
}

void PhysicsWorld2D::find_contacts() {
    m_contacts.clear();
    const auto pairs = broadphase_pairs();
    for (const auto& p : pairs) {
        ContactManifold m;
        if (!collide(m_slots[p.first].body, m_slots[p.second].body, m)) continue;
        m.body_a = p.first;
        m.body_b = p.second;
        m_contacts.push_back(m);
    }
    std::stable_sort(m_contacts.begin(), m_contacts.end(),
                     [](const ContactManifold& a, const ContactManifold& b) {
                         if (a.body_a != b.body_a) return a.body_a < b.body_a;
                         if (a.body_b != b.body_b) return a.body_b < b.body_b;
                         if (a.points[0].y != b.points[0].y) return a.points[0].y < b.points[0].y;
                         return a.points[0].x < b.points[0].x;
                     });
}

void PhysicsWorld2D::prepare_contacts() {
    const usize n = m_contacts.size();
    m_acc_normal.assign(n * 2u, 0.0f);
    m_acc_friction.assign(n * 2u, 0.0f);
    m_restitution_bias.assign(n, 0.0f);
    for (usize i = 0; i < n; ++i) {
        const ContactManifold& m = m_contacts[i];
        if (m.is_trigger || m.point_count == 0) continue;
        const Body2D& a = m_slots[m.body_a].body;
        const Body2D& b = m_slots[m.body_b].body;
        const Vec2 p = m.points[0];
        const Vec2 va = a.velocity_at_point(p);
        const Vec2 vb = b.velocity_at_point(p);
        const f32 vn = (vb - va).dot(m.normal);
        f32 bias = 0.0f;
        if (vn < -1.0f) bias = -vn * m.restitution;
        m_restitution_bias[i] = bias;
    }
}

void PhysicsWorld2D::solve_velocities() {
    for (usize i = 0; i < m_contacts.size(); ++i) {
        ContactManifold& m = m_contacts[i];
        if (m.is_trigger) continue;
        Body2D& a = m_slots[m.body_a].body;
        Body2D& b = m_slots[m.body_b].body;
        for (u32 k = 0; k < m.point_count; ++k) {
            const Vec2 p = m.points[k];
            const Vec2 ra = p - a.position;
            const Vec2 rb = p - b.position;
            const Vec2 va = a.velocity_at_point(p);
            const Vec2 vb = b.velocity_at_point(p);
            const Vec2 rv = vb - va;
            const f32 vn = rv.dot(m.normal);
            const f32 ra_n = ra.x * m.normal.y - ra.y * m.normal.x;
            const f32 rb_n = rb.x * m.normal.y - rb.y * m.normal.x;
            const f32 inv =
                a.inv_mass + b.inv_mass + ra_n * ra_n * a.inv_inertia +
                rb_n * rb_n * b.inv_inertia;
            if (inv <= EPSILON) continue;
            f32 j = -(vn - m_restitution_bias[i]) / inv;
            const usize acc_i = i * 2u + k;
            const f32 old = m_acc_normal[acc_i];
            m_acc_normal[acc_i] = old + j;
            if (m_acc_normal[acc_i] < 0.0f) m_acc_normal[acc_i] = 0.0f;
            j = m_acc_normal[acc_i] - old;
            const Vec2 impulse = m.normal * j;
            a.apply_impulse_at_point(-impulse, p);
            b.apply_impulse_at_point(impulse, p);

            const Vec2 tangent = Vec2{-m.normal.y, m.normal.x};
            const Vec2 rv2 = b.velocity_at_point(p) - a.velocity_at_point(p);
            const f32 vt = rv2.dot(tangent);
            const f32 ra_t = ra.x * tangent.y - ra.y * tangent.x;
            const f32 rb_t = rb.x * tangent.y - rb.y * tangent.x;
            const f32 inv_t =
                a.inv_mass + b.inv_mass + ra_t * ra_t * a.inv_inertia +
                rb_t * rb_t * b.inv_inertia;
            if (inv_t <= EPSILON) continue;
            f32 jt = -vt / inv_t;
            const f32 max_f = m.friction * m_acc_normal[acc_i];
            const f32 old_f = m_acc_friction[acc_i];
            m_acc_friction[acc_i] = clamp(old_f + jt, -max_f, max_f);
            jt = m_acc_friction[acc_i] - old_f;
            const Vec2 ft = tangent * jt;
            a.apply_impulse_at_point(-ft, p);
            b.apply_impulse_at_point(ft, p);
        }
    }
}

void PhysicsWorld2D::solve_positions() {
    for (ContactManifold& m : m_contacts) {
        if (m.is_trigger || m.point_count == 0) continue;
        Body2D& a = m_slots[m.body_a].body;
        Body2D& b = m_slots[m.body_b].body;
        ContactManifold fresh;
        if (!collide(a, b, fresh)) continue;
        const f32 corr = clamp(kBaumgarte * (fresh.penetration - kSlop),
                               0.0f, kMaxCorrection);
        if (corr <= 0.0f) continue;
        const f32 inv = a.inv_mass + b.inv_mass;
        if (inv <= EPSILON) continue;
        const Vec2 n = fresh.normal;
        a.position -= n * (corr * a.inv_mass / inv);
        b.position += n * (corr * b.inv_mass / inv);
        m.penetration = fresh.penetration;
        m.normal = n;
    }
}

void PhysicsWorld2D::solve_constraints(f32 dt) {
    for (const DistanceConstraint2D& c : m_distance) {
        Body2D* a = body(c.a);
        Body2D* b = body(c.b);
        if (!a || !b) continue;
        const Vec2 d = b->position - a->position;
        const f32 len = d.length();
        if (len < EPSILON) continue;
        const Vec2 n = d / len;
        const f32 inv = a->inv_mass + b->inv_mass;
        if (inv <= EPSILON) continue;
        // Velocity-level solve with a Baumgarte term: the closing speed is
        // cancelled and `stiffness` bleeds the length error in over the step,
        // so 1.0 tracks the rest length and 0.0 leaves the pair free.
        const f32 Cdot = (b->velocity - a->velocity).dot(n);
        const f32 C = len - c.rest_length;
        const f32 j = -(Cdot + C * c.stiffness / dt) / inv;
        const Vec2 impulse = n * j;
        a->apply_impulse(-impulse);
        b->apply_impulse(impulse);
    }
    for (const RevoluteConstraint2D& c : m_revolute) {
        Body2D* a = body(c.a);
        Body2D* b = body(c.b);
        if (!a || !b) continue;
        const Vec2 ra = rotate_local(c.local_anchor_a, a->angle);
        const Vec2 rb = rotate_local(c.local_anchor_b, b->angle);
        const Vec2 wa = a->position + ra;
        const Vec2 wb = b->position + rb;
        // Cdot = (vb + w_b x rb) - (va + w_a x ra); the cross product with a
        // scalar angular velocity is the perpendicular, scaled.
        const Vec2 perp_a{-ra.y, ra.x};
        const Vec2 perp_b{-rb.y, rb.x};
        const Vec2 Cdot = (b->velocity + perp_b * b->angular_velocity) -
                          (a->velocity + perp_a * a->angular_velocity);
        // Effective mass K = (1/ma + 1/mb) I + skew(ra) Ia^-1 skew(ra)^T +
        // skew(rb) Ib^-1 skew(rb)^T, symmetric positive definite unless both
        // bodies are fixed. Solved explicitly rather than iteratively: a 2x2
        // system is one divide, and an approximate pin would let a hinged body
        // drift at speed proportional to the joint's error.
        const f32 im = a->inv_mass + b->inv_mass;
        const f32 k11 = im + ra.y * ra.y * a->inv_inertia + rb.y * rb.y * b->inv_inertia;
        const f32 k12 = -ra.x * ra.y * a->inv_inertia - rb.x * rb.y * b->inv_inertia;
        const f32 k22 = im + ra.x * ra.x * a->inv_inertia + rb.x * rb.x * b->inv_inertia;
        const f32 det = k11 * k22 - k12 * k12;
        if (det <= EPSILON) continue;
        const Vec2 P{(-k22 * Cdot.x + k12 * Cdot.y) / det,
                     (k12 * Cdot.x - k11 * Cdot.y) / det};
        a->apply_impulse_at_point(-P, wa);
        b->apply_impulse_at_point(P, wb);
    }
}

void PhysicsWorld2D::solve_constraint_positions() {
    for (const DistanceConstraint2D& c : m_distance) {
        Body2D* a = body(c.a);
        Body2D* b = body(c.b);
        if (!a || !b) continue;
        const Vec2 d = b->position - a->position;
        const f32 len = d.length();
        if (len < EPSILON) continue;
        const Vec2 n = d / len;
        const f32 err = len - c.rest_length;
        const f32 inv = a->inv_mass + b->inv_mass;
        if (inv <= EPSILON) continue;
        // Full correction in one step: the split by inverse mass only decides
        // *who* moves, so the pair lands on the rest length regardless of the
        // mass ratio.
        const f32 corr = err * c.stiffness;
        a->position += n * (corr * a->inv_mass / inv);
        b->position -= n * (corr * b->inv_mass / inv);
    }
    for (const RevoluteConstraint2D& c : m_revolute) {
        Body2D* a = body(c.a);
        Body2D* b = body(c.b);
        if (!a || !b) continue;
        const Vec2 ra = rotate_local(c.local_anchor_a, a->angle);
        const Vec2 rb = rotate_local(c.local_anchor_b, b->angle);
        const Vec2 wa = a->position + ra;
        const Vec2 wb = b->position + rb;
        const Vec2 C = wb - wa;
        const f32 im = a->inv_mass + b->inv_mass;
        const f32 k11 = im + ra.y * ra.y * a->inv_inertia + rb.y * rb.y * b->inv_inertia;
        const f32 k12 = -ra.x * ra.y * a->inv_inertia - rb.x * rb.y * b->inv_inertia;
        const f32 k22 = im + ra.x * ra.x * a->inv_inertia + rb.x * rb.x * b->inv_inertia;
        const f32 det = k11 * k22 - k12 * k12;
        if (det <= EPSILON) continue;
        // P = K^-1 (-C) drives the anchor error to zero in one pass; applied as
        // a pseudo-impulse so both the linear and the rotational degrees of
        // freedom absorb their share of the drift.
        const Vec2 P{(-k22 * C.x + k12 * C.y) / det,
                     (k12 * C.x - k11 * C.y) / det};
        a->position -= P * a->inv_mass;
        a->angle -= (ra.x * P.y - ra.y * P.x) * a->inv_inertia;
        b->position += P * b->inv_mass;
        b->angle += (rb.x * P.y - rb.y * P.x) * b->inv_inertia;
    }
}

void PhysicsWorld2D::step(f32 dt, u32 velocity_iterations, u32 position_iterations) {
    if (dt <= 0.0f) return;
    integrate_bodies(dt);
    find_contacts();
    prepare_contacts();
    for (u32 i = 0; i < velocity_iterations; ++i) {
        solve_constraints(dt);
        solve_velocities();
    }
    integrate_positions(dt);
    for (u32 i = 0; i < position_iterations; ++i) {
        find_contacts();
        solve_positions();
        solve_constraint_positions();
    }
    find_contacts();
}

BodyHandle PhysicsWorld2D::point_query(Vec2 world_point) const {
    for (u32 i = 0; i < static_cast<u32>(m_slots.size()); ++i) {
        if (!m_slots[i].alive) continue;
        if (m_slots[i].body.is_static) continue;
        if (point_in_body(m_slots[i].body, world_point)) {
            return BodyHandle{i, m_slots[i].generation};
        }
    }
    return {};
}

RaycastHit2D PhysicsWorld2D::raycast(Vec2 start, Vec2 dir, f32 max_distance) const {
    RaycastHit2D best;
    f32 best_t = max_distance;
    for (u32 i = 0; i < static_cast<u32>(m_slots.size()); ++i) {
        if (!m_slots[i].alive) continue;
        const Body2D& b = m_slots[i].body;
        f32 t = 0.0f;
        Vec2 n{};
        bool hit = false;
        if (b.shape.kind == Shape2D::Kind::Circle) {
            hit = ray_circle(start, dir, max_distance, b.position, b.shape.radius, t, n);
        } else {
            const Vec2 local_o = rotate_local(start - b.position, -b.angle);
            const Vec2 local_d = rotate_local(dir, -b.angle);
            Vec2 ln{};
            hit = ray_aabb_local(local_o, local_d, b.shape.half, max_distance, t, ln);
            n = rotate_local(ln, b.angle);
        }
        if (!hit || t > best_t) continue;
        best_t = t;
        best.hit = true;
        best.distance = t;
        best.point = start + dir * t;
        best.normal = n;
        best.body = BodyHandle{i, m_slots[i].generation};
    }
    return best;
}

} // namespace nf::scene2d
