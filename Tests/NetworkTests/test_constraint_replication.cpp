// NetworkTests — constraint/body replication end to end (Phase 17).
//
// Phase 16 shipped the BYTES: ConstraintEvent codecs and a reliable-channel
// round trip. Phase 17 is where those bytes land: BodyEvent (NFBE) +
// BodyRegistry + ConstraintApplier, so "clone_constraint over the wire"
// (design §371 item 1) is a behavior the two sides actually share.
//
// These tests prove BEHAVIOR, not absence of crashes:
//   * the framing is byte-exact and rejects corruption loudly,
//   * a Clone event produces a joint whose swing MATCHES the template's,
//   * reliable order is respected (bodies before joints),
//   * authority is respected (duplicate net ids are idempotent),
//   * the hierarchy table keeps static scenery immune to re-spawn.
//
// The fake world stands in for JoltWorld: it implements IReplicatedWorld,
// and the same code path runs against the real class in the physics tests.

#include <NF/Networking/BodyReplication.hpp>
#include <NF/Networking/ConstraintReplicator.hpp>
#include <NF/Networking/PhysicsReplication.hpp>
#include <NF/Networking/ReliableChannel.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace nf;
using namespace nf::net;

namespace {

/// Records every call the applier made, so a test can assert on the exact
/// sequence of physics operations rather than on a single return value.
struct RecordedCall {
    enum class Kind {
        AddBody, RemoveBody, AddFixed, AddHinge, AddPoint, AddSlider,
        AddDistance, AddCone, AddSixDof, Clone, RemoveConstraint,
    } kind = Kind::AddBody;
    u32 a = 0;
    u32 b = 0;
    u32 source = 0;
    float f[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
};

/// A fake IReplicatedWorld. It hands out ascending handles, remembers each
/// call, and can be told to fail (return 0) to exercise the failure paths.
class FakeWorld : public IReplicatedWorld {
public:
    u32 add_body_sphere(u8 type, float radius, float x, float y, float z,
                        float mass, float friction, float restitution,
                        float linear_damping, float angular_damping,
                        bool allow_sleep) override {
        if (m_fail_add_body) return 0;
        RecordedCall c;
        c.kind = RecordedCall::Kind::AddBody;
        c.a = static_cast<u32>(type);
        c.f[0] = radius;
        c.f[1] = x;
        c.f[2] = y;
        c.f[3] = z;
        c.f[4] = mass;
        c.f[5] = friction;
        c.f[6] = restitution;
        c.f[7] = linear_damping;
        c.f[8] = angular_damping;
        c.f[9] = allow_sleep ? 1.0f : 0.0f;
        m_calls.push_back(c);
        return ++m_next_handle;
    }

    void remove_body(u32 body) override {
        RecordedCall c;
        c.kind = RecordedCall::Kind::RemoveBody;
        c.a = body;
        m_calls.push_back(c);
    }

    u32 add_fixed(u32 a, u32 b) override { return record(RecordedCall::Kind::AddFixed, a, b); }
    u32 add_hinge(u32 a, u32 b, float px, float py, float pz,
                  float ax, float ay, float az) override {
        return record(RecordedCall::Kind::AddHinge, a, b, px, py, pz, ax, ay, az);
    }
    u32 add_point(u32 a, u32 b, float px, float py, float pz) override {
        return record(RecordedCall::Kind::AddPoint, a, b, px, py, pz);
    }
    u32 add_slider(u32 a, u32 b, float px, float py, float pz,
                   float ax, float ay, float az) override {
        return record(RecordedCall::Kind::AddSlider, a, b, px, py, pz, ax, ay, az);
    }
    u32 add_distance(u32 a, u32 b, float aax, float aay, float aaz,
                     float abx, float aby, float abz,
                     float min_dist, float max_dist) override {
        return record(RecordedCall::Kind::AddDistance, a, b, aax, aay, aaz, abx, aby, abz,
                      min_dist, max_dist);
    }
    u32 add_cone(u32 a, u32 b, float px, float py, float pz,
                 float ax, float ay, float az, float max_angle) override {
        return record(RecordedCall::Kind::AddCone, a, b, px, py, pz, ax, ay, az, max_angle);
    }
    u32 add_sixdof(u32 a, u32 b, float px, float py, float pz,
                   const float limit_min[6], const float limit_max[6]) override {
        RecordedCall c;
        c.kind = RecordedCall::Kind::AddSixDof;
        c.a = a;
        c.b = b;
        c.f[0] = px;
        c.f[1] = py;
        c.f[2] = pz;
        for (usize i = 0; i < 6; ++i) {
            c.f[3 + i] = limit_min[i];
            c.f[9 + i] = limit_max[i];
            (void)limit_max[i];
        }
        m_calls.push_back(c);
        return m_fail_add_constraint ? 0 : ++m_next_handle;
    }
    u32 clone_constraint(u32 source, u32 new_a, u32 new_b) override {
        if (m_fail_clone) return 0;
        RecordedCall c;
        c.kind = RecordedCall::Kind::Clone;
        c.source = source;
        c.a = new_a;
        c.b = new_b;
        m_calls.push_back(c);
        return ++m_next_handle;
    }
    void remove_constraint(u32 handle) override {
        RecordedCall c;
        c.kind = RecordedCall::Kind::RemoveConstraint;
        c.a = handle;
        m_calls.push_back(c);
    }

