#pragma once

#include <NF/Core/Types.hpp>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace nf {

namespace bits {

inline int popcount64(uint64_t v) {
#if defined(_MSC_VER)
    return static_cast<int>(__popcnt64(v));
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(v);
#else
    v = v - ((v >> 1) & 0x5555555555555555ULL);
    v = (v & 0x3333333333333333ULL) + ((v >> 2) & 0x3333333333333333ULL);
    return static_cast<int>((((v + (v >> 4)) & 0x0F0F0F0F0F0F0F0FULL) * 0x0101010101010101ULL) >> 56);
#endif
}

inline int count_trailing_zeros64(uint64_t v) {
    if (v == 0) return 64;
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanForward64(&index, v);
    return static_cast<int>(index);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(v);
#else
    int c = 0;
    if ((v & 0xFFFFFFFFULL) == 0) { c += 32; v >>= 32; }
    if ((v & 0x0000FFFFULL) == 0) { c += 16; v >>= 16; }
    if ((v & 0x000000FFULL) == 0) { c += 8; v >>= 8; }
    if ((v & 0x0000000FULL) == 0) { c += 4; v >>= 4; }
    if ((v & 0x00000003ULL) == 0) { c += 2; v >>= 2; }
    if ((v & 0x00000001ULL) == 0) { c += 1; }
    return c;
#endif
}

inline int count_leading_zeros64(uint64_t v) {
    if (v == 0) return 64;
#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanReverse64(&index, v);
    return static_cast<int>(63 - index);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(v);
#else
    int c = 0;
    if ((v & 0xFFFFFFFF00000000ULL) == 0) { c += 32; v <<= 32; }
    if ((v & 0xFFFF000000000000ULL) == 0) { c += 16; v <<= 16; }
    if ((v & 0xFF00000000000000ULL) == 0) { c += 8; v <<= 8; }
    if ((v & 0xF000000000000000ULL) == 0) { c += 4; v <<= 4; }
    if ((v & 0xC000000000000000ULL) == 0) { c += 2; v <<= 2; }
    if ((v & 0x8000000000000000ULL) == 0) { c += 1; }
    return c;
#endif
}

} // namespace bits

