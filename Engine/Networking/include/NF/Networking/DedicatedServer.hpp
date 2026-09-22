#pragma once

// NF/Networking/DedicatedServer.hpp — headless authoritative server host
// (design §81: NOVAForgeServer must build without Graphics, Editor or GPU
// resources).
//
// AuthoritativeServer is the SIMULATION; this class is the HOST. It owns the
// pieces the simulation deliberately knows nothing about:
//   - one UdpSocket bound to a port (loopback-only by default, exactly like
//     the tests; a production host passes a real address),
//   - one ReliableChannel per connected client, demultiplexed by source port
//     because every connection shares the server's single socket,
//   - player provisioning: a client's first datagram claims the next free
//     player slot (a net entity id), so "connecting" is implicit in UDP,
//     which is the honest model for a connectionless transport.
//
// What it never owns: a window, a Vulkan device, a renderer, a scene file. It
// links NFCore and NFNetworking only, which is what makes the dedicated server
// buildable headless.
//
// Tick pacing (the part a host must get right, and the reason the API is split
// the way it is): a dedicated server cannot simply tick at a fixed rate, or it
// races its clients — it would step tick 5 while a client's input for tick 5 is
// still in flight, apply neutral, and that client's snapshot would never come.
// So the host calls pump() (drain sockets, send acks, no simulation step) until
// all_inputs_in_for(tick) reports every connected client has delivered that
// tick's input, and only then tick(). A deadline in the host covers a stalled
// or absent client; the library reports the readiness, the host owns the
// policy. An empty server (no clients yet) is always ready, so a freshly
// started server still advances.

#include <NF/Networking/Authoritative.hpp>
#include <NF/Networking/ReliableChannel.hpp>
#include <NF/Networking/Snapshot.hpp>
#include <NF/Networking/Socket.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace nf::net {

struct DedicatedServerConfig {
    u16 port = 0;                   // 0 = ephemeral (the host reads local_port())
    bool loopback_only = true;      // a public host sets this false
    float fixed_dt = 1.0f / 60.0f;  // shared tick rate, client and server
    u32 max_players = 16;           // beyond this, a new client is refused
    float spawn_x = 0.0f;           // where a newly provisioned player appears
    float spawn_y = 0.0f;           // (player n is offset to x + (n-1), so two
    float spawn_z = 0.0f;           // connecting clients never stack)
};

class DedicatedServer {
public:
    explicit DedicatedServer(const DedicatedServerConfig& config = {});
    ~DedicatedServer();

    DedicatedServer(const DedicatedServer&) = delete;
    DedicatedServer& operator=(const DedicatedServer&) = delete;

    /// Binds the socket. Returns false and fills `out_error` on a bind failure.
    bool start(std::string* out_error = nullptr);
    void stop();
    bool is_running() const { return m_running; }
    u16 local_port() const { return m_socket.local_port(); }

    /// Pumps the socket without stepping the simulation: drains incoming
    /// datagrams into the per-client channels (provisioning a player on first
    /// contact), submits decoded inputs, and flushes acks back to each client.
    /// Idempotent and safe to call in a tight poll loop.
    void pump(u64 now_ms);

    /// One authoritative tick: pump, step the shared world, broadcast the
    /// resulting snapshot to every connected client as a raw datagram (loss is
    /// fine — the next tick supersedes).
    void tick(u64 now_ms);

    /// True when every connected client has delivered its input for `tick`.
    /// A server with NO client connected reports false on purpose: it must not
    /// burn its tick clock before the first player arrives, or a late-joining
    /// client can never catch up. The host's per-tick deadline is what lets an
    /// idle server proceed anyway — the library reports readiness, the host
    /// owns the policy. A client that has delivered PAST the wanted tick also
    /// satisfies the gate (inputs arrive in order per client), so a burst of
    /// packets does not stall it.
    bool all_inputs_in_for(u32 tick) const;

    const AuthoritativeServer& sim() const { return m_world; }
    /// Authoritative position of a player, for a host-side query or a test.
    bool player_position(u32 player_id, float& x, float& y, float& z) const;
    u32 tick_index() const { return m_world.tick_index(); }
    u32 player_count() const { return static_cast<u32>(m_players.size()); }
    u32 refused_connections() const { return m_refused; }
    u32 malformed_packets() const { return m_malformed; }

private:
    struct Client {
        u32 player_id = 0;
        u32 ip = 0;
        u16 port = 0;
        ReliableChannel channel;
        u32 last_submitted_tick = 0;
        bool has_submitted = false;
    };

    Client* client_by_port(u16 port);
    bool provision(u32 ip, u16 port);

    DedicatedServerConfig m_config;
    UdpSocket m_socket;
    AuthoritativeServer m_world;
    std::unordered_map<u32, Client> m_players;   // player id -> connection
    std::unordered_map<u16, u32> m_player_by_port; // source port -> player id
    u32 m_next_player = 1;
    u32 m_refused = 0;
    u32 m_malformed = 0;
    bool m_running = false;
};

} // namespace nf::net