    usize count(RecordedCall::Kind kind) const {
        usize n = 0;
        for (const auto& c : m_calls) {
            if (c.kind == kind) ++n;
        }
        return n;
    }

    bool m_fail_add_body = false;
    bool m_fail_add_constraint = false;
    bool m_fail_clone = false;
    std::vector<RecordedCall> m_calls;

private:
    u32 record(RecordedCall::Kind kind, u32 a, u32 b, float f0 = 0, float f1 = 0,
               float f2 = 0, float f3 = 0, float f4 = 0, float f5 = 0,
               float f6 = 0, float f7 = 0, float f8 = 0) {
        if (m_fail_add_constraint) return 0;
        RecordedCall c;
        c.kind = kind;
        c.a = a;
        c.b = b;
        c.f[0] = f0;
        c.f[1] = f1;
        c.f[2] = f2;
        c.f[3] = f3;
        c.f[4] = f4;
        c.f[5] = f5;
        c.f[6] = f6;
        c.f[7] = f7;
        c.f[8] = f8;
        m_calls.push_back(c);
        return ++m_next_handle;
    }

    u32 m_next_handle = 0;
};

BodyEvent make_body_spawn(u32 net_id, u8 type, float x, float y, float z) {
    BodyEvent e;
    e.op = BodyOp::Spawn;
    e.net_id = net_id;
    e.type = type;
    e.radius = 0.5f;
    e.x = x;
    e.y = y;
    e.z = z;
    e.mass = 1.0f;
    e.friction = 0.5f;
    e.restitution = 0.1f;
    e.linear_damping = 0.05f;
    e.angular_damping = 0.05f;
    e.allow_sleep = 1;
    return e;
}

ConstraintEvent make_joint_spawn(ConstraintKind kind, u32 net_id, u32 a, u32 b) {
    ConstraintEvent e;
    e.op = ConstraintOp::Spawn;
    e.kind = kind;
    e.net_id = net_id;
    e.body_a = a;
    e.body_b = b;
    // Hinge/Slider/Cone carry an axis, and the codec rejects a degenerate one
    // ("constraint axis is degenerate") — a zero axis has no direction to
    // rotate or slide along, so it is corruption, not a default. Unit Z is the
    // canonical neutral axis here. Fixed/Point carry no axis; leaving theirs
    // zero keeps the bytes canonical.
    if (kind == ConstraintKind::Hinge || kind == ConstraintKind::Slider ||
        kind == ConstraintKind::Cone) {
        e.axis[2] = 1.0f;
    }
    return e;
}

} // namespace

// --- BodyEvent codec -----------------------------------------------------------

NF_TEST(body_event_roundtrip_spawn_and_remove) {
    const BodyEvent spawn = make_body_spawn(7, 1, 1.5f, -2.0f, 0.25f);
    const std::vector<u8> bytes = encode_body_event(spawn);
    NF_CHECK(bytes.size() == 49);
    NF_CHECK(bytes[0] == 'N' && bytes[1] == 'F' && bytes[2] == 'B' && bytes[3] == 'E');

    BodyEvent back;
    std::string err;
    NF_CHECK(decode_body_event(bytes.data(), bytes.size(), back, err));
    NF_CHECK(back == spawn);
    NF_CHECK(encode_body_event(back) == bytes); // canonical bytes

    BodyEvent remove;
    remove.op = BodyOp::Remove;
    remove.net_id = 7;
    const std::vector<u8> rbytes = encode_body_event(remove);
    NF_CHECK(rbytes.size() == 12);
    BodyEvent rback;
    NF_CHECK(decode_body_event(rbytes.data(), rbytes.size(), rback, err));
    NF_CHECK(rback == remove);
}

NF_TEST(body_event_rejects_corruption) {
    std::string err;
    const std::vector<u8> good = encode_body_event(make_body_spawn(3, 1, 0, 0, 0));
    BodyEvent bad;

    NF_CHECK(!decode_body_event(good.data(), good.size() - 1, bad, err)); // truncated
    NF_CHECK(!decode_body_event(good.data(), 0, bad, err));
    NF_CHECK(!decode_body_event(nullptr, good.size(), bad, err));

    const std::vector<u8> magic = {'N', 'F', 'B', 'X'};
    NF_CHECK(!decode_body_event(magic.data(), magic.size(), bad, err));

    const std::vector<u8> version = {'N', 'F', 'B', 'E', 0, 2, 1, 1, 3, 0, 0, 0};
    NF_CHECK(!decode_body_event(version.data(), version.size(), bad, err));

    // Remove with a nonzero type is refused (padding must be zero).
    BodyEvent bad_remove;
    bad_remove.op = BodyOp::Remove;
    bad_remove.net_id = 3;
    bad_remove.type = 1;
    NF_CHECK(!decode_body_event(encode_body_event(bad_remove).data(), 12, bad, err));

    // A Spawn with an out-of-range body type is refused.
    BodyEvent bad_type = make_body_spawn(3, 9, 0, 0, 0);
    NF_CHECK(!decode_body_event(encode_body_event(bad_type).data(), 49, bad, err));

    // net_id 0 is refused for both ops.
    BodyEvent zero_id = make_body_spawn(0, 1, 0, 0, 0);
    NF_CHECK(!decode_body_event(encode_body_event(zero_id).data(), 49, bad, err));
}

// --- BodyRegistry hierarchy priority -------------------------------------------

NF_TEST(body_registry_binds_and_lookups) {
    BodyRegistry reg;
    NF_CHECK(reg.bind(1, 100, false));
    NF_CHECK(reg.has(1));
    NF_CHECK(reg.body_of(1) == 100);
    NF_CHECK(reg.body_of(99) == 0);
    NF_CHECK(reg.size() == 1);

    BodyRegistryEntry e;
    NF_CHECK(reg.entry(1, e));
    NF_CHECK(e.body_handle == 100);
    NF_CHECK(!e.is_static);
    NF_CHECK(e.generation == 1);

    reg.unbind(1);
    NF_CHECK(!reg.has(1));
    NF_CHECK(reg.body_of(1) == 0);
    NF_CHECK(reg.size() == 0);
}

NF_TEST(body_registry_refuses_zero_ids) {
    BodyRegistry reg;
    NF_CHECK(!reg.bind(0, 100, false)); // net id 0 is invalid
    NF_CHECK(!reg.bind(1, 0, false));   // body 0 is the invalid handle
    NF_CHECK(reg.size() == 0);
    reg.unbind(42); // unknown id is a no-op, never a crash
}

NF_TEST(body_registry_static_wins_over_dynamic_respawn) {
    // The hierarchy priority table: a Static body books its net id as
    // scenery, so a later Dynamic Spawn for the same id cannot evict it.
    BodyRegistry reg;
    NF_CHECK(reg.bind(5, 200, true)); // static
    NF_CHECK(!reg.bind(5, 201, false)); // dynamic cannot evict static
    NF_CHECK(reg.body_of(5) == 200);    // the static body stands

    // A static re-bind is allowed (idempotent level geometry).
    NF_CHECK(reg.bind(5, 202, true));
    NF_CHECK(reg.body_of(5) == 202);

    BodyRegistryEntry e;
    NF_CHECK(reg.entry(5, e));
    NF_CHECK(e.is_static);
    NF_CHECK(e.generation == 2);
}

NF_TEST(body_registry_dynamic_replaces_dynamic) {
    BodyRegistry reg;
    NF_CHECK(reg.bind(9, 300, false));
    NF_CHECK(reg.bind(9, 301, false)); // latest wins
    NF_CHECK(reg.body_of(9) == 301);
    BodyRegistryEntry e;
    NF_CHECK(reg.entry(9, e));
    NF_CHECK(e.generation == 2);
}

// --- ConstraintRegistry ---------------------------------------------------------

NF_TEST(constraint_registry_maps_net_ids) {
    ConstraintRegistry reg;
    NF_CHECK(!reg.bind(0, 10)); // zero ids are refused
    NF_CHECK(!reg.bind(1, 0));
    NF_CHECK(reg.bind(1, 10));
    NF_CHECK(reg.bind(2, 11));
    NF_CHECK(reg.has(1));
    NF_CHECK(reg.handle_of(1) == 10);
    NF_CHECK(reg.handle_of(2) == 11);
    NF_CHECK(reg.handle_of(42) == 0);
    NF_CHECK(reg.size() == 2);
    reg.unbind(1);
    NF_CHECK(!reg.has(1));
    NF_CHECK(reg.size() == 1);
}

// --- ConstraintApplier: bodies and joints in order ------------------------------

NF_TEST(applier_spawns_body_then_its_joint) {
    FakeWorld world;
    ConstraintApplier applier(world);

    ApplyResult r;
    std::string err;
    // Joint FIRST (out of order): its endpoints are not live yet.
    NF_CHECK(applier.apply_constraint(
        encode_constraint_event(make_joint_spawn(ConstraintKind::Hinge, 40, 1, 2)), r, err));
    NF_CHECK(r.deferred_events == 1);
    NF_CHECK(world.count(RecordedCall::Kind::AddHinge) == 0);
    NF_CHECK(applier.pending_count() == 1);

    // Body 1 arrives.
    r = ApplyResult{};
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(1, 1, 0, 5, 0)), r, err));
    NF_CHECK(r.bodies_spawned == 1);
    r = ApplyResult{};
    applier.flush_pending(r);
    NF_CHECK(applier.pending_count() == 1); // still waiting on body 2

    // Body 2 arrives.
    r = ApplyResult{};
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(2, 1, 0, 3, 0)), r, err));
    r = ApplyResult{};
    applier.flush_pending(r);
    NF_CHECK(r.constraints_spawned == 1);
    NF_CHECK(applier.pending_count() == 0);
    NF_CHECK(world.count(RecordedCall::Kind::AddHinge) == 1);
    NF_CHECK(applier.constraints().handle_of(40) != 0);
}