/// Fixed-size compile-time bitset with hardware-accelerated popcount and bit scanning.
template <size_t N>
class BitSet {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);
    static constexpr size_t NumWords = (N == 0) ? 1 : (N + 63) / 64;
    static constexpr uint64_t LastWordMask = (N == 0 || N % 64 == 0) ? ~0ULL : ((1ULL << (N % 64)) - 1ULL);

    constexpr BitSet() : m_words{} {}

    constexpr size_t size() const noexcept { return N; }

    BitSet& set(size_t pos, bool val = true) {
        if (pos < N) {
            if (val) {
                m_words[pos / 64] |= (1ULL << (pos % 64));
            } else {
                m_words[pos / 64] &= ~(1ULL << (pos % 64));
            }
        }
        return *this;
    }

    BitSet& set() noexcept {
        for (size_t i = 0; i < NumWords; ++i) {
            m_words[i] = ~0ULL;
        }
        mask_unused();
        return *this;
    }

    BitSet& reset(size_t pos) {
        return set(pos, false);
    }

    BitSet& reset() noexcept {
        for (size_t i = 0; i < NumWords; ++i) {
            m_words[i] = 0ULL;
        }
        return *this;
    }

    BitSet& flip(size_t pos) {
        if (pos < N) {
            m_words[pos / 64] ^= (1ULL << (pos % 64));
        }
        return *this;
    }

    BitSet& flip() noexcept {
        for (size_t i = 0; i < NumWords; ++i) {
            m_words[i] = ~m_words[i];
        }
        mask_unused();
        return *this;
    }

    bool test(size_t pos) const {
        if (pos >= N) return false;
        return (m_words[pos / 64] & (1ULL << (pos % 64))) != 0;
    }

    bool operator[](size_t pos) const {
        return test(pos);
    }

    size_t count() const noexcept {
        size_t c = 0;
        for (size_t i = 0; i < NumWords; ++i) {
            c += static_cast<size_t>(bits::popcount64(m_words[i]));
        }
        return c;
    }

    bool any() const noexcept {
        for (size_t i = 0; i < NumWords; ++i) {
            if (m_words[i] != 0) return true;
        }
        return false;
    }

    bool none() const noexcept {
        return !any();
    }

    bool all() const noexcept {
        return count() == N;
    }

    size_t find_first() const noexcept {
        for (size_t w = 0; w < NumWords; ++w) {
            if (m_words[w] != 0) {
                size_t bit = w * 64 + static_cast<size_t>(bits::count_trailing_zeros64(m_words[w]));
                return (bit < N) ? bit : npos;
            }
        }
        return npos;
    }

    size_t find_next(size_t from) const noexcept {
        if (from + 1 >= N) return npos;
        size_t next_idx = from + 1;
        size_t w = next_idx / 64;
        size_t b = next_idx % 64;

        uint64_t masked = m_words[w] & (~0ULL << b);
        if (masked != 0) {
            size_t bit = w * 64 + static_cast<size_t>(bits::count_trailing_zeros64(masked));
            return (bit < N) ? bit : npos;
        }

        for (size_t i = w + 1; i < NumWords; ++i) {
            if (m_words[i] != 0) {
                size_t bit = i * 64 + static_cast<size_t>(bits::count_trailing_zeros64(m_words[i]));
                return (bit < N) ? bit : npos;
            }
        }
        return npos;
    }

    BitSet operator~() const noexcept {
        BitSet res = *this;
        res.flip();
        return res;
    }

    BitSet& operator&=(const BitSet& other) noexcept {
        for (size_t i = 0; i < NumWords; ++i) {
            m_words[i] &= other.m_words[i];
        }
        return *this;
    }

    BitSet& operator|=(const BitSet& other) noexcept {
        for (size_t i = 0; i < NumWords; ++i) {
            m_words[i] |= other.m_words[i];
        }
        return *this;
    }

    BitSet& operator^=(const BitSet& other) noexcept {
        for (size_t i = 0; i < NumWords; ++i) {
            m_words[i] ^= other.m_words[i];
        }
        mask_unused();
        return *this;
    }

    friend BitSet operator&(BitSet lhs, const BitSet& rhs) noexcept {
        lhs &= rhs;
        return lhs;
    }

    friend BitSet operator|(BitSet lhs, const BitSet& rhs) noexcept {
        lhs |= rhs;
        return lhs;
    }

    friend BitSet operator^(BitSet lhs, const BitSet& rhs) noexcept {
        lhs ^= rhs;
        return lhs;
    }

    bool operator==(const BitSet& other) const noexcept {
        for (size_t i = 0; i < NumWords; ++i) {
            if (m_words[i] != other.m_words[i]) return false;
        }
        return true;
    }

    bool operator!=(const BitSet& other) const noexcept {
        return !(*this == other);
    }

    std::string to_string() const {
        std::string res;
        res.resize(N);
        for (size_t i = 0; i < N; ++i) {
            res[N - 1 - i] = test(i) ? '1' : '0';
        }
        return res;
    }

private:
    void mask_unused() noexcept {
        if constexpr (N > 0 && (N % 64 != 0)) {
            m_words[NumWords - 1] &= LastWordMask;
        }
    }

    uint64_t m_words[NumWords];
};

