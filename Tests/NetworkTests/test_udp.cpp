// NetworkTests — UDP loopback transport (Phase 16 kickoff).
//
// Two sockets on 127.0.0.1, ephemeral ports: send/recv fidelity, sender
// identity, empty-queue behavior, and lifecycle rules. Headless-safe: no
// external network, no firewall surface beyond loopback.

#include <NF/Test/TestFramework.hpp>
#include <NF/Networking/Socket.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace nf;
using namespace nf::net;

NF_TEST(udp_loopback_send_recv) {
    UdpSocket a;
    UdpSocket b;
    std::string err;
    NF_CHECK(a.open(0, true, &err));
    NF_CHECK(b.open(0, true, &err));
    NF_CHECK(a.is_open() && b.is_open());
    NF_CHECK(a.local_port() != 0 && b.local_port() != 0);
    NF_CHECK(a.local_port() != b.local_port());

    const std::vector<u8> msg = {'h', 'e', 'l', 'l', 'o'};
    NF_CHECK(a.send_to(msg, kIpv4Loopback, b.local_port()));

    // Loopback delivery is fast but not synchronous: poll briefly.
    Datagram d;
    bool got = false;
    for (int i = 0; i < 200 && !got; ++i) {
        got = b.recv_from(d);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    NF_CHECK(got);
    NF_CHECK(d.payload == msg);
    NF_CHECK(d.from_ip == kIpv4Loopback);
    NF_CHECK(d.from_port == a.local_port());
}

NF_TEST(udp_empty_queue_reads_false) {
    UdpSocket a;
    NF_CHECK(a.open(0, true, nullptr));
    Datagram d;
    NF_CHECK(!a.recv_from(d)); // nothing sent: normal, not an error
    NF_CHECK(d.payload.empty());
}

NF_TEST(udp_lifecycle_rules) {
    UdpSocket a;
    Datagram d;
    NF_CHECK(!a.is_open());
    NF_CHECK(!a.recv_from(d)); // closed socket never receives
    const std::vector<u8> msg = {1};
    NF_CHECK(!a.send_to(msg, kIpv4Loopback, 1234)); // closed socket never sends
    NF_CHECK(!a.send_to(nullptr, 0, kIpv4Loopback, 1234)); // empty send rejected

    NF_CHECK(a.open(0, true, nullptr));
    NF_CHECK(a.is_open());
    std::string err;
    NF_CHECK(!a.open(0, true, &err)); // double open rejected
    NF_CHECK(!err.empty());
    a.close();
    NF_CHECK(!a.is_open());
    NF_CHECK(a.local_port() == 0);
    a.close(); // double close: safe no-op

    // Move transfers ownership; the donor goes quiet.
    UdpSocket m;
    NF_CHECK(m.open(0, true, nullptr));
    const u16 port = m.local_port();
    UdpSocket moved = std::move(m);
    NF_CHECK(moved.is_open());
    NF_CHECK(moved.local_port() == port);
    NF_CHECK(!m.is_open());
}

NF_TEST(udp_fire_and_forget_to_nobody) {
    UdpSocket a;
    UdpSocket b;
    NF_CHECK(a.open(0, true, nullptr));
    NF_CHECK(b.open(0, true, nullptr));
    // Nobody listens on b's port+1... UDP does not care: the send succeeds,
    // nothing arrives anywhere.
    const std::vector<u8> msg = {'x'};
    NF_CHECK(a.send_to(msg, kIpv4Loopback, static_cast<u16>(b.local_port() + 1)));
    Datagram d;
    NF_CHECK(!b.recv_from(d));
}

NF_TEST(udp_max_safe_payload_constant) {
    NF_CHECK(UdpSocket::kMaxSafePayload == 512);
    NF_CHECK(kIpv4Loopback == 0x7F000001u);
}
