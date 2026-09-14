#pragma once

// NF/Core/Math.hpp — SIMD-aware math primitives for NOVAForge Engine
// Provides Vec2, Vec3, Vec4, Mat4, Quat with cache-friendly operations.

#include <NF/Core/Types.hpp>

#include <cmath>
#include <concepts>
#include <cstdint>
#include <utility>

// --- Platform SIMD detection ---
#if defined(__x86_64__) || defined(_M_X64)
    #include <xmmintrin.h>
    #include <emmintrin.h>
    #include <immintrin.h>
    #define NF_HAS_SSE 1
#else
    #define NF_HAS_SSE 0
#endif

namespace nf {

// --- Constants ---
inline constexpr f32 PI = 3.14159265358979323846f;
inline constexpr f32 TWO_PI = 2.0f * PI;
inline constexpr f32 HALF_PI = 0.5f * PI;
inline constexpr f32 EPSILON = 1e-5f;
inline constexpr f32 DEG_TO_RAD = PI / 180.0f;
inline constexpr f32 RAD_TO_DEG = 180.0f / PI;

// --- Utility functions ---
template<std::floating_point T>
constexpr T abs(T x) { return x < T(0) ? -x : x; }

template<std::floating_point T>
constexpr T min(T a, T b) { return a < b ? a : b; }

template<std::floating_point T>
constexpr T max(T a, T b) { return a > b ? a : b; }

template<std::floating_point T>
constexpr T clamp(T x, T lo, T hi) { return x < lo ? lo : (x > hi ? hi : x); }

template<std::floating_point T>
constexpr T lerp(T a, T b, T t) { return a + (b - a) * t; }

template<std::floating_point T>
constexpr T saturate(T x) { return clamp(x, T(0), T(1)); }

constexpr f32 to_radians(f32 deg) { return deg * DEG_TO_RAD; }
constexpr f32 to_degrees(f32 rad) { return rad * RAD_TO_DEG; }

// =========================================================================
// Vec2
// =========================================================================
struct Vec2 {
    f32 x, y;

    constexpr Vec2() : x(0), y(0) {}
    constexpr Vec2(f32 x, f32 y) : x(x), y(y) {}
    constexpr explicit Vec2(f32 s) : x(s), y(s) {}

    constexpr Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(f32 s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(f32 s) const { return {x / s, y / s}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }

    constexpr Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    constexpr Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    constexpr Vec2& operator*=(f32 s) { x *= s; y *= s; return *this; }

    constexpr f32 dot(const Vec2& o) const { return x * o.x + y * o.y; }
    constexpr f32 length_sq() const { return dot(*this); }
    f32 length() const { return std::sqrt(length_sq()); }
    Vec2 normalized() const {
        f32 len = length();
        return len > EPSILON ? *this / len : *this;
    }
};

// =========================================================================
// Vec3
// =========================================================================
struct Vec3 {
    f32 x, y, z;

    constexpr Vec3() : x(0), y(0), z(0) {}
    constexpr Vec3(f32 x, f32 y, f32 z) : x(x), y(y), z(z) {}
    constexpr explicit Vec3(f32 s) : x(s), y(s), z(s) {}

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(f32 s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(f32 s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }

    constexpr Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    constexpr Vec3& operator*=(f32 s) { x *= s; y *= s; z *= s; return *this; }

    constexpr f32 dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    constexpr Vec3 cross(const Vec3& o) const {
        return {
            y * o.z - z * o.y,
            z * o.x - x * o.z,
            x * o.y - y * o.x
        };
    }
    constexpr f32 length_sq() const { return dot(*this); }
    f32 length() const { return std::sqrt(length_sq()); }
    Vec3 normalized() const {
        f32 len = length();
        return len > EPSILON ? *this / len : *this;
    }

    static const Vec3 up;
    static const Vec3 forward;
    static const Vec3 right;
    static const Vec3 zero;
    static const Vec3 one;
};

inline constexpr const Vec3 Vec3::up{0, 1, 0};
inline constexpr const Vec3 Vec3::forward{0, 0, 1};
inline constexpr const Vec3 Vec3::right{1, 0, 0};
inline constexpr const Vec3 Vec3::zero{0, 0, 0};
inline constexpr const Vec3 Vec3::one{1, 1, 1};

// =========================================================================
// Vec4
// =========================================================================
struct Vec4 {
    f32 x, y, z, w;

    constexpr Vec4() : x(0), y(0), z(0), w(0) {}
    constexpr Vec4(f32 x, f32 y, f32 z, f32 w) : x(x), y(y), z(z), w(w) {}
    constexpr Vec4(const Vec3& v, f32 w) : x(v.x), y(v.y), z(v.z), w(w) {}

    constexpr Vec4 operator+(const Vec4& o) const { return {x+o.x, y+o.y, z+o.z, w+o.w}; }
    constexpr Vec4 operator-(const Vec4& o) const { return {x-o.x, y-o.y, z-o.z, w-o.w}; }
    constexpr Vec4 operator*(f32 s) const { return {x*s, y*s, z*s, w*s}; }

    constexpr f32 dot(const Vec4& o) const { return x*o.x + y*o.y + z*o.z + w*o.w; }
};

// =========================================================================
// Mat4 — Row-major 4x4 matrix
// =========================================================================
struct Mat4 {
    // Row-major: m[row][col]
    f32 m[4][4];

    constexpr Mat4() : m{} {}

    static constexpr Mat4 identity() {
        Mat4 r;
        r.m[0][0] = 1; r.m[1][1] = 1; r.m[2][2] = 1; r.m[3][3] = 1;
        return r;
    }

    static Mat4 translate(const Vec3& v);
    static Mat4 scale(const Vec3& v);
    static Mat4 rotate_x(f32 rad);
    static Mat4 rotate_y(f32 rad);
    static Mat4 rotate_z(f32 rad);
    static Mat4 rotation(const Vec3& axis, f32 angle);
    static Mat4 perspective(f32 fovy, f32 aspect, f32 near_z, f32 far_z);
    static Mat4 orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 near_z, f32 far_z);
    static Mat4 look_at(const Vec3& eye, const Vec3& center, const Vec3& up);

    Mat4 operator*(const Mat4& o) const;
    Vec4 operator*(const Vec4& v) const;
    Vec3 transform_point(const Vec3& v) const;
    Vec3 transform_direction(const Vec3& v) const;

    Mat4 transposed() const;
    Mat4 inverse() const;
};

// =========================================================================
// Quat — Quaternion for rotation
// =========================================================================
struct Quat {
    f32 x, y, z, w;

    constexpr Quat() : x(0), y(0), z(0), w(1) {}
    constexpr Quat(f32 x, f32 y, f32 z, f32 w) : x(x), y(y), z(z), w(w) {}

    static Quat from_axis_angle(const Vec3& axis, f32 angle);
    static Quat from_euler(f32 pitch, f32 yaw, f32 roll);

    Quat operator*(const Quat& o) const;
    Quat normalized() const;
    f32 dot(const Quat& o) const;
    Mat4 to_matrix() const;
};

} // namespace nf
