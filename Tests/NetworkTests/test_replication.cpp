// NetworkTests — authoritative server + prediction (Phase 16).
//
// In-memory pumping first (exact lockstep), then a full loopback run over
// real UDP sockets + the reliable channel (inputs) with raw snapshot
// datagrams back.

#include <NF/Networking/Authoritative.hpp>
#include <NF/Networking/ReliableChannel.hpp>
#include <NF/Networking/Snapshot.hpp>
#include <NF/Networking/Socket.hpp>
#include <NF/Test/TestFramework.hpp>

#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

using namespace nf;
using namespace nf::net;

NF_TEST(net_input_codec_roundtrip) {
    NetInput in;
    in.tick = 12345;
    in.move_x = 0.75f;
    in.move_z = -0.25f;
    in.jump = true;
    const std::vector<u8> bytes = encode_input(in);
    NF_CHECK(bytes.size() == 16);
    NetInput back;
    std::string err;
    NF_CHECK(decode_input(bytes.data(), bytes.size(), back, err));
    NF_CHECK(back == in);
    NetInput bad;
    NF_CHECK(!decode_input(bytes.data(), 7, bad, err));
    std::vector<u8> corrupt = bytes;
    corrupt[12] = 7;
    NF_CHECK(!decode_input(corrupt.data(), corrupt.size(), bad, err));
}

NF_TEST(server_integrates_tick_matched_inputs) {
    AuthoritativeServer server;
    server.add_player(1, 0, 0, 0);
    for (u32 t = 0; t < 60; ++t) {
        NetInput in;
        in.tick = t;
        in.move_x = 1.0f;
        server.submit_input(1, in);
        server.tick();
    }
    NF_CHECK(server.tick_index() == 60);
    float x = 0, y = 0, z = 0;
    NF_CHECK(server.world().position(1, x, y, z));
    // 60 ticks * (1/60)s * 6 u/s = 6 units.
    NF_CHECK_NEAR(x, 6.0f, 1e-4f);
    NF_CHECK_NEAR(z, 0.0f, 1e-6f);
    NF_CHECK(server.last_snapshot().tick == 59);
}

NF_TEST(server_missing_input_is_neutral) {
    AuthoritativeServer server;
    server.add_player(1, 0, 0, 0);
    NetInput in;
    in.tick = 0;
    in.move_x = 1.0f;
    server.submit_input(1, in);
    server.tick(); // tick 0 moves
    server.tick(); // tick 1: no input -> coasts to a stop (velocity overwritten neutral)
    float x = 0, y = 0, z = 0;
    NF_CHECK(server.world().position(1, x, y, z));
    NF_CHECK_NEAR(x, 0.1f, 1e-4f); // exactly one tick of motion
}

NF_TEST(client_prediction_matches_zero_latency_server) {
    AuthoritativeServer server;
    server.add_player(1, 0, 0, 0);
    PredictedClient client(1);
    for (u32 t = 0; t < 30; ++t) {
        const float mx = (t % 2 == 0) ? 1.0f : 0.0f;
        client.push_local_input(mx, 0.5f);
        NetInput in;
        in.tick = t;
        in.move_x = mx;
        in.move_z = 0.5f;
        server.submit_input(1, in);
        server.tick();
        client.on_snapshot(server.last_snapshot());
    }
    float cx = 0, cy = 0, cz = 0, sx = 0, sy = 0, sz = 0;
    NF_CHECK(client.predicted_position(cx, cy, cz));
    NF_CHECK(server.world().position(1, sx, sy, sz));
    NF_CHECK_NEAR(cx, sx, 1e-4f);
    NF_CHECK_NEAR(cz, sz, 1e-4f);
    NF_CHECK_NEAR(client.last_correction(), 0.0f, 1e-4f); // no drift, no snap
}

NF_TEST(client_reconciles_after_teleport) {
    AuthoritativeServer server;
    server.add_player(1, 0, 0, 0);
    PredictedClient client(1);
    for (u32 t = 0; t < 10; ++t) {
        client.push_local_input(1.0f, 0.0f);
        NetInput in;
        in.tick = t;
        in.move_x = 1.0f;
        server.submit_input(1, in);
        server.tick();
        client.on_snapshot(server.last_snapshot());
    }
    // Server-side teleport on a NEW tick (lag spike / correction event the
    // client never saw): advance once, slip the fresh snapshot, deliver.
    server.tick();
    Snapshot slipped = server.last_snapshot();
    for (auto& e : slipped.entities) e.x += 5.0f;
    client.on_snapshot(slipped);
    NF_CHECK(client.last_correction() > 4.9f); // the snap was real
    // ...and the client keeps playing on top of authority afterwards.
    client.push_local_input(1.0f, 0.0f);
    float cx = 0, cy = 0, cz = 0;
    NF_CHECK(client.predicted_position(cx, cy, cz));
    NF_CHECK(cx > 5.0f);
    // Stale snapshots never rewind.
    client.on_snapshot(server.last_snapshot());
    float cx2 = 0, cy2 = 0, cz2 = 0;
    NF_CHECK(client.predicted_position(cx2, cy2, cz2));
    NF_CHECK_NEAR(cx2, cx, 1e-5f);
}