/// Dynamically sized bitset with contiguous word backing and hardware acceleration.
class DynamicBitSet {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);

    DynamicBitSet() : m_size(0) {}
    explicit DynamicBitSet(size_t num_bits, bool init_val = false)
        : m_size(num_bits) {
        size_t words = (m_size + 63) / 64;
        m_words.resize(words, init_val ? ~0ULL : 0ULL);
        mask_unused();
    }

    size_t size() const noexcept { return m_size; }
    bool empty() const noexcept { return m_size == 0; }

    void resize(size_t num_bits, bool init_val = false) {
        size_t old_size = m_size;
        m_size = num_bits;
        size_t words = (m_size + 63) / 64;
        m_words.resize(words, 0ULL);
        if (num_bits > old_size && init_val) {
            for (size_t i = old_size; i < num_bits; ++i) {
                set(i, true);
            }
        } else {
            mask_unused();
        }
    }

    void clear() noexcept {
        m_size = 0;
        m_words.clear();
    }

    DynamicBitSet& set(size_t pos, bool val = true) {
        if (pos < m_size) {
            if (val) {
                m_words[pos / 64] |= (1ULL << (pos % 64));
            } else {
                m_words[pos / 64] &= ~(1ULL << (pos % 64));
            }
        }
        return *this;
    }

    DynamicBitSet& set() noexcept {
        for (auto& w : m_words) {
            w = ~0ULL;
        }
        mask_unused();
        return *this;
    }

    DynamicBitSet& reset(size_t pos) {
        return set(pos, false);
    }

    DynamicBitSet& reset() noexcept {
        for (auto& w : m_words) {
            w = 0ULL;
        }
        return *this;
    }

    DynamicBitSet& flip(size_t pos) {
        if (pos < m_size) {
            m_words[pos / 64] ^= (1ULL << (pos % 64));
        }
        return *this;
    }

    DynamicBitSet& flip() noexcept {
        for (auto& w : m_words) {
            w = ~w;
        }
        mask_unused();
        return *this;
    }

    bool test(size_t pos) const {
        if (pos >= m_size) return false;
        return (m_words[pos / 64] & (1ULL << (pos % 64))) != 0;
    }

    bool operator[](size_t pos) const {
        return test(pos);
    }

    size_t count() const noexcept {
        size_t c = 0;
        for (uint64_t w : m_words) {
            c += static_cast<size_t>(bits::popcount64(w));
        }
        return c;
    }

    bool any() const noexcept {
        for (uint64_t w : m_words) {
            if (w != 0) return true;
        }
        return false;
    }

    bool none() const noexcept {
        return !any();
    }

    bool all() const noexcept {
        return m_size > 0 && count() == m_size;
    }

    size_t find_first() const noexcept {
        for (size_t w = 0; w < m_words.size(); ++w) {
            if (m_words[w] != 0) {
                size_t bit = w * 64 + static_cast<size_t>(bits::count_trailing_zeros64(m_words[w]));
                return (bit < m_size) ? bit : npos;
            }
        }
        return npos;
    }

    size_t find_next(size_t from) const noexcept {
        if (from + 1 >= m_size) return npos;
        size_t next_idx = from + 1;
        size_t w = next_idx / 64;
        size_t b = next_idx % 64;

        uint64_t masked = m_words[w] & (~0ULL << b);
        if (masked != 0) {
            size_t bit = w * 64 + static_cast<size_t>(bits::count_trailing_zeros64(masked));
            return (bit < m_size) ? bit : npos;
        }

        for (size_t i = w + 1; i < m_words.size(); ++i) {
            if (m_words[i] != 0) {
                size_t bit = i * 64 + static_cast<size_t>(bits::count_trailing_zeros64(m_words[i]));
                return (bit < m_size) ? bit : npos;
            }
        }
        return npos;
    }

    DynamicBitSet& operator&=(const DynamicBitSet& other) {
        size_t common = std::min(m_words.size(), other.m_words.size());
        for (size_t i = 0; i < common; ++i) {
            m_words[i] &= other.m_words[i];
        }
        for (size_t i = common; i < m_words.size(); ++i) {
            m_words[i] = 0;
        }
        return *this;
    }

    DynamicBitSet& operator|=(const DynamicBitSet& other) {
        if (other.m_size > m_size) {
            resize(other.m_size);
        }
        for (size_t i = 0; i < other.m_words.size(); ++i) {
            m_words[i] |= other.m_words[i];
        }
        mask_unused();
        return *this;
    }

    DynamicBitSet& operator^=(const DynamicBitSet& other) {
        if (other.m_size > m_size) {
            resize(other.m_size);
        }
        for (size_t i = 0; i < other.m_words.size(); ++i) {
            m_words[i] ^= other.m_words[i];
        }
        mask_unused();
        return *this;
    }

    bool operator==(const DynamicBitSet& other) const noexcept {
        if (m_size != other.m_size) return false;
        return m_words == other.m_words;
    }

    bool operator!=(const DynamicBitSet& other) const noexcept {
        return !(*this == other);
    }

    std::string to_string() const {
        std::string res;
        res.resize(m_size);
        for (size_t i = 0; i < m_size; ++i) {
            res[m_size - 1 - i] = test(i) ? '1' : '0';
        }
        return res;
    }

private:
    void mask_unused() noexcept {
        if (m_size == 0) return;
        size_t rem = m_size % 64;
        if (rem != 0) {
            uint64_t mask = (1ULL << rem) - 1ULL;
            m_words.back() &= mask;
        }
    }

    size_t m_size = 0;
    std::vector<uint64_t> m_words;
};

} // namespace nf
