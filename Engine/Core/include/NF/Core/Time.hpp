#pragma once

// NF/Core/Time.hpp — Time measurement utilities

#include <NF/Core/Types.hpp>

#include <chrono>

namespace nf {

class Clock {
public:
    using time_point = std::chrono::high_resolution_clock::time_point;
    using duration = std::chrono::high_resolution_clock::duration;

    Clock() : m_start(now()) {}

    time_point now() const {
        return std::chrono::high_resolution_clock::now();
    }

    f64 elapsed_seconds() const {
        auto e = now() - m_start;
        return std::chrono::duration<f64>(e).count();
    }

    f64 elapsed_ms() const {
        return elapsed_seconds() * 1000.0;
    }

    f64 elapsed_us() const {
        return elapsed_seconds() * 1000000.0;
    }

    void reset() {
        m_start = now();
    }

    static u64 tick_count() {
        return std::chrono::high_resolution_clock::now().time_since_epoch().count();
    }

    static f64 seconds_since_epoch() {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<f64>(now.time_since_epoch()).count();
    }

private:
    time_point m_start;
};

// Timer for game ticks
class Timer {
public:
    void start() { m_clock.reset(); }

    f32 tick() {
        f32 dt = static_cast<f32>(m_clock.elapsed_seconds());
        m_clock.reset();
        m_elapsed += dt;
        return dt;
    }

    f32 delta() const { return m_delta; }
    f32 elapsed() const { return m_elapsed; }

private:
    Clock m_clock;
    f32 m_delta = 0.0f;
    f32 m_elapsed = 0.0f;
};

// Scoped timer for profiling
class ScopedTimer {
public:
    explicit ScopedTimer(const char* label)
        : m_label(label), m_start(m_clock.now()) {}

    ~ScopedTimer() {
        auto end = m_clock.now();
        auto ms = std::chrono::duration<f64, std::milli>(end - m_start).count();
        // Use logger or store result
        (void)ms;
        (void)m_label;
    }

private:
    const char* m_label;
    Clock m_clock;
    Clock::time_point m_start;
};

// Thread-safe global time
class Time {
public:
    static f64 now();
    static u64 now_ticks();
    static void sleep_ms(u32 ms);
};

} // namespace nf