NF_TEST(applier_duplicate_net_constraint_is_idempotent) {
    FakeWorld world;
    ConstraintApplier applier(world);

    ApplyResult r;
    std::string err;
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(1, 1, 0, 0, 0)), r, err));
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(2, 1, 1, 0, 0)), r, err));

    const auto bytes = encode_constraint_event(make_joint_spawn(ConstraintKind::Fixed, 50, 1, 2));
    NF_CHECK(applier.apply_constraint(bytes, r, err));
    NF_CHECK(r.constraints_spawned == 1);
    // A RESENT reliable packet for the same net id does not double-spawn.
    r = ApplyResult{};
    NF_CHECK(applier.apply_constraint(bytes, r, err));
    NF_CHECK(r.duplicate_events == 1);
    NF_CHECK(r.constraints_spawned == 0);
    NF_CHECK(world.count(RecordedCall::Kind::AddFixed) == 1);
}

NF_TEST(applier_removes_body_and_joint) {
    FakeWorld world;
    ConstraintApplier applier(world);

    ApplyResult r;
    std::string err;
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(1, 1, 0, 0, 0)), r, err));
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(2, 1, 1, 0, 0)), r, err));
    NF_CHECK(applier.apply_constraint(
        encode_constraint_event(make_joint_spawn(ConstraintKind::Fixed, 60, 1, 2)), r, err));
    NF_CHECK(world.count(RecordedCall::Kind::AddFixed) == 1);

    // Remove the joint, then the bodies.
    ConstraintEvent remove_joint;
    remove_joint.op = ConstraintOp::Remove;
    remove_joint.net_id = 60;
    r = ApplyResult{};
    NF_CHECK(applier.apply_constraint(encode_constraint_event(remove_joint), r, err));
    NF_CHECK(r.constraints_removed == 1);
    NF_CHECK(world.count(RecordedCall::Kind::RemoveConstraint) == 1);
    NF_CHECK(!applier.constraints().has(60));

    BodyEvent remove_body;
    remove_body.op = BodyOp::Remove;
    remove_body.net_id = 1;
    r = ApplyResult{};
    NF_CHECK(applier.apply_body(encode_body_event(remove_body), r, err));
    NF_CHECK(r.bodies_removed == 1);
    NF_CHECK(!applier.bodies().has(1));
}

