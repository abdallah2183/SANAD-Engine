// NetworkTests — the dedicated server (design §81: NOVAForgeServer builds and
// runs without Graphics, Editor or GPU resources).
//
// Two layers, both over real UDP:
//   1. In-process: DedicatedServer in the test's own address space, with the
//      test driving pump/tick — deterministic, and the place to assert the
//      provisioning/refusal/malformed-packet rules.
//   2. The real binary: the test spawns the built NOVAForgeServer process and
//      a PredictedClient connects to it. That is the §81 deliverable itself —
//      a headless server a client actually talks to — and the only place the
//      tick-gate pacing (wait for inputs, then step) is exercised across a
//      process boundary.

#include <NF/Networking/Authoritative.hpp>
#include <NF/Networking/DedicatedServer.hpp>
#include <NF/Networking/ReliableChannel.hpp>
#include <NF/Networking/Snapshot.hpp>
#include <NF/Networking/Socket.hpp>
#include <NF/Test/TestFramework.hpp>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace nf;
using namespace nf::net;

namespace {

/// A minimal game-layer client: a predicting entity plus the socket plumbing
/// every loopback test in this suite already uses, factored out so the
/// dedicated-server tests can talk to a server the way a game would.
class LoopbackClient {
public:
    LoopbackClient(u32 entity, u16 server_port)
        : m_client(entity), m_server_port(server_port), m_channel(0, 5) {
        m_socket.open(0, true, nullptr);
    }

    u16 local_port() const { return m_socket.local_port(); }

    /// Predicts one tick locally and sends that tick's input to the server.
    void push_input(float move_x, float move_z, u64 now_ms) {
        m_client.push_local_input(move_x, move_z);
        NetInput in;
        in.tick = m_client.next_tick() - 1; // the tick just predicted
        in.move_x = move_x;
        in.move_z = move_z;
        m_channel.send_reliable(encode_input(in));
        flush_outgoing(now_ms);
    }

    void flush_outgoing(u64 now_ms) {
        for (const auto& pkt : m_channel.poll_outgoing(now_ms)) {
            m_socket.send_to(encode_packet(pkt), kIpv4Loopback, m_server_port);
        }
    }

