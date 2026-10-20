// NF/Networking/ReliableChannel.cpp — in-order reliable stream.

#include <NF/Networking/ReliableChannel.hpp>

namespace nf::net {

namespace {

void push_u16(std::vector<u8>& out, u16 v) {
    out.push_back(static_cast<u8>(v & 0xFF));
    out.push_back(static_cast<u8>((v >> 8) & 0xFF));
}

void push_u32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v & 0xFF));
    out.push_back(static_cast<u8>((v >> 8) & 0xFF));
    out.push_back(static_cast<u8>((v >> 16) & 0xFF));
    out.push_back(static_cast<u8>((v >> 24) & 0xFF));
}

u16 read_u16(const u8* p) {
    return static_cast<u16>(p[0] | (static_cast<u16>(p[1]) << 8));
}

u32 read_u32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

} // namespace

ReliableChannel::ReliableChannel(u16 initial_seq, u64 resend_timeout_ms)
    : m_next_seq(initial_seq),
      m_resend_timeout_ms(resend_timeout_ms > 0 ? resend_timeout_ms : 1),
      m_expected(initial_seq) {}

bool ReliableChannel::seq_less(u16 a, u16 b) {
    // Wrap-aware: a precedes b when the forward distance a->b is in (0, 32768).
    return a != b && static_cast<u16>(b - a) < 0x8000u;
}

bool ReliableChannel::send_reliable(std::vector<u8> payload) {
    if (payload.empty()) return false;
    // The queue is bounded: a peer that never acks must not be able to grow
    // the sender's memory without limit.
    if (m_unacked.size() >= kMaxUnacked) return false;
    const u32 seq = m_next_seq;
    const auto inserted = m_unacked.emplace(seq, Unacked{std::move(payload), 0, false});
    // The sequence is already in flight: refuse the payload instead of
    // dropping it silently. With a 32-bit counter this cannot happen, but
    // the check is the difference between "lost" and "reported".
    if (!inserted.second) return false;
    ++m_next_seq;
    return true;
}

std::vector<NetPacket> ReliableChannel::poll_outgoing(u64 now_ms) {
    // Current ack state: the cumulative ack is the last delivered sequence,
    // and the bitfield names the sequences already received OUT OF ORDER
    // (bit n ⟺ ack + 1 + n). Those are exactly the packets sitting in the
    // receive buffer: the sender can free them now instead of resending
    // them until the gap in front of them fills.
    const u16 ack = static_cast<u16>(m_expected - 1);
    u32 bits = 0;
    for (const auto& [seq, payload] : m_receive_buffer) {
        (void)payload;
        const u16 distance = static_cast<u16>(seq - ack);
        if (distance >= 1 && distance <= 32) bits |= (1u << (distance - 1));
    }

    std::vector<NetPacket> out;
    for (auto& [seq, entry] : m_unacked) {
        const bool due = !entry.sent_once || now_ms - entry.last_sent_ms >= m_resend_timeout_ms;
        if (!due) continue;
        if (entry.sent_once) ++m_resends;
        entry.sent_once = true;
        entry.last_sent_ms = now_ms;
        NetPacket pkt;
        pkt.seq = static_cast<u16>(seq & 0xFFFFu);
        pkt.ack = ack;
        pkt.ack_bits = bits;
        pkt.payload = entry.payload;
        out.push_back(std::move(pkt));
    }
    // Pure ack (no payload) when there is nothing else to say: ack state
    // still needs to travel, or the peer resends forever. Emitted only when
    // the queue is silent to avoid doubling every packet.
    if (out.empty() && (m_received_any || m_expected != 0 || !m_receive_buffer.empty())) {
        NetPacket pkt;
        pkt.seq = next_seq(); // NOT consumed: pure acks carry no sequence
        pkt.ack = ack;
        pkt.ack_bits = bits;
        out.push_back(std::move(pkt));
    }
    return out;
}

std::vector<std::vector<u8>> ReliableChannel::receive(const NetPacket& packet) {
    std::vector<std::vector<u8>> ready;
    // First consume the peer's ack state to free our queue.
    {
        // Cumulative ack frees everything <= ack in wrap-aware order...
        // careful: ack is "highest in-order received". Free seq when it is
        // at or behind ack: !(ack < seq), i.e. seq <= ack in stream order.
        // Since our unacked seqs are all ahead of (or at) the peer's view,
        // free when NOT seq_less(ack, seq).
        for (auto it = m_unacked.begin(); it != m_unacked.end();) {
            const u16 seq = static_cast<u16>(it->first & 0xFFFFu);
            bool acked = !seq_less(packet.ack, seq);
            if (!acked) {
                // Bitfield: bit n ⟺ (ack + 1 + n) was received out of order.
                // The distance is measured FORWARD from the cumulative ack,
                // and only the 32 sequences right above it can be named.
                const u16 distance = static_cast<u16>(seq - packet.ack);
                if (distance >= 1 && distance <= 32) {
                    acked = (packet.ack_bits & (1u << (distance - 1))) != 0;
                }
            }
            if (acked) {
                it = m_unacked.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Pure acks carry no payload and no sequence.
    if (packet.payload.empty()) return ready;

    m_received_any = true;
    if (seq_less(packet.seq, m_expected)) {
        return ready; // duplicate or ancient: acked above, never delivered
    }
    if (packet.seq == m_expected) {
        ready.push_back(packet.payload);
        ++m_expected;
        // Drain buffered successors in order.
        for (;;) {
            auto it = m_receive_buffer.find(m_expected);
            if (it == m_receive_buffer.end()) break;
            ready.push_back(std::move(it->second));
            m_receive_buffer.erase(it);
            ++m_expected;
        }
        return ready;
    }
    // Future packet: buffer (cap: 256 gaps, then drop the newest — a flooded
    // peer degrades to resends, never to unbounded memory).
    if (m_receive_buffer.size() < 256) {
        m_receive_buffer.emplace(packet.seq, packet.payload);
    }
    return ready;
}

std::vector<u8> encode_packet(const NetPacket& packet) {
    std::vector<u8> out;
    out.push_back('N');
    out.push_back('F');
    out.push_back('C');
    out.push_back('H');
    out.push_back(1); // version
    push_u16(out, packet.seq);
    push_u16(out, packet.ack);
    push_u32(out, packet.ack_bits);
    out.insert(out.end(), packet.payload.begin(), packet.payload.end());
    return out; // 13-byte header + payload
}

bool decode_packet(const u8* data, usize size, NetPacket& out, std::string& out_error) {
    out = NetPacket{};
    constexpr usize kHeader = 13;
    if (!data || size < kHeader) {
        out_error = "channel packet too short";
        return false;
    }
    if (data[0] != 'N' || data[1] != 'F' || data[2] != 'C' || data[3] != 'H') {
        out_error = "not a channel packet (bad magic)";
        return false;
    }
    if (data[4] != 1) {
        out_error = "unsupported channel packet version";
        return false;
    }
    out.seq = read_u16(data + 5);
    out.ack = read_u16(data + 7);
    out.ack_bits = read_u32(data + 9);
    out.payload.assign(data + kHeader, data + size);
    return true;
}

} // namespace nf::net
