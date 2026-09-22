// NF/Networking/DedicatedServer.cpp — headless authoritative server host.

#include <NF/Networking/DedicatedServer.hpp>

#include <utility>

namespace nf::net {

namespace {

// tick_less(a, b): a is strictly before b in u32 wrap-aware order. Kept local
// because this module's tick comparison convention lives where it is used.
bool tick_less(u32 a, u32 b) {
    return a != b && static_cast<u32>(b - a) < 0x80000000u;
}

} // namespace

DedicatedServer::DedicatedServer(const DedicatedServerConfig& config)
    : m_config(config), m_world(config.fixed_dt) {}

DedicatedServer::~DedicatedServer() { stop(); }

bool DedicatedServer::start(std::string* out_error) {
    if (m_running) return true;
    if (!m_socket.open(m_config.port, m_config.loopback_only, out_error)) {
        return false;
    }
    m_running = true;
    return true;
}

void DedicatedServer::stop() {
    if (!m_running) return;
    m_socket.close();
    m_running = false;
}

bool DedicatedServer::provision(u32 ip, u16 port) {
    if (m_players.size() >= m_config.max_players) {
        ++m_refused;
        return false;
    }
    const u32 id = m_next_player++;
    Client c;
    c.player_id = id;
    c.ip = ip;
    c.port = port;
    // Player n spawns at spawn + (n-1, 0, 0): the first client lands exactly on
    // the spawn point, and two connecting clients never share a position —
    // their snapshots would be indistinguishable.
    m_world.add_player(id, m_config.spawn_x + static_cast<float>(id - 1),
                       m_config.spawn_y, m_config.spawn_z);
    m_players.emplace(id, std::move(c));
    m_player_by_port.emplace(port, id);
    return true;
}

DedicatedServer::Client* DedicatedServer::client_by_port(u16 port) {
    auto it = m_player_by_port.find(port);
    if (it == m_player_by_port.end()) return nullptr;
    auto pit = m_players.find(it->second);
    return pit == m_players.end() ? nullptr : &pit->second;
}

void DedicatedServer::pump(u64 now_ms) {
    if (!m_running) return;
    for (;;) {
        Datagram d;
        if (!m_socket.recv_from(d)) break;

        // Decode BEFORE provisioning: a stranger's datagram only claims a player
        // slot if it is actually one of our packets. Junk earns a malformed
        // count, not a reservation — otherwise a scanner spraying garbage at the
        // port exhausts max_players one byte at a time.
        NetPacket pkt;
        std::string err;
        if (!decode_packet(d.payload.data(), d.payload.size(), pkt, err)) {
            ++m_malformed;
            continue;
        }

        Client* client = client_by_port(d.from_port);
        if (client == nullptr) {
            // First contact: this datagram IS a client connecting. Provision
            // the next player id and bind the connection to this source port.
            // A full server drops the stranger (counted as a refusal) rather
            // than evicting a player already in the game.
            if (!provision(d.from_ip, d.from_port)) continue;
            client = client_by_port(d.from_port);
            if (client == nullptr) continue;
        }

        for (const auto& payload : client->channel.receive(pkt)) {
            NetInput in;
            if (!decode_input(payload.data(), payload.size(), in, err)) {
                ++m_malformed;
                continue;
            }
            m_world.submit_input(client->player_id, in);
            client->last_submitted_tick = in.tick;
            client->has_submitted = true;
        }
    }

    // Acknowledge every client over its own channel: acks must travel back the
    // socket they came in on, or a client's send queue never drains.
    for (auto& [id, client] : m_players) {
        (void)id;
        if (client.port == 0) continue; // provisioned but never sent
        for (const auto& pkt : client.channel.poll_outgoing(now_ms)) {
            m_socket.send_to(encode_packet(pkt), client.ip, client.port);
        }
    }
}

void DedicatedServer::tick(u64 now_ms) {
    pump(now_ms);
    m_world.tick();
    const std::vector<u8> bytes = encode_snapshot(m_world.last_snapshot());
    for (auto& [id, client] : m_players) {
        (void)id;
        if (client.port == 0) continue;
        m_socket.send_to(bytes, client.ip, client.port);
    }
}

bool DedicatedServer::player_position(u32 player_id, float& x, float& y, float& z) const {
    return m_world.world().position(player_id, x, y, z);
}

bool DedicatedServer::all_inputs_in_for(u32 tick) const {
    // A server with no client connected HOLDS: it must not burn its tick clock
    // before the first player arrives, or a late-joining client can never catch
    // up to a server already far ahead of it. The host's per-tick deadline is
    // what lets an idle server proceed anyway; the library reports readiness,
    // the host owns the policy.
    if (m_players.empty()) return false;
    for (const auto& [id, client] : m_players) {
        (void)id;
        // A player that has never sent anything is not holding the tick up:
        // the host's deadline covers it, not this gate.
        if (!client.has_submitted) continue;
        // Satisfied when the client has delivered the wanted tick OR already
        // moved past it (inputs arrive in order per client).
        if (tick_less(client.last_submitted_tick, tick)) return false;
    }
    return true;
}

} // namespace nf::net
