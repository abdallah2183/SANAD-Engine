#pragma once

// NF/Networking/Socket.hpp — non-blocking UDP transport (Phase 16 kickoff).
//
// A thin RAII wrapper over the OS datagram socket (WinSock2 on Windows):
// open/bind, fire-and-forget send_to, non-blocking recv_from. No threads,
// no reliability layer yet — that builds on these datagrams (sequence/ack
// headers live one layer up, see the multiplayer roadmap).
//
// Addresses are host-order IPv4 integers (0x7F000001 == 127.0.0.1) and ports
// are host-order u16; conversion happens inside, so call sites stay readable.

#include <NF/Core/Types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace nf::net {

/// 127.0.0.1 in host byte order.
inline constexpr u32 kIpv4Loopback = 0x7F000001u;

struct Datagram {
    std::vector<u8> payload;
    u32 from_ip = 0;
    u16 from_port = 0;
};

/// Process-wide socket subsystem (WSAStartup/WSACleanup). Reference-counted
/// and thread-safe; sockets ensure it is initialized on open().
class SocketSubsystem {
public:
    static bool ensure_initialized(std::string& out_error);
    static void shutdown();
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    /// Opens a non-blocking UDP socket bound to `port` (0 = ephemeral).
    /// Loopback-only binds when loopback_only is true (tests/default).
    bool open(u16 port = 0, bool loopback_only = true, std::string* out_error = nullptr);
    void close();
    bool is_open() const { return m_open; }
    u16 local_port() const { return m_local_port; }

    /// Sends one datagram (UDP: lossy, unordered, fire-and-forget).
    /// Returns false only on local errors (not open, message too large...);
    /// delivery itself is never guaranteed.
    bool send_to(const u8* data, usize size, u32 ipv4_host_order, u16 port);
    bool send_to(const std::vector<u8>& data, u32 ipv4_host_order, u16 port) {
        return send_to(data.data(), data.size(), ipv4_host_order, port);
    }

    /// Receives one datagram when available. Returns false when the queue
    /// is empty (normal for non-blocking I/O) or on error.
    bool recv_from(Datagram& out);

    /// Largest safe payload: stays under the IPv4 minimum reassembly size
    /// (576) minus headers, so datagrams survive routers unfragmented.
    static constexpr usize kMaxSafePayload = 512;

private:
    bool m_open = false;
    u16 m_local_port = 0;
#if defined(_WIN32)
    void* m_socket = nullptr; // SOCKET, untyped to keep windows.h out
#else
    int m_socket = -1;
#endif
};

} // namespace nf::net
