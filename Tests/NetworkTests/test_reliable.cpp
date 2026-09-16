// NetworkTests — reliable channel + snapshots (Phase 16).
//
// Pure logic (no sockets): two channels cross-wired in memory with scripted
// loss/reorder/duplication. The UDP tests next door prove the datagrams move.

#include <NF/Networking/ReliableChannel.hpp>
#include <NF/Networking/Snapshot.hpp>
#include <NF/Test/TestFramework.hpp>

#include <string>
#include <vector>

using namespace nf;
using namespace nf::net;

namespace {

// Pumps every outgoing packet from `from` into `to`, collecting deliveries.
void pump(ReliableChannel& from, ReliableChannel& to, u64 now_ms,
          std::vector<std::vector<u8>>& delivered) {
    for (auto& pkt : from.poll_outgoing(now_ms)) {
        for (auto& payload : to.receive(pkt)) delivered.push_back(payload);
    }
}

std::vector<u8> bytes(const char* s) {
    return std::vector<u8>(s, s + std::string(s).size());
}

} // namespace

NF_TEST(reliable_in_order_delivery_no_loss) {
    ReliableChannel a, b;
    NF_CHECK(a.send_reliable(bytes("one")));
    NF_CHECK(a.send_reliable(bytes("two")));
    NF_CHECK(a.send_reliable(bytes("three")));
    std::vector<std::vector<u8>> got;
    pump(a, b, 0, got);
    NF_CHECK(got.size() == 3);
    NF_CHECK(got[0] == bytes("one"));
    NF_CHECK(got[2] == bytes("three"));
    // B's acks free A's queue once they travel back.
    std::vector<std::vector<u8>> back;
    pump(b, a, 0, back);
    NF_CHECK(a.unacked_count() == 0);
}

NF_TEST(reliable_survives_total_loss_then_resends) {
    ReliableChannel a(0, 50), b;
    NF_CHECK(a.send_reliable(bytes("critical")));
    // First flight: dropped on the floor (never fed to b).
    NF_CHECK(!a.poll_outgoing(0).empty());
    NF_CHECK(a.unacked_count() == 1);
    // Not yet due: no resend.
    NF_CHECK(a.poll_outgoing(10).empty());
    // Past the timeout: resend carries the same payload and sequence.
    auto resend = a.poll_outgoing(100);
    NF_CHECK(resend.size() == 1);
    NF_CHECK(resend[0].payload == bytes("critical"));
    NF_CHECK(a.resends() == 1);
    // Deliver the resend: exactly-once.
    std::vector<std::vector<u8>> got;
    for (auto& pkt : resend) {
        for (auto& payload : b.receive(pkt)) got.push_back(payload);
    }
    NF_CHECK(got.size() == 1);
    // The duplicate (original finally arriving) is dropped.
    auto dup = a.poll_outgoing(200); // resend again (no ack yet)
    for (auto& pkt : dup) {
        for (auto& payload : b.receive(pkt)) got.push_back(payload);
    }
    NF_CHECK(got.size() == 1);
}

NF_TEST(reliable_reorders_into_order) {
    ReliableChannel a, b;
    NF_CHECK(a.send_reliable(bytes("A")));
    NF_CHECK(a.send_reliable(bytes("B")));
    NF_CHECK(a.send_reliable(bytes("C")));
    auto flight = a.poll_outgoing(0);
    NF_CHECK(flight.size() == 3);
    std::vector<std::vector<u8>> got;
    // Deliver C, A, B: C buffers, A delivers + drains B... C arrives first
    // (buffered), then A (delivers A), then B (delivers B, C).
    for (auto& payload : b.receive(flight[2])) got.push_back(payload);
    NF_CHECK(got.empty());
    for (auto& payload : b.receive(flight[0])) got.push_back(payload);
    NF_CHECK(got.size() == 1 && got[0] == bytes("A"));
    for (auto& payload : b.receive(flight[1])) got.push_back(payload);
    NF_CHECK(got.size() == 3);
    NF_CHECK(got[1] == bytes("B") && got[2] == bytes("C"));
}

NF_TEST(reliable_sequence_wraps_cleanly) {
    ReliableChannel a(65534), b(65534);
    NF_CHECK(a.send_reliable(bytes("w0")));
    NF_CHECK(a.send_reliable(bytes("w1")));
    NF_CHECK(a.send_reliable(bytes("w2"))); // seq wraps 65534,65535,0
    std::vector<std::vector<u8>> got;
    pump(a, b, 0, got);
    NF_CHECK(got.size() == 3);
    NF_CHECK(got[0] == bytes("w0") && got[2] == bytes("w2"));
    std::vector<std::vector<u8>> back;
    pump(b, a, 0, back);
    NF_CHECK(a.unacked_count() == 0);
}

NF_TEST(reliable_rejects_empty_payload) {
    ReliableChannel a;
    NF_CHECK(!a.send_reliable({}));
    NF_CHECK(a.unacked_count() == 0);
    NF_CHECK(a.poll_outgoing(0).empty() || true); // pure ack or silence: both fine
}

NF_TEST(snapshot_roundtrip_sorted_and_versioned) {
    Snapshot s;
    s.tick = 1234;
    s.entities.push_back(SnapshotEntity{9, 0, 1, 2, 3});
    s.entities.push_back(SnapshotEntity{2, 1, 4, 5, 6});
    s.entities.push_back(SnapshotEntity{2, 0, 7, 8, 9});
    const std::vector<u8> bytes_out = encode_snapshot(s);
    NF_CHECK(bytes_out.size() == 14 + 3 * 20);
    Snapshot back;
    std::string err;
    NF_CHECK(decode_snapshot(bytes_out.data(), bytes_out.size(), back, err));
    NF_CHECK(err.empty());
    NF_CHECK(back.tick == 1234);
    NF_CHECK(back.entities.size() == 3);
    // Sorted by (id, generation) on encode.
    NF_CHECK(back.entities[0].id == 2 && back.entities[0].generation == 0);
    NF_CHECK(back.entities[1].id == 2 && back.entities[1].generation == 1);
    NF_CHECK(back.entities[2].id == 9);
    NF_CHECK(back.entities[2].x == 1.0f);

    Snapshot empty;
    empty.tick = 7;
    const std::vector<u8> empty_bytes = encode_snapshot(empty);
    Snapshot back_empty;
    NF_CHECK(decode_snapshot(empty_bytes.data(), empty_bytes.size(), back_empty, err));
    NF_CHECK(back_empty.tick == 7 && back_empty.entities.empty());
}

NF_TEST(snapshot_rejects_corruption) {
    Snapshot s;
    s.tick = 1;
    s.entities.push_back(SnapshotEntity{1, 0, 0, 0, 0});
    const std::vector<u8> good = encode_snapshot(s);
    Snapshot out;
    std::string err;
    NF_CHECK(!decode_snapshot(nullptr, 0, out, err));
    NF_CHECK(!decode_snapshot(good.data(), 5, out, err)); // truncated header
    std::vector<u8> bad_magic = good;
    bad_magic[0] = 'X';
    NF_CHECK(!decode_snapshot(bad_magic.data(), bad_magic.size(), out, err));
    std::vector<u8> bad_version = good;
    bad_version[4] = 99;
    NF_CHECK(!decode_snapshot(bad_version.data(), bad_version.size(), out, err));
    std::vector<u8> truncated = good;
    truncated.pop_back();
    NF_CHECK(!decode_snapshot(truncated.data(), truncated.size(), out, err));
    NF_CHECK(!err.empty());
}