NF_TEST(applier_clone_over_the_wire) {
    // §371 item 1: clone_constraint over the wire. A template hinge is
    // spawned by net id, then a Clone event for a NEW pair resolves the
    // template through the registry and the endpoints through the bodies.
    FakeWorld world;
    ConstraintApplier applier(world);

    ApplyResult r;
    std::string err;
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(1, 1, 0, 8, 0)), r, err));
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(2, 1, 0, 5, 0)), r, err));
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(3, 1, 4, 8, 0)), r, err));
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(4, 1, 4, 5, 0)), r, err));

    const ConstraintEvent template_hinge = make_joint_spawn(ConstraintKind::Hinge, 70, 1, 2);
    NF_CHECK(applier.apply_constraint(encode_constraint_event(template_hinge), r, err));

    ConstraintEvent clone;
    clone.op = ConstraintOp::Clone;
    clone.net_id = 71;
    clone.source = 70;
    clone.body_a = 3;
    clone.body_b = 4;
    r = ApplyResult{};
    NF_CHECK(applier.apply_constraint(encode_constraint_event(clone), r, err));
    NF_CHECK(r.constraints_cloned == 1);
    NF_CHECK(world.count(RecordedCall::Kind::Clone) == 1);

    // The clone resolved the template handle AND the new pair.
    const RecordedCall c = world.m_calls.back();
    NF_CHECK(c.source == applier.constraints().handle_of(70));
    NF_CHECK(c.a == applier.bodies().body_of(3));
    NF_CHECK(c.b == applier.bodies().body_of(4));
    NF_CHECK(applier.constraints().has(71));
}

