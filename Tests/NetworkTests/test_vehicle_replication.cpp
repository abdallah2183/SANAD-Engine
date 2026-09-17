// NetworkTests — vehicle/constraint replication (Phase 16: net physics).
//
// Codecs first (exact-size framing, loud corruption failures), then the
// server input queue and client ghosts as pure logic, then the full stack
// over real UDP loopback: drive inputs ride the reliable channel up,
// vehicle snapshots return as raw datagrams, exactly like the player path.

#include <NF/Networking/PhysicsReplication.hpp>
#include <NF/Networking/ReliableChannel.hpp>
#include <NF/Networking/Socket.hpp>
#include <NF/Test/TestFramework.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace nf;
using namespace nf::net;

namespace {

VehicleDriveInput make_drive(u32 tick, u32 entity, float throttle, float steer, float brake) {
    VehicleDriveInput in;
    in.tick = tick;
    in.entity = entity;
    in.throttle = throttle;
    in.steer = steer;
    in.brake = brake;
    return in;
}

ConstraintEvent make_spawn(ConstraintKind kind, u32 net_id, u32 a, u32 b) {
    ConstraintEvent e;
    e.op = ConstraintOp::Spawn;
    e.kind = kind;
    e.net_id = net_id;
    e.body_a = a;
    e.body_b = b;
    return e;
}

} // namespace

// --- drive input codec --------------------------------------------------------

NF_TEST(drive_input_codec_roundtrip) {
    const VehicleDriveInput in = make_drive(4242, 7, 0.75f, -0.5f, 0.25f);
    const std::vector<u8> bytes = encode_drive_input(in);
    NF_CHECK(bytes.size() == 20);
    VehicleDriveInput back;
    std::string err;
    NF_CHECK(decode_drive_input(bytes.data(), bytes.size(), back, err));
    NF_CHECK(back == in);

    // Deterministic bytes: same input, same wire.
    NF_CHECK(encode_drive_input(in) == bytes);

    VehicleDriveInput bad;
    NF_CHECK(!decode_drive_input(bytes.data(), 19, bad, err)); // truncated
    NF_CHECK(!decode_drive_input(bytes.data(), 0, bad, err));  // empty
    NF_CHECK(!decode_drive_input(nullptr, 20, bad, err));      // null
}

NF_TEST(drive_input_rejects_bad_values) {
    std::string err;
    VehicleDriveInput bad;
    // Zero entity: no vehicle to drive.
    NF_CHECK(!decode_drive_input(encode_drive_input(make_drive(1, 0, 0, 0, 0)).data(), 20,
                                 bad, err));
    // Out of arcade range (gameplay only ever writes [-1,1]/[0,1]).
    NF_CHECK(!decode_drive_input(encode_drive_input(make_drive(1, 5, 1.5f, 0, 0)).data(), 20,
                                 bad, err));
    NF_CHECK(!decode_drive_input(encode_drive_input(make_drive(1, 5, 0, -2.0f, 0)).data(), 20,
                                 bad, err));
    NF_CHECK(!decode_drive_input(encode_drive_input(make_drive(1, 5, 0, 0, 1.5f)).data(), 20,
                                 bad, err));
    NF_CHECK(!decode_drive_input(encode_drive_input(make_drive(1, 5, 0, 0, -0.5f)).data(), 20,
                                 bad, err));
    // NaN would poison the simulation.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    NF_CHECK(!decode_drive_input(encode_drive_input(make_drive(1, 5, nan, 0, 0)).data(), 20,
                                 bad, err));
    // Boundary values are legal.
    VehicleDriveInput edge;
    NF_CHECK(decode_drive_input(encode_drive_input(make_drive(9, 3, -1.0f, 1.0f, 1.0f)).data(),
                                20, edge, err));
    NF_CHECK(edge.throttle == -1.0f && edge.steer == 1.0f && edge.brake == 1.0f);
}

// --- vehicle snapshot codec ---------------------------------------------------