NF_TEST(replication_over_loopback_sockets) {
    // Full stack over real UDP: client inputs ride the reliable channel
    // (framed packets) to the server; snapshots return as raw datagrams;
    // acks flow back through the same channels. 60 ticks, no loss.
    UdpSocket client_sock, server_sock;
    NF_CHECK(client_sock.open(0, true, nullptr));
    NF_CHECK(server_sock.open(0, true, nullptr));
    const u16 server_port = server_sock.local_port();
    const u16 client_port = client_sock.local_port();

    AuthoritativeServer server;
    server.add_player(1, 0, 0, 0);
    PredictedClient client(1);
    ReliableChannel up(0, 5); // client -> server inputs (fast resends)
    ReliableChannel down; // server -> client acks

    auto send_packets = [&](ReliableChannel& from, UdpSocket& sock, u16 port, u64 now_ms) {
        for (auto& pkt : from.poll_outgoing(now_ms)) {
            sock.send_to(encode_packet(pkt), kIpv4Loopback, port);
        }
    };
    // Receives on `sock` until `want_input_tick` arrives (server side) or
    // timeout. Channel packets feed `chan`; returns decoded input payloads.
    auto server_wait_input = [&](u32 want_tick) {
        bool have = false;
        for (int i = 0; i < 200 && !have; ++i) {
            Datagram d;
            while (server_sock.recv_from(d)) {
                NetPacket pkt;
                std::string err;
                if (!decode_packet(d.payload.data(), d.payload.size(), pkt, err)) continue;
                for (auto& payload : down.receive(pkt)) {
                    NetInput got;
                    if (decode_input(payload.data(), payload.size(), got, err) &&
                        got.tick == want_tick) {
                        server.submit_input(1, got);
                        have = true;
                    } else if (decode_input(payload.data(), payload.size(), got, err)) {
                        server.submit_input(1, got);
                    }
                }
                d = Datagram{};
            }
            if (!have) {
                // Nudge a resend while waiting (covers a dropped first flight).
                send_packets(up, client_sock, server_port, static_cast<u64>(want_tick) * 20 + i);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return have;
    };
    // Receives on the client until a snapshot arrives. Channel packets feed
    // `up` (freeing the input queue); snapshots decode into `snap`.
    auto client_wait_snapshot = [&](Snapshot& snap) {
        bool have = false;
        for (int i = 0; i < 200 && !have; ++i) {
            Datagram d;
            while (client_sock.recv_from(d)) {
                if (d.payload.size() >= 4 && d.payload[0] == 'N' && d.payload[1] == 'F' &&
                    d.payload[2] == 'C' && d.payload[3] == 'H') {
                    NetPacket pkt;
                    std::string err;
                    if (decode_packet(d.payload.data(), d.payload.size(), pkt, err)) {
                        for (auto& payload : up.receive(pkt)) (void)payload;
                    }
                } else {
                    std::string err;
                    if (decode_snapshot(d.payload.data(), d.payload.size(), snap, err)) {
                        have = true;
                    }
                }
                d = Datagram{};
            }
            if (!have) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return have;
    };

    for (u32 t = 0; t < 60; ++t) {
        client.push_local_input(1.0f, 0.0f);
        NetInput in;
        in.tick = t;
        in.move_x = 1.0f;
        NF_CHECK(up.send_reliable(encode_input(in)));
        send_packets(up, client_sock, server_port, static_cast<u64>(t) * 20);
        NF_CHECK(server_wait_input(t));
        server.tick();
        server_sock.send_to(encode_snapshot(server.last_snapshot()), kIpv4Loopback,
                            client_port);
        send_packets(down, server_sock, client_port, static_cast<u64>(t) * 20); // acks home
        Snapshot snap;
        NF_CHECK(client_wait_snapshot(snap));
        client.on_snapshot(snap);
    }
    // Settle remaining acks (no more snapshots; just drain the queue).
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
    float cx = 0, cy = 0, cz = 0, sx = 0, sy = 0, sz = 0;
    NF_CHECK(client.predicted_position(cx, cy, cz));
    NF_CHECK(server.world().position(1, sx, sy, sz));
    NF_CHECK_NEAR(cx, sx, 1e-3f);
    NF_CHECK_NEAR(cx, 6.0f, 1e-3f); // 60/60 * 6 u/s
    NF_CHECK(up.unacked_count() == 0); // every input acked over real UDP
}
