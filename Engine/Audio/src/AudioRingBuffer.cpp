#include <NF/Audio/AudioRingBuffer.hpp>

#include <cstring>

namespace nf::audio {

AudioRingBuffer::AudioRingBuffer(u32 capacity_frames) {
    // Round up to a power of two so the mask trick works.
    u32 cap = 1;
    while (cap < capacity_frames && cap < (1u << 30)) {
        cap <<= 1;
    }
    m_capacity = cap;
    m_mask = cap - 1;
    m_data.assign(static_cast<size_t>(cap) * 2, 0.0f);
}

u32 AudioRingBuffer::push(const f32* left, const f32* right, u32 frames) {
    if (left == nullptr || right == nullptr || frames == 0) {
        return 0;
    }
    const u32 w = m_write.load(std::memory_order_relaxed);
    const u32 r = m_read.load(std::memory_order_acquire);
    const u32 free = m_capacity - (w - r);
    const u32 n = frames < free ? frames : free;
    if (n < frames) {
        m_overruns.fetch_add(static_cast<u64>(frames - n), std::memory_order_relaxed);
    }
    for (u32 i = 0; i < n; ++i) {
        const u32 slot = (w + i) & m_mask;
        m_data[static_cast<size_t>(slot) * 2] = left[i];
        m_data[static_cast<size_t>(slot) * 2 + 1] = right[i];
    }
    m_write.store(w + n, std::memory_order_release);
    return n;
}

u32 AudioRingBuffer::pop(f32* out_interleaved, u32 frames) {
    if (out_interleaved == nullptr || frames == 0) {
        return 0;
    }
    const u32 r = m_read.load(std::memory_order_relaxed);
    const u32 w = m_write.load(std::memory_order_acquire);
    const u32 avail = w - r;
    const u32 n = frames < avail ? frames : avail;
    if (n < frames) {
        m_underruns.fetch_add(static_cast<u64>(frames - n), std::memory_order_relaxed);
    }
    // Contiguous run first; a wrapped run needs two copies (a single memcpy
    // past the vector end would over-read the heap, even when overwritten).
    const u32 base = r & m_mask;
    const u32 first = (base + n <= m_capacity) ? n : (m_capacity - base);
    std::memcpy(out_interleaved, &m_data[static_cast<size_t>(base) * 2],
                static_cast<size_t>(first) * 2 * sizeof(f32));
    if (first < n) {
        const u32 second = n - first;
        std::memcpy(out_interleaved + static_cast<size_t>(first) * 2, m_data.data(),
                    static_cast<size_t>(second) * 2 * sizeof(f32));
    }
    // Zero-fill the shortfall (silence, never stale ring data).
    for (u32 i = n; i < frames; ++i) {
        out_interleaved[static_cast<size_t>(i) * 2] = 0.0f;
        out_interleaved[static_cast<size_t>(i) * 2 + 1] = 0.0f;
    }
    m_read.store(r + n, std::memory_order_release);
    return n;
}

u32 AudioRingBuffer::stored_frames() const {
    const u32 w = m_write.load(std::memory_order_relaxed);
    const u32 r = m_read.load(std::memory_order_relaxed);
    return w - r;
}

u32 resample_linear(const f32* in_l, const f32* in_r, u32 in_frames, u32 in_rate, f32* out_l,
                    f32* out_r, u32 out_capacity, u32 out_rate, double& io_pos) {
    if (in_l == nullptr || in_r == nullptr || out_l == nullptr || out_r == nullptr ||
        in_frames == 0 || out_capacity == 0 || in_rate == 0 || out_rate == 0) {
        return 0;
    }
    const double step = static_cast<double>(in_rate) / static_cast<double>(out_rate);
    const f32 hold_l = in_l[in_frames - 1];
    const f32 hold_r = in_r[in_frames - 1];
    u32 written = 0;
    double pos = io_pos;
    while (written < out_capacity && pos < static_cast<double>(in_frames)) {
        const u32 i = static_cast<u32>(pos);
        const double frac = pos - static_cast<double>(i);
        const f32 s0l = in_l[i];
        const f32 s0r = in_r[i];
        const f32 s1l = (i + 1 < in_frames) ? in_l[i + 1] : hold_l;
        const f32 s1r = (i + 1 < in_frames) ? in_r[i + 1] : hold_r;
        const float f = static_cast<float>(frac);
        out_l[written] = s0l + (s1l - s0l) * f;
        out_r[written] = s0r + (s1r - s0r) * f;
        ++written;
        pos += step;
    }
    io_pos = pos - static_cast<double>(in_frames);
    if (io_pos < 0.0) {
        // Capacity capped the block early (caller undersized): restart at the
        // block start rather than carry a negative position (casting that to
        // u32 next call would be UB). Repeats a few samples; size the output
        // generously and this never happens.
        io_pos = 0.0;
    }
    return written;
}

} // namespace nf::audio