NF_TEST(vehicle_snapshot_codec_roundtrip) {
    VehicleSnapshot snap;
    snap.tick = 77;
    VehicleNetState b;
    b.entity = 9;
    b.x = 1.0f;
    b.vx = 3.0f;
    VehicleNetState a;
    a.entity = 4;
    a.x = -2.0f;
    a.vy = 1.5f;
    snap.vehicles.push_back(b); // out of order on purpose...
    snap.vehicles.push_back(a);
    const std::vector<u8> bytes = encode_vehicle_snapshot(snap);
    NF_CHECK(bytes.size() == 14 + 2 * 28);
    NF_CHECK(bytes[0] == 'N' && bytes[1] == 'F' && bytes[2] == 'V' && bytes[3] == 'S');

    VehicleSnapshot back;
    std::string err;
    NF_CHECK(decode_vehicle_snapshot(bytes.data(), bytes.size(), back, err));
    NF_CHECK(back.tick == 77);
    NF_CHECK(back.vehicles.size() == 2);
    NF_CHECK(back.vehicles[0].entity == 4); // ...sorted by entity on the wire
    NF_CHECK(back.vehicles[1].entity == 9);
    NF_CHECK(back.vehicles[1].vx == 3.0f);

    // Empty snapshot is legal (no vehicles replicated this tick).
    VehicleSnapshot empty;
    empty.tick = 5;
    VehicleSnapshot empty_back;
    NF_CHECK(decode_vehicle_snapshot(encode_vehicle_snapshot(empty).data(), 14, empty_back,
                                     err));
    NF_CHECK(empty_back.tick == 5 && empty_back.vehicles.empty());
}

NF_TEST(vehicle_snapshot_rejects_corruption) {
    VehicleSnapshot snap;
    snap.tick = 1;
    VehicleNetState v;
    v.entity = 2;
    v.x = 1.0f;
    snap.vehicles.push_back(v);
    const std::vector<u8> bytes = encode_vehicle_snapshot(snap);

    VehicleSnapshot bad;
    std::string err;
    NF_CHECK(!decode_vehicle_snapshot(bytes.data(), 13, bad, err)); // truncated header
    NF_CHECK(!decode_vehicle_snapshot(bytes.data(), bytes.size() - 1, bad, err)); // cut record
    NF_CHECK(!decode_vehicle_snapshot(nullptr, bytes.size(), bad, err));

    std::vector<u8> magic = bytes;
    magic[0] = 'X';
    NF_CHECK(!decode_vehicle_snapshot(magic.data(), magic.size(), bad, err));

    std::vector<u8> version = bytes;
    version[4] = 2; // version 1 expected
    NF_CHECK(!decode_vehicle_snapshot(version.data(), version.size(), bad, err));

    // Declared count disagrees with the byte count.
    std::vector<u8> count = bytes;
    count[10] = 9;
    NF_CHECK(!decode_vehicle_snapshot(count.data(), count.size(), bad, err));

    // Zero entity and NaN never land in a ghost.
    std::vector<u8> zero = bytes;
    for (int i = 0; i < 4; ++i) zero[14 + i] = 0;
    NF_CHECK(!decode_vehicle_snapshot(zero.data(), zero.size(), bad, err));
    VehicleNetState nan_v;
    nan_v.entity = 3;
    nan_v.vx = std::numeric_limits<float>::quiet_NaN();
    VehicleSnapshot nan_snap;
    nan_snap.vehicles.push_back(nan_v);
    const std::vector<u8> nan_bytes = encode_vehicle_snapshot(nan_snap);
    NF_CHECK(!decode_vehicle_snapshot(nan_bytes.data(), nan_bytes.size(), bad, err));
}

// --- constraint event codec -----------------------------------------------------