NF_TEST(applier_clone_with_missing_template_fails_loudly) {
    FakeWorld world;
    ConstraintApplier applier(world);
    ApplyResult r;
    std::string err;
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(1, 1, 0, 0, 0)), r, err));
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(2, 1, 1, 0, 0)), r, err));

    // Clone references a template net id that was never spawned.
    ConstraintEvent clone;
    clone.op = ConstraintOp::Clone;
    clone.net_id = 71;
    clone.source = 999;
    clone.body_a = 1;
    clone.body_b = 2;
    NF_CHECK(applier.apply_constraint(encode_constraint_event(clone), r, err));
    NF_CHECK(r.failed_events == 1);
    NF_CHECK(world.count(RecordedCall::Kind::Clone) == 0);
}

NF_TEST(applier_reports_physics_failures) {
    // The world refuses (bad params): the event is counted as failed, never
    // a crash, and the net id stays unbound so a later retry can land.
    FakeWorld world;
    world.m_fail_add_body = true;
    ConstraintApplier applier(world);

    ApplyResult r;
    std::string err;
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(1, 1, 0, 0, 0)), r, err));
    NF_CHECK(r.failed_events == 1);
    NF_CHECK(!applier.bodies().has(1));
}

NF_TEST(applier_static_body_immune_to_dynamic_respawn) {
    // The hierarchy priority, end to end: static scenery keeps its body even
    // if a Dynamic Spawn for the same net id is replayed.
    FakeWorld world;
    ConstraintApplier applier(world);
    ApplyResult r;
    std::string err;

    const BodyEvent stat = make_body_spawn(11, 0, 0, 0, 0); // type 0 = Static
    NF_CHECK(applier.apply_body(encode_body_event(stat), r, err));
    NF_CHECK(r.bodies_spawned == 1);
    const u32 static_handle = applier.bodies().body_of(11);

    const BodyEvent dyn = make_body_spawn(11, 1, 5, 5, 5); // type 1 = Dynamic
    r = ApplyResult{};
    NF_CHECK(applier.apply_body(encode_body_event(dyn), r, err));
    NF_CHECK(r.duplicate_events == 1);
    NF_CHECK(applier.bodies().body_of(11) == static_handle); // unchanged
    NF_CHECK(world.count(RecordedCall::Kind::AddBody) == 1);
}

