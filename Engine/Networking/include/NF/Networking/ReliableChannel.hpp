#pragma once

// NF/Networking/ReliableChannel.hpp — in-order reliable stream over datagrams
// (Phase 16: replication transport).
//
// One class drives both directions: the sender assigns sequence numbers and
// resends unacked payloads past the timeout; the receiver buffers
// out-of-order packets, delivers in order, drops duplicates, and reports
// cumulative ack + 32-bit ack history, which the sender consumes to free its
// queue. Pure logic, no sockets: unit tests drive it directly, and the game
// pumps poll_outgoing() into UdpSocket and receive() out of it.
//
// Sequence arithmetic is u16 with 0x8000-window comparison, so streams wrap
// safely every 65536 packets.

#include <NF/Core/Types.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nf::net {

struct NetPacket {
    u16 seq = 0; // this packet's sequence
    u16 ack = 0; // highest in-order sequence received by the sender
    u32 ack_bits = 0; // bit n set ⟺ packet (ack - 1 - n) was received
    std::vector<u8> payload; // empty payloads are pure acks (never queued)
};

class ReliableChannel {
public:
    explicit ReliableChannel(u16 initial_seq = 0, u64 resend_timeout_ms = 100);

    /// Queues a payload for reliable delivery (assigned the next sequence).
    /// Empty payloads are rejected (use a 1-byte code instead): every queued
    /// packet must be distinguishable from a pure ack.
    bool send_reliable(std::vector<u8> payload);

    /// Packets to transmit now: fresh payloads plus resends past the timeout.
    /// Advances the resend clock per packet emitted.
    std::vector<NetPacket> poll_outgoing(u64 now_ms);

    /// Feeds one received packet. Returns newly deliverable payloads in
    /// order (possibly empty). Never throws, never delivers twice.
    std::vector<std::vector<u8>> receive(const NetPacket& packet);

    u16 next_seq() const { return m_next_seq; }
    usize unacked_count() const { return m_unacked.size(); }
    usize buffered_count() const { return m_receive_buffer.size(); }
    u64 resends() const { return m_resends; }

private:
    struct Unacked {
        std::vector<u8> payload;
        u64 last_sent_ms = 0;
        bool sent_once = false;
    };

    static bool seq_less(u16 a, u16 b); // a < b in wrap-aware order
    void note_received(u16 seq);

    u16 m_next_seq = 0;
    u64 m_resend_timeout_ms = 100;
    std::map<u16, Unacked> m_unacked; // by sequence (ordered => resends in order)

    u16 m_expected = 0; // next in-order sequence to deliver
    std::map<u16, std::vector<u8>> m_receive_buffer;
    // Recently received sequences (for ack_bits): last 33 below expected.
    std::vector<u16> m_recent;
    u64 m_resends = 0;
};

} // namespace nf::net