NF_TEST(constraint_event_roundtrip_all_kinds) {
    std::string err;
    // Fixed: ids only.
    ConstraintEvent fixed = make_spawn(ConstraintKind::Fixed, 100, 1, 2);
    // Point: anchor.
    ConstraintEvent point = make_spawn(ConstraintKind::Point, 101, 1, 2);
    point.point[0] = 1.0f;
    point.point[1] = 2.0f;
    point.point[2] = 3.0f;
    // Hinge: anchor + axis.
    ConstraintEvent hinge = make_spawn(ConstraintKind::Hinge, 102, 3, 4);
    hinge.point[1] = 1.5f;
    hinge.axis[2] = 1.0f;
    // Slider: anchor + axis.
    ConstraintEvent slider = make_spawn(ConstraintKind::Slider, 103, 5, 6);
    slider.point[0] = -1.0f;
    slider.axis[1] = 1.0f;
    // Distance: two anchors + min/max.
    ConstraintEvent dist = make_spawn(ConstraintKind::Distance, 104, 7, 8);
    dist.point[0] = 1.0f;
    dist.anchor_b[0] = 2.0f;
    dist.param0 = 0.5f;
    dist.param1 = 2.0f;
    // Cone: anchor + axis + angle.
    ConstraintEvent cone = make_spawn(ConstraintKind::Cone, 105, 9, 10);
    cone.point[2] = 0.5f;
    cone.axis[0] = 1.0f;
    cone.param0 = 0.6f;
    // SixDOF: center + 12 limits.
    ConstraintEvent six = make_spawn(ConstraintKind::SixDOF, 106, 11, 12);
    six.point[1] = 3.0f;
    for (int i = 0; i < 6; ++i) {
        six.limits_min[i] = -0.1f * static_cast<float>(i);
        six.limits_max[i] = 0.1f * static_cast<float>(i);
    }
    // Clone + remove.
    ConstraintEvent clone;
    clone.op = ConstraintOp::Clone;
    clone.net_id = 200;
    clone.source = 102;
    clone.body_a = 13;
    clone.body_b = 14;
    ConstraintEvent remove;
    remove.op = ConstraintOp::Remove;
    remove.net_id = 100;

    const ConstraintEvent cases[] = {fixed, point, hinge, slider,
                                     dist,  cone,  six,   clone,
                                     remove};
    // Wire sizes: header(8) + payload per (op, kind).
    const usize sizes[] = {20, 32, 44, 44, 52, 48, 80, 24, 12};
    for (usize i = 0; i < 9; ++i) {
        const std::vector<u8> bytes = encode_constraint_event(cases[i]);
        NF_CHECK(bytes.size() == sizes[i]);
        NF_CHECK(bytes[0] == 'N' && bytes[1] == 'F' && bytes[2] == 'C' && bytes[3] == 'E');
        ConstraintEvent back;
        NF_CHECK(decode_constraint_event(bytes.data(), bytes.size(), back, err));
        NF_CHECK(back == cases[i]);
        NF_CHECK(encode_constraint_event(back) == bytes); // canonical bytes
    }
}

NF_TEST(constraint_event_rejects_corruption) {
    ConstraintEvent hinge = make_spawn(ConstraintKind::Hinge, 102, 3, 4);
    hinge.point[1] = 1.5f;
    hinge.axis[2] = 1.0f;
    const std::vector<u8> bytes = encode_constraint_event(hinge);

    ConstraintEvent bad;
    std::string err;
    NF_CHECK(!decode_constraint_event(bytes.data(), bytes.size() - 1, bad, err));
    NF_CHECK(!decode_constraint_event(bytes.data(), 0, bad, err));
    NF_CHECK(!decode_constraint_event(nullptr, bytes.size(), bad, err));

    std::vector<u8> magic = bytes;
    magic[1] = 'X';
    NF_CHECK(!decode_constraint_event(magic.data(), magic.size(), bad, err));

    std::vector<u8> version = bytes;
    version[4] = 9;
    NF_CHECK(!decode_constraint_event(version.data(), version.size(), bad, err));

    std::vector<u8> op = bytes;
    op[6] = 9; // unknown op
    NF_CHECK(!decode_constraint_event(op.data(), op.size(), bad, err));

    std::vector<u8> kind = bytes;
    kind[7] = 0; // unknown kind
    NF_CHECK(!decode_constraint_event(kind.data(), kind.size(), bad, err));

    // Zero ids and same-body joints mirror the JoltWorld refusals.
    ConstraintEvent zero = make_spawn(ConstraintKind::Fixed, 0, 1, 2);
    NF_CHECK(!decode_constraint_event(encode_constraint_event(zero).data(), 20, bad, err));
    ConstraintEvent same = make_spawn(ConstraintKind::Fixed, 50, 7, 7);
    NF_CHECK(!decode_constraint_event(encode_constraint_event(same).data(), 20, bad, err));

    // Inverted distance limits, wild cone angle, degenerate axis.
    ConstraintEvent dist = make_spawn(ConstraintKind::Distance, 60, 1, 2);
    dist.param0 = 3.0f;
    dist.param1 = 1.0f;
    NF_CHECK(!decode_constraint_event(encode_constraint_event(dist).data(), 52, bad, err));
    ConstraintEvent cone = make_spawn(ConstraintKind::Cone, 61, 1, 2);
    cone.axis[0] = 1.0f;
    cone.param0 = 9.0f;
    NF_CHECK(!decode_constraint_event(encode_constraint_event(cone).data(), 48, bad, err));
    ConstraintEvent noaxis = make_spawn(ConstraintKind::Hinge, 62, 1, 2);
    NF_CHECK(!decode_constraint_event(encode_constraint_event(noaxis).data(), 44, bad, err));

    // A remove for id zero frees nothing.
    ConstraintEvent rm;
    rm.op = ConstraintOp::Remove;
    NF_CHECK(!decode_constraint_event(encode_constraint_event(rm).data(), 12, bad, err));
}