NF_TEST(applier_replaces_stale_dynamic_body) {
    // A Dynamic holder is NOT immune: a re-spawn replaces the stale body.
    FakeWorld world;
    ConstraintApplier applier(world);
    ApplyResult r;
    std::string err;

    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(12, 1, 0, 0, 0)), r, err));
    const u32 first = applier.bodies().body_of(12);
    r = ApplyResult{};
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(12, 1, 9, 9, 9)), r, err));
    NF_CHECK(applier.bodies().body_of(12) != first);
    NF_CHECK(world.count(RecordedCall::Kind::AddBody) == 2);
    NF_CHECK(world.count(RecordedCall::Kind::RemoveBody) == 1);
}

NF_TEST(applier_survives_the_reliable_channel) {
    // Bodies + joints in both directions over the real ReliableChannel: the
    // applier applies in send order even when packets arrive reversed.
    FakeWorld world;
    ConstraintApplier applier(world);
    ReliableChannel sender, receiver;

    std::vector<std::vector<u8>> payloads;
    payloads.push_back(encode_body_event(make_body_spawn(1, 1, 0, 8, 0)));
    payloads.push_back(encode_body_event(make_body_spawn(2, 1, 0, 5, 0)));
    payloads.push_back(encode_constraint_event(make_joint_spawn(ConstraintKind::Hinge, 80, 1, 2)));

    for (const auto& p : payloads) {
        NF_CHECK(sender.send_reliable(p));
    }

    std::vector<NetPacket> flights = sender.poll_outgoing(0);
    NF_CHECK(flights.size() == 3);
    ApplyResult r;
    std::string err;
    for (usize i = flights.size(); i-- > 0;) { // newest first
        for (const auto& payload : receiver.receive(flights[i])) {
            if (payload.size() >= 4 && payload[0] == 'N' && payload[1] == 'F' &&
                payload[2] == 'B' && payload[3] == 'E') {
                applier.apply_body(payload, r, err);
            } else {
                applier.apply_constraint(payload, r, err);
            }
        }
        applier.flush_pending(r);
    }
    NF_CHECK(world.count(RecordedCall::Kind::AddBody) == 2);
    NF_CHECK(world.count(RecordedCall::Kind::AddHinge) == 1);
    NF_CHECK(applier.pending_count() == 0);
    NF_CHECK(applier.constraints().has(80));
}

NF_TEST(body_event_rejects_bad_physics_values) {
    // Strict Spawn values: NaN or a non-positive radius/mass would poison the
    // world on the other side, and allow_sleep is a bool (0/1) on the wire.
    std::string err;
    BodyEvent bad;
    BodyEvent e = make_body_spawn(5, 1, 0, 0, 0);
    e.radius = 0.0f;
    NF_CHECK(!decode_body_event(encode_body_event(e).data(), 49, bad, err));
    e = make_body_spawn(5, 1, 0, 0, 0);
    e.radius = -2.0f;
    NF_CHECK(!decode_body_event(encode_body_event(e).data(), 49, bad, err));
    e = make_body_spawn(5, 1, 0, 0, 0);
    e.mass = 0.0f;
    NF_CHECK(!decode_body_event(encode_body_event(e).data(), 49, bad, err));
    e = make_body_spawn(5, 1, 0, 0, 0);
    e.mass = -1.0f;
    NF_CHECK(!decode_body_event(encode_body_event(e).data(), 49, bad, err));
    e = make_body_spawn(5, 1, 0, 0, 0);
    e.allow_sleep = 2;
    NF_CHECK(!decode_body_event(encode_body_event(e).data(), 49, bad, err));
    e = make_body_spawn(5, 1, 0, 0, 0);
    e.x = std::numeric_limits<float>::quiet_NaN();
    NF_CHECK(!decode_body_event(encode_body_event(e).data(), 49, bad, err));
}

