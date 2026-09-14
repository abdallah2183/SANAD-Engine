#pragma once

// NF/Core/Types.hpp — Fundamental type definitions for NOVAForge Engine

#include <cstdint>
#include <cstddef>
#include <cstdint>

namespace nf {

// --- Signed integers ---
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

// --- Unsigned integers ---
using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// --- Floating point ---
using f32 = float;
using f64 = double;

// --- Character types ---
using char_t  = char;
using wchar_t_t = wchar_t;

// --- Size / ptrdiff ---
using usize = size_t;
using isize = ptrdiff_t;

// --- Byte type ---
enum class byte : u8 {};

// --- Boolean ---
// Use builtin bool

// --- Handle base ---
// Handle types are opaque — they wrap an index + generation for validity checking.
template<typename Tag>
struct Handle {
    static constexpr u32 Invalid = ~0u;

    u32 index = Invalid;
    u32 generation = 0;

    constexpr bool is_valid() const { return index != Invalid; }
    constexpr bool operator==(const Handle& other) const = default;
    constexpr bool operator!=(const Handle& other) const = default;
};

// --- Non-owning span view ---
template<typename T>
class Span {
public:
    constexpr Span() = default;
    constexpr Span(T* data, usize size) : m_data(data), m_size(size) {}

    template<usize N>
    constexpr Span(T (&arr)[N]) : m_data(arr), m_size(N) {}

    constexpr T*       data()       { return m_data; }
    constexpr const T* data() const { return m_data; }
    constexpr usize    size() const { return m_size; }
    constexpr bool     empty() const { return m_size == 0; }

    constexpr T&       operator[](usize i)       { return m_data[i]; }
    constexpr const T& operator[](usize i) const { return m_data[i]; }

    constexpr T*       begin()       { return m_data; }
    constexpr T*       end()         { return m_data + m_size; }
    constexpr const T* begin() const { return m_data; }
    constexpr const T* end() const   { return m_data + m_size; }

private:
    T*    m_data = nullptr;
    usize m_size = 0;
};

// --- Compile-time constants ---
inline constexpr u32 KB = 1024;
inline constexpr u32 MB = KB * 1024;
inline constexpr u32 GB = MB * 1024;

inline constexpr u32 KiB = 1024;
inline constexpr u32 MiB = KiB * 1024;
inline constexpr u32 GiB = MiB * 1024;

// --- Index constants ---
inline constexpr u32 u32_max = ~0u;
inline constexpr u64 u64_max = ~0ull;
inline constexpr usize usize_max = ~static_cast<usize>(0);

// --- Non-copyable / Non-movable ---
class NonCopyable {
public:
    NonCopyable() = default;
    ~NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

class NonMovable {
public:
    NonMovable() = default;
    ~NonMovable() = default;
    NonMovable(NonMovable&&) = delete;
    NonMovable& operator=(NonMovable&&) = delete;
};

} // namespace nf