// --- server input queue -----------------------------------------------------------

NF_TEST(input_queue_tick_matched_consume) {
    VehicleInputQueue queue;
    queue.submit_drive_input(7, make_drive(0, 7, 1.0f, 0.0f, 0.0f));
    queue.submit_drive_input(7, make_drive(1, 7, 0.5f, 0.25f, 0.0f));
    NF_CHECK(queue.pending(7) == 2);

    VehicleDriveInput out;
    NF_CHECK(queue.consume(7, 0, out)); // exact tick match
    NF_CHECK(out.throttle == 1.0f && out.tick == 0);
    NF_CHECK(queue.pending(7) == 1);

    NF_CHECK(!queue.consume(7, 9, out)); // missing tick = neutral
    NF_CHECK(out.throttle == 0.0f && out.steer == 0.0f && out.brake == 0.0f);
    NF_CHECK(out.tick == 9 && out.entity == 7);
    NF_CHECK(queue.pending(7) == 1); // a miss drops nothing

    NF_CHECK(queue.consume(7, 1, out));
    NF_CHECK(out.throttle == 0.5f && out.steer == 0.25f);
    NF_CHECK(queue.pending(7) == 0);
}

NF_TEST(input_queue_resubmit_wins_and_old_drops) {
    VehicleInputQueue queue;
    queue.submit_drive_input(3, make_drive(4, 3, 1.0f, 0.0f, 0.0f));
    queue.submit_drive_input(3, make_drive(4, 3, -1.0f, 0.5f, 1.0f)); // resubmit
    NF_CHECK(queue.pending(3) == 1);
    VehicleDriveInput out;
    NF_CHECK(queue.consume(3, 4, out));
    NF_CHECK(out.throttle == -1.0f && out.steer == 0.5f && out.brake == 1.0f);

    // Consuming a newer tick drops the older one unseen (late never rewinds).
    queue.submit_drive_input(3, make_drive(10, 3, 1.0f, 0.0f, 0.0f));
    queue.submit_drive_input(3, make_drive(11, 3, 1.0f, 0.0f, 0.0f));
    NF_CHECK(queue.consume(3, 11, out));
    NF_CHECK(out.tick == 11 && queue.pending(3) == 0);

    // Entities are isolated.
    queue.submit_drive_input(8, make_drive(0, 8, 1.0f, 0.0f, 0.0f));
    NF_CHECK(!queue.consume(9, 0, out));
    NF_CHECK(out.entity == 9 && out.throttle == 0.0f);
    NF_CHECK(queue.pending(8) == 1 && queue.pending(999) == 0);
}

// --- client ghosts ------------------------------------------------------------------

NF_TEST(ghost_client_applies_and_tracks_correction) {
    VehicleGhostClient ghost(7);
    NF_CHECK(!ghost.has_snapshot());

    VehicleSnapshot s0;
    s0.tick = 0;
    VehicleNetState v;
    v.entity = 7;
    v.x = 10.0f;
    s0.vehicles.push_back(v);
    ghost.apply_vehicle_snapshot(s0);
    NF_CHECK(ghost.has_snapshot() && ghost.last_tick() == 0);
    NF_CHECK(ghost.last_correction() == 0.0f); // brand-new ghost: nothing to snap from
    VehicleNetState read;
    NF_CHECK(ghost.state(7, read) && read.x == 10.0f);
    NF_CHECK(!ghost.state(8, read)); // unknown ghost
    NF_CHECK(ghost.ghost_count() == 1);

    // Server moved the car 3 units: the snap distance is reported.
    VehicleSnapshot s1;
    s1.tick = 1;
    VehicleNetState v1 = v;
    v1.x = 13.0f;
    v1.vx = 5.0f;
    VehicleNetState other;
    other.entity = 12;
    other.z = -4.0f;
    s1.vehicles.push_back(v1);
    s1.vehicles.push_back(other);
    ghost.apply_vehicle_snapshot(s1);
    NF_CHECK_NEAR(ghost.last_correction(), 3.0f, 1e-5f);
    NF_CHECK(ghost.state(7, read) && read.vx == 5.0f);
    NF_CHECK(ghost.state(12, read) && read.z == -4.0f);
    NF_CHECK(ghost.ghost_count() == 2);
}