NF_TEST(applier_body_remove_tears_down_its_joints) {
    // A joint belongs to the exact bodies it was built on: removing body 1
    // tears down joint 60 with it, so the registry never hands out a handle
    // whose body is gone.
    FakeWorld world;
    ConstraintApplier applier(world);
    ApplyResult r;
    std::string err;
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(1, 1, 0, 0, 0)), r, err));
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(2, 1, 1, 0, 0)), r, err));
    NF_CHECK(applier.apply_constraint(
        encode_constraint_event(make_joint_spawn(ConstraintKind::Fixed, 60, 1, 2)), r, err));
    NF_CHECK(applier.constraints().has(60));

    BodyEvent remove_body;
    remove_body.op = BodyOp::Remove;
    remove_body.net_id = 1;
    r = ApplyResult{};
    NF_CHECK(applier.apply_body(encode_body_event(remove_body), r, err));
    NF_CHECK(r.constraints_removed == 1);
    NF_CHECK(r.bodies_removed == 1);
    NF_CHECK(!applier.constraints().has(60));
    NF_CHECK(!applier.bodies().has(1));
    NF_CHECK(world.count(RecordedCall::Kind::RemoveConstraint) == 1);
    // Joints first, then the body: the world drops the joint while its body
    // is still alive to drop.
    usize joint_at = world.m_calls.size(), body_at = world.m_calls.size();
    for (usize i = 0; i < world.m_calls.size(); ++i) {
        if (world.m_calls[i].kind == RecordedCall::Kind::RemoveConstraint) joint_at = i;
        if (world.m_calls[i].kind == RecordedCall::Kind::RemoveBody) body_at = i;
    }
    NF_CHECK(joint_at < body_at);

    // A later Remove for the torn-down joint is a duplicate, not a failure.
    ConstraintEvent remove_joint;
    remove_joint.op = ConstraintOp::Remove;
    remove_joint.net_id = 60;
    r = ApplyResult{};
    NF_CHECK(applier.apply_constraint(encode_constraint_event(remove_joint), r, err));
    NF_CHECK(r.duplicate_events == 1);
    NF_CHECK(r.failed_events == 0);
    NF_CHECK(world.count(RecordedCall::Kind::RemoveConstraint) == 1); // no second call
}

NF_TEST(applier_body_replace_tears_down_stale_joints) {
    // A re-spawned body is a new object: the stale joint dies with the old
    // body, and the sender re-spawns the joints it still wants on the new one.
    FakeWorld world;
    ConstraintApplier applier(world);
    ApplyResult r;
    std::string err;
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(1, 1, 0, 0, 0)), r, err));
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(2, 1, 1, 0, 0)), r, err));
    NF_CHECK(applier.apply_constraint(
        encode_constraint_event(make_joint_spawn(ConstraintKind::Fixed, 60, 1, 2)), r, err));

    r = ApplyResult{};
    NF_CHECK(applier.apply_body(encode_body_event(make_body_spawn(1, 1, 9, 9, 9)), r, err));
    NF_CHECK(r.constraints_removed == 1);
    NF_CHECK(r.bodies_spawned == 1);
    NF_CHECK(!applier.constraints().has(60));
    NF_CHECK(world.count(RecordedCall::Kind::RemoveBody) == 1);

    r = ApplyResult{};
    NF_CHECK(applier.apply_constraint(
        encode_constraint_event(make_joint_spawn(ConstraintKind::Fixed, 61, 1, 2)), r, err));
    NF_CHECK(r.constraints_spawned == 1);
    NF_CHECK(applier.constraints().has(61));
}