    /// Reads until the snapshot for `tick` arrives (or the attempt times out),
    /// feeding channel packets into the receive path so acks flow home. Does
    /// NOT apply the snapshot — the caller decides (a test may want to assert
    /// on it first).
    bool wait_snapshot(u32 tick, Snapshot& out) {
        for (int i = 0; i < 500; ++i) {
            Datagram d;
            while (m_socket.recv_from(d)) {
                if (d.payload.size() >= 4 && d.payload[0] == 'N' && d.payload[1] == 'F' &&
                    d.payload[2] == 'C' && d.payload[3] == 'H') {
                    NetPacket pkt;
                    std::string err;
                    if (decode_packet(d.payload.data(), d.payload.size(), pkt, err)) {
                        for (const auto& payload : m_channel.receive(pkt)) (void)payload;
                    }
                } else {
                    std::string err;
                    if (decode_snapshot(d.payload.data(), d.payload.size(), out, err) &&
                        out.tick == tick) {
                        return true;
                    }
                }
                d = Datagram{};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    PredictedClient& client() { return m_client; }

private:
    UdpSocket m_socket;
    u16 m_server_port = 0;
    ReliableChannel m_channel;
    PredictedClient m_client;
};

} // namespace

NF_TEST(dedicated_server_holds_its_tick_clock_for_the_first_client) {
    // No client has connected yet. The server must NOT advance: a server that
    // burns its tick clock before the first player arrives puts every late
    // joiner permanently behind it. A host steps an idle server anyway only
    // when its per-tick deadline says so — the library reports readiness, the
    // host owns the policy.
    DedicatedServerConfig config;
    DedicatedServer server(config);
    NF_CHECK(server.start());
    NF_CHECK(server.local_port() != 0);
    NF_CHECK(!server.all_inputs_in_for(0));
    NF_CHECK(server.player_count() == 0);
    // A host that steps anyway (deadline passed) still advances cleanly.
    server.tick(0);
    NF_CHECK(server.tick_index() == 1);
    NF_CHECK(server.refused_connections() == 0);
    NF_CHECK(server.malformed_packets() == 0);
    server.stop();
    NF_CHECK(!server.is_running());
}

NF_TEST(dedicated_server_provisions_a_player_on_first_contact) {
    // "Connecting" is implicit on UDP: the client's first datagram claims a
    // player slot. The server names the entity (id 1, the first slot), so a
    // PredictedClient(1) sees its own entity in the first snapshot it gets.
    DedicatedServerConfig config;
    DedicatedServer server(config);
    NF_CHECK(server.start());

    LoopbackClient client(1, server.local_port());
    NF_CHECK(server.player_count() == 0);
    for (u32 t = 0; t < 60; ++t) {
        const u64 now = static_cast<u64>(t) * 20;
        client.push_input(1.0f, 0.0f, now);
        server.pump(now); // provisions on t == 0, submits input t
        if (t == 0) {
            NF_CHECK(server.player_count() == 1); // the first packet connected
            NF_CHECK(server.tick_index() == 0);   // pump never steps the world
        }
        NF_CHECK(server.all_inputs_in_for(t));
        server.tick(now); // step + broadcast snapshot t
        Snapshot snap;
        NF_CHECK(client.wait_snapshot(t, snap));
        client.client().on_snapshot(snap);
    }

    // 60 ticks of full throttle at 6 u/s from the spawn origin: the server and
    // the client agree to the last digit, and no correction ever had to happen.
    float sx = 0, sy = 0, sz = 0, cx = 0, cy = 0, cz = 0;
    NF_CHECK(server.player_position(1, sx, sy, sz));
    NF_CHECK(client.client().predicted_position(cx, cy, cz));
    NF_CHECK_NEAR(cx, sx, 1e-3f);
    NF_CHECK_NEAR(cx, 6.0f, 1e-3f);
    NF_CHECK_NEAR(client.client().last_correction(), 0.0f, 1e-3f);
    NF_CHECK(server.malformed_packets() == 0);
    server.stop();
}

NF_TEST(dedicated_server_tracks_two_clients_on_one_socket) {
    // Both connections share the server's single socket, demultiplexed by
    // source port. Each client must see only its own entity move and the other
    // arrive as a ghost — the same invariant the two-client loopback test
    // holds, but with the player slots provisioned by the server itself.
    DedicatedServerConfig config;
    DedicatedServer server(config);
    NF_CHECK(server.start());

    LoopbackClient a(1, server.local_port());
    LoopbackClient b(2, server.local_port());
    for (u32 t = 0; t < 60; ++t) {
        const u64 now = static_cast<u64>(t) * 20;
        a.push_input(1.0f, 0.0f, now);
        b.push_input(0.0f, 1.0f, now);
        server.pump(now);
        if (t == 0) NF_CHECK(server.player_count() == 2);
        NF_CHECK(server.all_inputs_in_for(t));
        server.tick(now);
        Snapshot sa, sb;
        NF_CHECK(a.wait_snapshot(t, sa));
        NF_CHECK(b.wait_snapshot(t, sb));
        a.client().on_snapshot(sa);
        b.client().on_snapshot(sb);
    }

    float ax = 0, ay = 0, az = 0, bx = 0, by = 0, bz = 0;
    NF_CHECK(server.player_position(1, ax, ay, az));
    NF_CHECK(server.player_position(2, bx, by, bz));
    NF_CHECK_NEAR(ax, 6.0f, 1e-3f); // slot 1 spawned at the origin, drove +x
    NF_CHECK_NEAR(bx, 1.0f, 1e-3f); // slot 2 spawned at x+1 and never moved in x
    NF_CHECK_NEAR(bz, 6.0f, 1e-3f); // ...it drove +z

    // Client a's own prediction is right about itself, AND its ghost of b
    // matches the server: a client that only matched its own entity could be
    // showing every other player in the wrong place.
    float pa_x = 0, pa_y = 0, pa_z = 0, ga_x = 0, ga_y = 0, ga_z = 0;
    NF_CHECK(a.client().predicted_position(pa_x, pa_y, pa_z));
    NF_CHECK(a.client().world().position(2, ga_x, ga_y, ga_z));
    NF_CHECK_NEAR(pa_x, ax, 1e-3f);
    NF_CHECK_NEAR(ga_x, bx, 1e-3f);
    NF_CHECK_NEAR(ga_z, bz, 1e-3f);
    server.stop();
}

NF_TEST(dedicated_server_refuses_clients_past_capacity) {
    DedicatedServerConfig config;
    config.max_players = 1;
    DedicatedServer server(config);
    NF_CHECK(server.start());

    LoopbackClient a(1, server.local_port());
    LoopbackClient b(2, server.local_port());
    a.push_input(1.0f, 0.0f, 0);
    server.pump(0);
    NF_CHECK(server.player_count() == 1);
    // The server is full: b's first packet is counted as a refusal and b never
    // earns a slot — connecting cannot evict a player already in the game.
    b.push_input(1.0f, 0.0f, 1);
    server.pump(1);
    NF_CHECK(server.player_count() == 1);
    NF_CHECK(server.refused_connections() == 1);
    server.stop();
}

NF_TEST(dedicated_server_counts_malformed_packets) {
    // A truncated channel packet is not a crash: it is counted and dropped, so
    // a bad actor (or a version skew) shows up in telemetry instead of killing
    // the tick. Junk does not earn a player slot either.
    DedicatedServerConfig config;
    DedicatedServer server(config);
    NF_CHECK(server.start());

    UdpSocket client_sock;
    NF_CHECK(client_sock.open(0, true, nullptr));
    const u8 junk[] = {'N', 'F', 'C', 'H', 1, 0, 0}; // magic, then garbage
    NF_CHECK(client_sock.send_to(junk, sizeof(junk), kIpv4Loopback, server.local_port()));
    server.pump(0);
    NF_CHECK(server.malformed_packets() == 1);
    NF_CHECK(server.player_count() == 0);
    server.stop();
}

NF_TEST(client_connects_to_the_headless_server_binary) {
    // The §81 deliverable: the REAL binary, started headless on an ephemeral
    // port, with a real client connecting to it over real UDP. Not a library
    // call — a separate process that links no renderer, creates no window and
    // touches no GPU, and the two still stay in lockstep for 60 ticks.
#ifdef NF_SERVER_EXE
    const std::string exe = NF_SERVER_EXE;
#else
    const std::string exe;
#endif
    if (exe.empty() || !std::filesystem::exists(exe)) {
        NF_SKIP("NOVAForgeServer binary not built for this configuration");
        return;
    }

    // Start the server and let it choose a port (tests may run in parallel; a
    // fixed port would collide). It prints "PORT <n>" on stdout once bound.
    // The budget is exactly the 60 ticks this exchange runs: a server handed a
    // larger budget outlives the client and then steps every remaining tick on
    // the deadline alone, so _pclose blocks long after the game is over.
    const std::string cmd = "\"" + exe + "\" --ticks 60 --tick-deadline-ms 500";
#ifdef _MSC_VER
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    NF_CHECK(pipe != nullptr);
    u16 server_port = 0;
    {
        char line[128] = {0};
        NF_CHECK(std::fgets(line, sizeof(line), pipe) != nullptr);
        // sscanf is C4996 under /WX; from_chars is the engine's own idiom
        // (InputMapper, Reflection) and refuses a bad line instead of silently
        // leaving `parsed` uninitialised.
        const std::string printed(line);
        const std::string kPrefix = "PORT ";
        if (printed.rfind(kPrefix, 0) == 0) {
            const auto rest = std::string_view(printed).substr(kPrefix.size());
            int parsed = 0;
            if (std::from_chars(rest.data(), rest.data() + rest.size(), parsed).ec == std::errc()) {
                server_port = static_cast<u16>(parsed);
            }
        }
        NF_CHECK(server_port != 0);
    }

    // The client connects. The server provisions it on first contact, the tick
    // gate admits the input, and the exchange goes lockstep across the process
    // boundary: send input t, wait for snapshot t, repeat.
    LoopbackClient client(1, server_port);
    for (u32 t = 0; t < 60; ++t) {
        const u64 now = static_cast<u64>(t) * 20;
        client.push_input(1.0f, 0.0f, now);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Snapshot snap;
        NF_CHECK(client.wait_snapshot(t, snap));
        client.client().on_snapshot(snap);
    }

    float cx = 0, cy = 0, cz = 0;
    NF_CHECK(client.client().predicted_position(cx, cy, cz));
    NF_CHECK_NEAR(cx, 6.0f, 1e-3f); // 60 ticks at 6 u/s, reconciled against the server
    // The prediction was exact: the client never needed a snap against the
    // server's authority over the whole run.
    NF_CHECK_NEAR(client.client().last_correction(), 0.0f, 1e-2f);

#ifdef _MSC_VER
    const int rc = _pclose(pipe);
#else
    const int rc = pclose(pipe);
#endif
    NF_CHECK(rc == 0); // the server exited cleanly when its tick budget ran out
}
