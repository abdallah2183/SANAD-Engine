// NF/Networking/src/ConstraintReplicator.cpp — applies net events to a
// physics world (Phase 17).
//
// The applier is the second half of §371's first item. Phase 16 owned the
// bytes (codecs + a reliable-channel round trip); this is where those bytes
// meet a physics world through the IReplicatedWorld interface, so the
// networking module still links nothing but ws2_32.

#include <NF/Networking/ConstraintReplicator.hpp>

#include <cmath>
#include <utility>

namespace nf::net {

// --- ConstraintRegistry ---------------------------------------------------------

bool ConstraintRegistry::bind(u32 net_constraint_id, u32 constraint_handle) {
    if (net_constraint_id == 0 || constraint_handle == 0) return false;
    m_by_net[net_constraint_id] = constraint_handle;
    return true;
}

void ConstraintRegistry::unbind(u32 net_constraint_id) {
    m_by_net.erase(net_constraint_id);
}

u32 ConstraintRegistry::handle_of(u32 net_constraint_id) const {
    const auto it = m_by_net.find(net_constraint_id);
    return it == m_by_net.end() ? 0 : it->second;
}

// --- ConstraintApplier ----------------------------------------------------------

ConstraintApplier::ConstraintApplier(IReplicatedWorld& world) : m_world(world) {}

bool ConstraintApplier::endpoints_live(const ConstraintEvent& e) const {
    // Clone names its new endpoints directly; Spawn names its own. Both must
    // resolve, and a two-body constraint needs two DIFFERENT bodies.
    const u32 a = m_bodies.body_of(e.body_a);
    const u32 b = m_bodies.body_of(e.body_b);
    return a != 0 && b != 0 && a != b;
}

void ConstraintApplier::apply_constraint_event(const ConstraintEvent& e,
                                               ApplyResult& result) {
    // REMOVE FIRST, and it is the one op that needs no endpoints: the wire
    // carries the net id alone, and what resolves it is the registry, not the
    // bodies (they may already be gone). It must also come BEFORE the
    // duplicate check below — an applied joint is by definition a known id,
    // so ordering the other way made every Remove look like a resent Spawn
    // and silently swallowed the joint teardown.
    if (e.op == ConstraintOp::Remove) {
        const u32 handle = m_constraints.handle_of(e.net_id);
        // A Remove for an id this side never applied is a resent reliable
        // packet, not a physics refusal: failed_events means the world said
        // no, and the world was never asked.
        if (handle == 0) {
            result.duplicate_events++;
            return;
        }
        m_world.remove_constraint(handle);
        m_constraints.unbind(e.net_id);
        m_joint_bodies.erase(e.net_id);
        result.constraints_removed++;
        return;
    }

    // AUTHORITY: the server assigns every net constraint id. A repeat for an
    // applied id is a resent reliable packet — ignore it, exactly the
    // idempotency the ghost client applies to snapshots.
    if (m_constraints.has(e.net_id)) {
        result.duplicate_events++;
        return;
    }

    // ORDER: bodies before their joints. A joint whose endpoints have not
    // arrived yet is queued — the reliable channel delivers in send order,
    // so the body event is always moments behind.
    if (!endpoints_live(e)) {
        m_pending.push_back(e);
        result.deferred_events++;
        return;
    }

    const u32 a = m_bodies.body_of(e.body_a);
    const u32 b = m_bodies.body_of(e.body_b);

    u32 handle = 0;
    if (e.op == ConstraintOp::Clone) {
        // The template joint is looked up by the same net ids the server
        // assigned — this is "clone_constraint over the wire" (§371/§349).
        const u32 source = m_constraints.handle_of(e.source);
        if (source == 0) {
            result.failed_events++;
            return;
        }
        handle = m_world.clone_constraint(source, a, b);
    } else {
        switch (e.kind) {
        case ConstraintKind::Fixed:
            handle = m_world.add_fixed(a, b);
            break;
        case ConstraintKind::Point:
            handle = m_world.add_point(a, b, e.point[0], e.point[1], e.point[2]);
            break;
        case ConstraintKind::Hinge:
            handle = m_world.add_hinge(a, b, e.point[0], e.point[1], e.point[2],
                                       e.axis[0], e.axis[1], e.axis[2]);
            break;
        case ConstraintKind::Slider:
            handle = m_world.add_slider(a, b, e.point[0], e.point[1], e.point[2],
                                        e.axis[0], e.axis[1], e.axis[2]);
            break;
        case ConstraintKind::Distance:
            handle = m_world.add_distance(a, b, e.point[0], e.point[1], e.point[2],
                                          e.anchor_b[0], e.anchor_b[1], e.anchor_b[2],
                                          e.param0, e.param1);
            break;
        case ConstraintKind::Cone:
            handle = m_world.add_cone(a, b, e.point[0], e.point[1], e.point[2],
                                      e.axis[0], e.axis[1], e.axis[2], e.param0);
            break;
        case ConstraintKind::SixDOF:
            handle = m_world.add_sixdof(a, b, e.point[0], e.point[1], e.point[2],
                                        e.limits_min, e.limits_max);
            break;
        }
    }

    if (handle == 0) {
        result.failed_events++;
        return;
    }
    m_constraints.bind(e.net_id, handle);
    m_joint_bodies[e.net_id] = {e.body_a, e.body_b};
    if (e.op == ConstraintOp::Clone) result.constraints_cloned++;
    else result.constraints_spawned++;
}

void ConstraintApplier::remove_joints_on_body(u32 net_body, ApplyResult& result) {
    for (auto it = m_joint_bodies.begin(); it != m_joint_bodies.end();) {
        if (it->second.first != net_body && it->second.second != net_body) {
            ++it;
            continue;
        }
        const u32 handle = m_constraints.handle_of(it->first);
        if (handle != 0) m_world.remove_constraint(handle);
        m_constraints.unbind(it->first);
        it = m_joint_bodies.erase(it);
        result.constraints_removed++;
    }
}

bool ConstraintApplier::apply_body(const u8* data, usize size, ApplyResult& result,
                                   std::string& err) {
    BodyEvent ev;
    if (!decode_body_event(data, size, ev, err)) return false;

    if (ev.op == BodyOp::Remove) {
        // Joints first: they reference the exact body going away, and the
        // world must drop them while it is still alive to drop.
        remove_joints_on_body(ev.net_id, result);
        const u32 handle = m_bodies.body_of(ev.net_id);
        if (handle != 0) m_world.remove_body(handle);
        m_bodies.unbind(ev.net_id);
        result.bodies_removed++;
        return true;
    }

    // A Spawn for an id already held: the reliable channel means the sender's
    // intent is real, but the HIERARCHY PRIORITY table decides who owns the
    // id — a Static holder is scenery and is never evicted by a re-spawn.
    BodyRegistryEntry current{};
    const bool held = m_bodies.entry(ev.net_id, current);
    if (held && current.is_static) {
        result.duplicate_events++;
        return true;
    }
    const u32 handle = m_world.add_body_sphere(
        ev.type, ev.radius, ev.x, ev.y, ev.z, ev.mass, ev.friction,
        ev.restitution, ev.linear_damping, ev.angular_damping,
        ev.allow_sleep != 0);
    if (handle == 0) {
        result.failed_events++;
        return true;
    }

    if (held) {
        // A re-spawned body is a new object: joints on the stale one die with
        // it (joints first, then the body), and the sender re-spawns the
        // joints it still wants.
        remove_joints_on_body(ev.net_id, result);
        m_world.remove_body(current.body_handle); // replace the stale body
    }
    m_bodies.bind(ev.net_id, handle, ev.type == 0 /* Static = scenery */);
    result.bodies_spawned++;
    return true;
}

bool ConstraintApplier::apply_constraint(const u8* data, usize size,
                                         ApplyResult& result, std::string& err) {
    ConstraintEvent ev;
    if (!decode_constraint_event(data, size, ev, err)) return false;
    apply_constraint_event(ev, result);
    return true;
}

void ConstraintApplier::flush_pending(ApplyResult& result) {
    // Retry every deferred joint whose endpoints have since bound. A joint
    // whose body NEVER arrives keeps reporting through pending_count().
    for (usize i = 0; i < m_pending.size();) {
        if (endpoints_live(m_pending[i])) {
            const ConstraintEvent e = std::move(m_pending[i]);
            m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(i));
            apply_constraint_event(e, result);
        } else {
            ++i;
        }
    }
}

} // namespace nf::net