NF_TEST(ghost_client_rejects_stale_snapshots) {
    VehicleGhostClient ghost(5);
    VehicleSnapshot s;
    s.tick = 10;
    VehicleNetState v;
    v.entity = 5;
    v.x = 1.0f;
    s.vehicles.push_back(v);
    ghost.apply_vehicle_snapshot(s);
    NF_CHECK(ghost.last_tick() == 10);

    // Same tick twice and older ticks never rewind.
    VehicleSnapshot same = s;
    same.vehicles[0].x = 99.0f;
    ghost.apply_vehicle_snapshot(same);
    VehicleNetState read;
    NF_CHECK(ghost.state(5, read) && read.x == 1.0f);

    VehicleSnapshot older;
    older.tick = 3;
    older.vehicles.push_back(v);
    ghost.apply_vehicle_snapshot(older);
    NF_CHECK(ghost.last_tick() == 10);
    NF_CHECK(ghost.state(5, read) && read.x == 1.0f);
}

// --- full stack over real UDP ----------------------------------------------------------

NF_TEST(vehicle_replication_over_loopback_sockets) {
    // Drive inputs ride the reliable channel up; vehicle snapshots return as
    // raw datagrams; acks drain home. 60 ticks of full throttle, no loss.
    UdpSocket client_sock, server_sock;
    NF_CHECK(client_sock.open(0, true, nullptr));
    NF_CHECK(server_sock.open(0, true, nullptr));
    const u16 server_port = server_sock.local_port();
    const u16 client_port = client_sock.local_port();

    VehicleInputQueue queue;
    VehicleGhostClient ghost(7);
    ReliableChannel up(0, 5); // client -> server drive inputs (fast resends)
    ReliableChannel down;     // server -> client acks

    float server_x = 0.0f, server_vx = 0.0f;
    constexpr float kSpeed = 6.0f;
    constexpr float kDt = 1.0f / 60.0f;

    auto send_packets = [&](ReliableChannel& from, UdpSocket& sock, u16 port, u64 now_ms) {
        for (auto& pkt : from.poll_outgoing(now_ms)) {
            sock.send_to(encode_packet(pkt), kIpv4Loopback, port);
        }
    };
    auto server_wait_drive = [&](u32 want_tick) {
        bool have = false;
        for (int i = 0; i < 200 && !have; ++i) {
            Datagram d;
            while (server_sock.recv_from(d)) {
                NetPacket pkt;
                std::string err;
                if (!decode_packet(d.payload.data(), d.payload.size(), pkt, err)) continue;
                for (auto& payload : down.receive(pkt)) {
                    VehicleDriveInput got;
                    if (decode_drive_input(payload.data(), payload.size(), got, err)) {
                        queue.submit_drive_input(got.entity, got);
                        if (got.tick == want_tick) have = true;
                    }
                }
                d = Datagram{};
            }
            if (!have) {
                send_packets(up, client_sock, server_port, static_cast<u64>(want_tick) * 20 + i);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return have;
    };
    auto client_wait_vehicle = [&](VehicleSnapshot& snap) {
        bool have = false;
        for (int i = 0; i < 200 && !have; ++i) {
            Datagram d;
            while (client_sock.recv_from(d)) {
                if (d.payload.size() >= 4 && d.payload[0] == 'N' && d.payload[1] == 'F' &&
                    d.payload[2] == 'V' && d.payload[3] == 'S') {
                    std::string err;
                    if (decode_vehicle_snapshot(d.payload.data(), d.payload.size(), snap,
                                                err)) {
                        have = true;
                    }
                } else if (d.payload.size() >= 4 && d.payload[0] == 'N' &&
                           d.payload[1] == 'F' && d.payload[2] == 'C' &&
                           d.payload[3] == 'H') {
                    NetPacket pkt;
                    std::string err;
                    if (decode_packet(d.payload.data(), d.payload.size(), pkt, err)) {
                        for (auto& payload : up.receive(pkt)) (void)payload;
                    }
                }
                d = Datagram{};
            }
            if (!have) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return have;
    };

    for (u32 t = 0; t < 60; ++t) {
        NF_CHECK(up.send_reliable(encode_drive_input(make_drive(t, 7, 1.0f, 0.0f, 0.0f))));
        send_packets(up, client_sock, server_port, static_cast<u64>(t) * 20);
        NF_CHECK(server_wait_drive(t));
        // Server: consume the tick's input, integrate, publish truth.
        VehicleDriveInput drive;
        NF_CHECK(queue.consume(7, t, drive));
        NF_CHECK(drive.throttle == 1.0f);
        server_vx = drive.throttle * kSpeed;
        server_x += server_vx * kDt;
        VehicleSnapshot snap;
        snap.tick = t;
        VehicleNetState state;
        state.entity = 7;
        state.x = server_x;
        state.vx = server_vx;
        snap.vehicles.push_back(state);
        server_sock.send_to(encode_vehicle_snapshot(snap), kIpv4Loopback, client_port);
        send_packets(down, server_sock, client_port, static_cast<u64>(t) * 20);
        VehicleSnapshot got;
        NF_CHECK(client_wait_vehicle(got));
        ghost.apply_vehicle_snapshot(got);
    }
    // Settle remaining acks.
    for (int i = 0; i < 50 && up.unacked_count() > 0; ++i) {
        send_packets(down, server_sock, client_port, 100000 + i);
        Datagram d;
        while (client_sock.recv_from(d)) {
            NetPacket pkt;
            std::string err;
            if (decode_packet(d.payload.data(), d.payload.size(), pkt, err)) {
                for (auto& payload : up.receive(pkt)) (void)payload;
            }
            d = Datagram{};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    VehicleNetState read;
    NF_CHECK(ghost.state(7, read));
    NF_CHECK_NEAR(read.x, server_x, 1e-4f);
    NF_CHECK_NEAR(read.x, 6.0f, 1e-3f); // 60/60 * 6 u/s at full throttle
    NF_CHECK_NEAR(read.vx, 6.0f, 1e-4f);
    NF_CHECK(queue.pending(7) == 0);   // every input consumed exactly once
    NF_CHECK(up.unacked_count() == 0); // every input acked over real UDP
}

NF_TEST(constraint_events_survive_the_reliable_channel) {
    // Spawn + clone + remove in order over a lossy-feeling pump: out-of-order
    // delivery still applies in send order, so the joint lifecycle is exact.
    ReliableChannel sender, receiver;
    ConstraintEvent spawn = make_spawn(ConstraintKind::Hinge, 40, 1, 2);
    spawn.point[1] = 2.0f;
    spawn.axis[2] = 1.0f;
    ConstraintEvent clone;
    clone.op = ConstraintOp::Clone;
    clone.net_id = 41;
    clone.source = 40;
    clone.body_a = 3;
    clone.body_b = 4;
    ConstraintEvent remove;
    remove.op = ConstraintOp::Remove;
    remove.net_id = 40;

    NF_CHECK(sender.send_reliable(encode_constraint_event(spawn)));
    NF_CHECK(sender.send_reliable(encode_constraint_event(clone)));
    NF_CHECK(sender.send_reliable(encode_constraint_event(remove)));

    // Deliver newest-first: the channel still hands them over in order.
    std::vector<NetPacket> flights = sender.poll_outgoing(0);
    NF_CHECK(flights.size() == 3);
    std::vector<ConstraintEvent> applied;
    for (usize i = flights.size(); i-- > 0;) {
        for (auto& payload : receiver.receive(flights[i])) {
            ConstraintEvent e;
            std::string err;
            NF_CHECK(decode_constraint_event(payload.data(), payload.size(), e, err));
            applied.push_back(e);
        }
    }
    NF_CHECK(applied.size() == 3);
    NF_CHECK(applied[0] == spawn);
    NF_CHECK(applied[1] == clone);
    NF_CHECK(applied[2] == remove);
}
