// NF/Networking/Socket_Stub.cpp — closed-socket transport (non-Windows).
//
// The engine ships on Windows first (WinSock2 in Socket_Win.cpp); this TU
// keeps every other platform compiling with a transport that reports
// "not open" until a POSIX backend lands. Nothing here can send or receive.

#include <NF/Networking/Socket.hpp>

namespace nf::net {

bool SocketSubsystem::ensure_initialized(std::string& out_error) {
    out_error = "socket subsystem has no backend on this platform";
    return false;
}

void SocketSubsystem::shutdown() {}

UdpSocket::~UdpSocket() = default;
UdpSocket::UdpSocket(UdpSocket&& other) noexcept = default;
UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept = default;

bool UdpSocket::open(u16, bool, std::string* out_error) {
    if (out_error) *out_error = "UDP sockets need the Windows backend";
    return false;
}

void UdpSocket::close() {}

bool UdpSocket::send_to(const u8*, usize, u32, u16) {
    return false;
}

bool UdpSocket::recv_from(Datagram&) {
    return false;
}

} // namespace nf::net
