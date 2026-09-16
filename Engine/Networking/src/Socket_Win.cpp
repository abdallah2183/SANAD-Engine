// NF/Networking/Socket_Win.cpp — WinSock2 UDP transport (Windows).

#include <NF/Networking/Socket.hpp>

#include <NF/Core/Logger.hpp>

#include <atomic>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace nf::net {

namespace {

std::mutex& winsock_mutex() {
    static std::mutex m;
    return m;
}

std::atomic<int>& winsock_refs() {
    static std::atomic<int> refs{0};
    return refs;
}

u32 swap_u32(u32 v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v & 0xFF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}

u16 swap_u16(u16 v) {
    return static_cast<u16>(((v & 0xFFu) << 8) | ((v & 0xFF00u) >> 8));
}

} // namespace

bool SocketSubsystem::ensure_initialized(std::string& out_error) {
    std::lock_guard lock(winsock_mutex());
    if (winsock_refs().load() > 0) {
        winsock_refs().fetch_add(1);
        return true;
    }
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        out_error = "WSAStartup failed";
        return false;
    }
    winsock_refs().store(1);
    return true;
}

void SocketSubsystem::shutdown() {
    std::lock_guard lock(winsock_mutex());
    if (winsock_refs().fetch_sub(1) <= 1) {
        winsock_refs().store(0);
        WSACleanup();
    }
}

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept {
    m_open = other.m_open;
    m_local_port = other.m_local_port;
    m_socket = other.m_socket;
    other.m_open = false;
    other.m_local_port = 0;
    other.m_socket = nullptr;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        m_open = other.m_open;
        m_local_port = other.m_local_port;
        m_socket = other.m_socket;
        other.m_open = false;
        other.m_local_port = 0;
        other.m_socket = nullptr;
    }
    return *this;
}

bool UdpSocket::open(u16 port, bool loopback_only, std::string* out_error) {
    if (m_open) {
        if (out_error) *out_error = "socket already open";
        return false;
    }
    std::string init_err;
    if (!SocketSubsystem::ensure_initialized(init_err)) {
        if (out_error) *out_error = init_err;
        return false;
    }
    bool ok = false;
    SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        if (out_error) *out_error = "socket() failed";
    } else {
        // Non-blocking: recv_from returns immediately when empty.
        u_long nonblocking = 1;
        if (ioctlsocket(sock, FIONBIO, &nonblocking) != 0) {
            if (out_error) *out_error = "ioctlsocket() failed";
            ::closesocket(sock);
        } else {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = loopback_only ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
            if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                if (out_error) *out_error = "bind() failed (port in use?)";
                ::closesocket(sock);
            } else {
                // Read back the bound port (ephemeral when port == 0).
                sockaddr_in bound{};
                int len = sizeof(bound);
                if (getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
                    m_local_port = ntohs(bound.sin_port);
                } else {
                    m_local_port = port;
                }
                m_socket = reinterpret_cast<void*>(sock);
                m_open = true;
                ok = true;
            }
        }
    }
    if (!ok) SocketSubsystem::shutdown(); // balance the init ref
    return ok;
}

void UdpSocket::close() {
    if (!m_open) return;
    ::closesocket(reinterpret_cast<SOCKET>(m_socket));
    m_socket = nullptr;
    m_open = false;
    m_local_port = 0;
    SocketSubsystem::shutdown();
}

bool UdpSocket::send_to(const u8* data, usize size, u32 ipv4_host_order, u16 port) {
    if (!m_open || !data || size == 0 || size > 65507) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(ipv4_host_order);
    const int sent = ::sendto(reinterpret_cast<SOCKET>(m_socket),
                              reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
                              reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (sent < 0) {
        const int err = WSAGetLastError();
        // Host-unreachable on loopback just means nobody listens (ICMP); for
        // fire-and-forget UDP that is not a local failure... but Winsock
        // reports it on the NEXT call, so treat WOULDBLOCK-style codes plus
        // the unreachable family as soft: the datagram is gone either way.
        if (err == WSAEWOULDBLOCK || err == WSAECONNRESET || err == WSAECONNREFUSED ||
            err == WSAEHOSTUNREACH) {
            return true;
        }
        NF_LOG_WARN(LogCategory::Network, "UdpSocket::send_to failed ({})", err);
        return false;
    }
    return static_cast<usize>(sent) == size;
}

bool UdpSocket::recv_from(Datagram& out) {
    out.payload.clear();
    out.from_ip = 0;
    out.from_port = 0;
    if (!m_open) return false;
    // 64KB: maximum UDP payload; larger datagrams cannot exist.
    static thread_local std::vector<char> buffer(65535);
    sockaddr_in from{};
    int from_len = sizeof(from);
    const int got = ::recvfrom(reinterpret_cast<SOCKET>(m_socket), buffer.data(),
                               static_cast<int>(buffer.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &from_len);
    if (got <= 0) return false; // empty queue (WSAEWOULDBLOCK) or error
    out.payload.assign(reinterpret_cast<u8*>(buffer.data()),
                       reinterpret_cast<u8*>(buffer.data()) + got);
    out.from_ip = swap_u32(static_cast<u32>(from.sin_addr.s_addr));
    out.from_port = swap_u16(static_cast<u16>(from.sin_port));
    return true;
}

} // namespace nf::net
