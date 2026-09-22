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

#include <algorithm>
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

NF_TEST(desync_detected_and_corrected_over_loopback) {
    // A deliberate UPLINK BLACKOUT desyncs the client from the server over real
    // UDP: the client keeps predicting (it holds its inputs locally) while the
    // server, seeing nothing for those ticks, applies neutral and stands still.
    // The link returns, the first authoritative snapshot lands, and
    // last_correction() reports the accumulated drift — that is DETECTION. The
    // acked-and-stale history is dropped and play continues from authority, so
    // the correction signal returns to zero and both ends finish in agreement —
    // that is CORRECTION.
    //
    // The in-memory teleport test can exercise the reconcile path but never
    // this: here the desync accumulates across a real socket for 20 ticks
    // before it is even observable, and the client heals itself from bytes on
    // the wire.
    UdpSocket client_sock, server_sock;
    NF_CHECK(client_sock.open(0, true, nullptr));
    NF_CHECK(server_sock.open(0, true, nullptr));
    const u16 server_port = server_sock.local_port();
    const u16 client_port = client_sock.local_port();

    AuthoritativeServer server;
    server.add_player(1, 0, 0, 0);
    PredictedClient client(1);
    ReliableChannel up(0, 5); // client -> server inputs
    ReliableChannel down;     // server -> client acks

    auto send_packets = [&](ReliableChannel& from, UdpSocket& sock, u16 port, u64 now_ms) {
        for (auto& pkt : from.poll_outgoing(now_ms)) {
            sock.send_to(encode_packet(pkt), kIpv4Loopback, port);
        }
    };
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
                    if (decode_input(payload.data(), payload.size(), got, err)) {
                        server.submit_input(1, got);
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

    // Ticks 10..29 are the blackout: neither direction is pumped. The client
    // predicts through them anyway, which is exactly what a player sees during
    // a lag spike — the game keeps moving, the server does not.
    const u32 kBlackoutBegin = 10;
    const u32 kBlackoutEnd = 30;
    auto link_up = [&](u32 t) { return t < kBlackoutBegin || t >= kBlackoutEnd; };

    float correction_at_restore = -1.0f;
    for (u32 t = 0; t < 60; ++t) {
        client.push_local_input(1.0f, 0.0f); // predicted every tick, blackout or not
        if (link_up(t)) {
            NetInput in;
            in.tick = t;
            in.move_x = 1.0f;
            NF_CHECK(up.send_reliable(encode_input(in)));
            send_packets(up, client_sock, server_port, static_cast<u64>(t) * 20);
            NF_CHECK(server_wait_input(t));
        }
        server.tick(); // the server always advances; a missing input is neutral
        if (link_up(t)) {
            server_sock.send_to(encode_snapshot(server.last_snapshot()), kIpv4Loopback,
                                client_port);
            send_packets(down, server_sock, client_port, static_cast<u64>(t) * 20);
            Snapshot snap;
            NF_CHECK(client_wait_snapshot(snap));
            client.on_snapshot(snap);
            if (t == kBlackoutEnd) correction_at_restore = client.last_correction();
        }
    }
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

    // DETECTED: the first snapshot after the blackout corrected 20 ticks of
    // client-only motion. The client had predicted 31 ticks (3.1u) against the
    // server's 11 (1.1u): the drift is real and was reported.
    NF_CHECK(correction_at_restore > 1.9f);

    // CORRECTED: the two ends now agree, and the correction signal has returned
    // to zero — the last snapshots applied cleanly, not by snapping every tick.
    float cx = 0, cy = 0, cz = 0, sx = 0, sy = 0, sz = 0;
    NF_CHECK(client.predicted_position(cx, cy, cz));
    NF_CHECK(server.world().position(1, sx, sy, sz));
    NF_CHECK_NEAR(cx, sx, 1e-3f);
    NF_CHECK_NEAR(cx, 4.0f, 1e-3f); // 40 driven ticks (0-9 + 30-59) at 6 u/s
    NF_CHECK_NEAR(client.last_correction(), 0.0f, 1e-3f);
    NF_CHECK(up.unacked_count() == 0);
}

NF_TEST(two_clients_synchronize_over_loopback) {
    // Two predicting clients, one authoritative server, three real UDP sockets.
    // Each client OWNS one entity and predicts it; the other player arrives
    // only as a ghost inside the shared snapshot. Synchronization means both
    // games agree about BOTH players — not merely that each client is right
    // about itself. A client that only matched its own entity could be showing
    // every other player in the wrong place and this test would still pass, so
    // the cross-entity assertions are the ones that matter.
    UdpSocket server_sock, a_sock, b_sock;
    NF_CHECK(server_sock.open(0, true, nullptr));
    NF_CHECK(a_sock.open(0, true, nullptr));
    NF_CHECK(b_sock.open(0, true, nullptr));
    const u16 server_port = server_sock.local_port();
    const u16 a_port = a_sock.local_port();
    const u16 b_port = b_sock.local_port();

    AuthoritativeServer server;
    server.add_player(1, 0, 0, 0);
    server.add_player(2, 0, 0, 0);
    PredictedClient a(1); // drives +x
    PredictedClient b(2); // drives +z
    ReliableChannel up_a(0, 5), up_b(0, 5); // each client's sender
    ReliableChannel down_a, down_b;         // the server-side peer of each

    auto send_packets = [&](ReliableChannel& from, UdpSocket& sock, u16 port, u64 now_ms) {
        for (auto& pkt : from.poll_outgoing(now_ms)) {
            sock.send_to(encode_packet(pkt), kIpv4Loopback, port);
        }
    };
    // The server demultiplexes the two clients by source port, because both
    // connections share one socket on the server side. NetInput carries no
    // player id: the socket the bytes came in on IS the identity.
    auto server_pump = [&](u32 want_tick, bool& got_a, bool& got_b) {
        for (int i = 0; i < 200 && (!got_a || !got_b); ++i) {
            Datagram d;
            while (server_sock.recv_from(d)) {
                NetPacket pkt;
                std::string err;
                if (!decode_packet(d.payload.data(), d.payload.size(), pkt, err)) continue;
                const bool from_a = (d.from_port == a_port);
                ReliableChannel& chan = from_a ? down_a : down_b;
                for (auto& payload : chan.receive(pkt)) {
                    NetInput got;
                    if (!decode_input(payload.data(), payload.size(), got, err)) continue;
                    const u32 player = from_a ? 1u : 2u;
                    server.submit_input(player, got);
                    if (got.tick == want_tick) {
                        if (from_a) got_a = true;
                        else got_b = true;
                    }
                }
                d = Datagram{};
            }
            // Nudge a resend for whichever client has not landed this tick yet.
            if (!got_a) send_packets(up_a, a_sock, server_port, static_cast<u64>(want_tick) * 20 + i);
            if (!got_b) send_packets(up_b, b_sock, server_port, static_cast<u64>(want_tick) * 20 + i);
            if (!got_a || !got_b) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    auto client_wait_snapshot = [&](UdpSocket& sock, ReliableChannel& up, Snapshot& snap) {
        bool have = false;
        for (int i = 0; i < 200 && !have; ++i) {
            Datagram d;
            while (sock.recv_from(d)) {
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

    float max_correction_a = 0.0f, max_correction_b = 0.0f;
    for (u32 t = 0; t < 60; ++t) {
        a.push_local_input(1.0f, 0.0f);
        b.push_local_input(0.0f, 1.0f);
        NetInput ia;
        ia.tick = t;
        ia.move_x = 1.0f;
        NF_CHECK(up_a.send_reliable(encode_input(ia)));
        NetInput ib;
        ib.tick = t;
        ib.move_z = 1.0f;
        NF_CHECK(up_b.send_reliable(encode_input(ib)));
        send_packets(up_a, a_sock, server_port, static_cast<u64>(t) * 20);
        send_packets(up_b, b_sock, server_port, static_cast<u64>(t) * 20);

        bool got_a = false, got_b = false;
        server_pump(t, got_a, got_b);
        NF_CHECK(got_a && got_b);

        server.tick();
        const std::vector<u8> snap_bytes = encode_snapshot(server.last_snapshot());
        server_sock.send_to(snap_bytes, kIpv4Loopback, a_port);
        server_sock.send_to(snap_bytes, kIpv4Loopback, b_port);
        send_packets(down_a, server_sock, a_port, static_cast<u64>(t) * 20);
        send_packets(down_b, server_sock, b_port, static_cast<u64>(t) * 20);

        Snapshot sa, sb;
        NF_CHECK(client_wait_snapshot(a_sock, up_a, sa));
        NF_CHECK(client_wait_snapshot(b_sock, up_b, sb));
        a.on_snapshot(sa);
        b.on_snapshot(sb);
        max_correction_a = std::max(max_correction_a, a.last_correction());
        max_correction_b = std::max(max_correction_b, b.last_correction());
    }
    for (int i = 0; i < 50 && (up_a.unacked_count() > 0 || up_b.unacked_count() > 0); ++i) {
        send_packets(down_a, server_sock, a_port, 100000 + i);
        send_packets(down_b, server_sock, b_port, 100000 + i);
        auto drain = [&](UdpSocket& sock, ReliableChannel& up) {
            Datagram d;
            while (sock.recv_from(d)) {
                NetPacket pkt;
                std::string err;
                if (decode_packet(d.payload.data(), d.payload.size(), pkt, err)) {
                    for (auto& payload : up.receive(pkt)) (void)payload;
                }
                d = Datagram{};
            }
        };
        drain(a_sock, up_a);
        drain(b_sock, up_b);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Each client is right about its OWN entity (prediction held).
    float ax = 0, ay = 0, az = 0, bx = 0, by = 0, bz = 0;
    NF_CHECK(a.predicted_position(ax, ay, az));
    NF_CHECK(b.predicted_position(bx, by, bz));
    NF_CHECK_NEAR(ax, 6.0f, 1e-3f); // 60 ticks at 6 u/s along +x
    NF_CHECK_NEAR(bz, 6.0f, 1e-3f); // ...and along +z

    // ...and about the OTHER player, who exists on each side only as a ghost
    // rebuilt from the shared snapshot. Both games see the same world.
    float ghost_bx = 0, ghost_by = 0, ghost_bz = 0; // client A's view of player 2
    float ghost_ax = 0, ghost_ay = 0, ghost_az = 0; // client B's view of player 1
    NF_CHECK(a.world().position(2, ghost_bx, ghost_by, ghost_bz));
    NF_CHECK(b.world().position(1, ghost_ax, ghost_ay, ghost_az));
    NF_CHECK_NEAR(ghost_bx, 0.0f, 1e-3f);
    NF_CHECK_NEAR(ghost_bz, 6.0f, 1e-3f);
    NF_CHECK_NEAR(ghost_ax, 6.0f, 1e-3f);
    NF_CHECK_NEAR(ghost_az, 0.0f, 1e-3f);

    // The server is the authority both clients agree with.
    float sx1 = 0, sy1 = 0, sz1 = 0, sx2 = 0, sy2 = 0, sz2 = 0;
    NF_CHECK(server.world().position(1, sx1, sy1, sz1));
    NF_CHECK(server.world().position(2, sx2, sy2, sz2));
    NF_CHECK_NEAR(ax, sx1, 1e-3f);
    NF_CHECK_NEAR(ghost_bz, sz2, 1e-3f);
    NF_CHECK_NEAR(bz, sz2, 1e-3f);
    NF_CHECK_NEAR(ghost_ax, sx1, 1e-3f);

    // Neither client ever corrected: prediction was exact for the whole run, so
    // the synchronization above is real agreement, not a series of snaps.
    NF_CHECK_NEAR(max_correction_a, 0.0f, 1e-3f);
    NF_CHECK_NEAR(max_correction_b, 0.0f, 1e-3f);
    NF_CHECK(up_a.unacked_count() == 0);
    NF_CHECK(up_b.unacked_count() == 0);
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
