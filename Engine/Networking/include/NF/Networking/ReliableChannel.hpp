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
// The ack history names the FUTURE sequences that already arrived out of
// order: bit n is set when the packet (ack + 1 + n) was received before its
// predecessor. That is the information the sender actually needs — a packet
// sitting in the receiver's out-of-order buffer is delivered, and the sender
// may free it immediately instead of resending it until the gap in front of
// it finally fills.
//
// Sequence arithmetic is u16 with 0x8000-window comparison, so streams wrap
// safely every 65536 packets. The INTERNAL sequence counter is 32-bit: the
// 16-bit wire sequence repeats every 65536 packets, and a queue that still
// held a packet from the previous lap would collide on the key.

#include <NF/Core/Types.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nf::net {

struct NetPacket {
    u16 seq = 0; // this packet's sequence
    u16 ack = 0; // highest in-order sequence received by the sender
    u32 ack_bits = 0; // bit n set ⟺ packet (ack + 1 + n) was received out of order
    std::vector<u8> payload; // empty payloads are pure acks (never queued)
};

class ReliableChannel {
public:
    explicit ReliableChannel(u16 initial_seq = 0, u64 resend_timeout_ms = 100);
    /// Queues a payload for reliable delivery (assigned the next sequence).
    /// Empty payloads are rejected (use a 1-byte code instead): every queued
    /// packet must be distinguishable from a pure ack.
    /// Returns false when the payload was refused: the queue is full (the
    /// peer is not acking) or the sequence collided with a packet that is
    /// still in flight. A refused payload is NEVER silently dropped.
    bool send_reliable(std::vector<u8> payload);

    /// Packets to transmit now: fresh payloads plus resends past the timeout.
    /// Advances the resend clock per packet emitted.
    std::vector<NetPacket> poll_outgoing(u64 now_ms);

    /// Feeds one received packet. Returns newly deliverable payloads in
    /// order (possibly empty). Never throws, never delivers twice.
    std::vector<std::vector<u8>> receive(const NetPacket& packet);

    /// The next sequence that will be put on the wire (16-bit: it repeats
    /// every 65536 packets, exactly like the packets themselves).
    u16 next_seq() const { return static_cast<u16>(m_next_seq & 0xFFFFu); }
    usize unacked_count() const { return m_unacked.size(); }
    usize buffered_count() const { return m_receive_buffer.size(); }
    u64 resends() const { return m_resends; }

private:
    /// How many payloads may be in flight at once. A peer that never acks
    /// must not be able to grow the sender's queue without bound; 4096
    /// payloads is far more than any real stream holds between acks.
    static constexpr usize kMaxUnacked = 4096;

    struct Unacked {
        std::vector<u8> payload;
        u64 last_sent_ms = 0;
        bool sent_once = false;
    };

    static bool seq_less(u16 a, u16 b); // a < b in wrap-aware order

    u32 m_next_seq = 0; // internal sequence counter (never wraps in practice)
    u64 m_resend_timeout_ms = 100;
    std::map<u32, Unacked> m_unacked; // by internal sequence (ordered => resends in order)

    u16 m_expected = 0; // next in-order sequence to deliver
    std::map<u16, std::vector<u8>> m_receive_buffer; // out-of-order arrivals
    bool m_received_any = false; // a payload-bearing packet was received
    u64 m_resends = 0;
};

/// Datagram framing for channel packets: magic(4) version(1) seq(2) ack(2)
/// bits(4) + payload. decode validates and fails loudly on corruption.
std::vector<u8> encode_packet(const NetPacket& packet);
bool decode_packet(const u8* data, usize size, NetPacket& out, std::string& out_error);

} // namespace nf::net
