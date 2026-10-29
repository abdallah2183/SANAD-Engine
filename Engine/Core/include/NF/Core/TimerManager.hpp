#pragma once

#include <NF/Core/Types.hpp>
#include <functional>
#include <vector>
#include <mutex>
#include <cstdint>

namespace nf {

/// Generational, lightweight handle identifying a registered timer.
struct TimerHandle {
    uint32_t id = 0;
    uint32_t generation = 0;

    constexpr TimerHandle() = default;
    constexpr TimerHandle(uint32_t in_id, uint32_t in_gen) : id(in_id), generation(in_gen) {}

    constexpr bool valid() const noexcept { return id != 0 && generation != 0; }
    constexpr explicit operator bool() const noexcept { return valid(); }

    constexpr bool operator==(const TimerHandle& other) const noexcept {
        return id == other.id && generation == other.generation;
    }
    constexpr bool operator!=(const TimerHandle& other) const noexcept {
        return !(*this == other);
    }
    constexpr bool operator<(const TimerHandle& other) const noexcept {
        if (id != other.id) return id < other.id;
        return generation < other.generation;
    }
};

/// High-performance, re-entrancy safe delayed and recurring timer scheduler.
class TimerManager {
public:
    static TimerManager& instance();

    TimerManager();
    ~TimerManager() = default;

    TimerManager(const TimerManager&) = delete;
    TimerManager& operator=(const TimerManager&) = delete;

    /// Schedules a one-shot callback after delay_seconds.
    TimerHandle set_timeout(float delay_seconds, std::function<void()> callback);

    /// Schedules a recurring callback firing every interval_seconds.
    TimerHandle set_interval(float interval_seconds, std::function<void()> callback);

    /// Cancels an active or paused timer. Returns true if cancelled.
    bool clear_timer(TimerHandle handle);

    /// Pauses an active timer.
    bool pause_timer(TimerHandle handle);

    /// Resumes a paused timer.
    bool resume_timer(TimerHandle handle);

    /// Checks if the timer handle is active and unexpired.
    bool is_timer_active(TimerHandle handle) const;

    /// Checks if the timer handle is paused.
    bool is_timer_paused(TimerHandle handle) const;

    /// Returns time remaining in seconds until the next firing, or -1 if invalid.
    float get_time_remaining(TimerHandle handle) const;

    /// Returns total elapsed time in seconds since the timer was created.
    float get_elapsed_time(TimerHandle handle) const;

    /// Time dilation factor (e.g. 1.0 = normal, 0.5 = half speed, 0.0 = paused).
    void set_time_dilation(float dilation) noexcept;
    float get_time_dilation() const noexcept;

    /// Advances simulation time and fires expired timers. Re-entrancy safe.
    void update(float dt);

    /// Clears all timers.
    void clear_all();

    /// Returns count of currently active or paused timers.
    size_t active_count() const;

private:
    struct Entry {
        uint32_t id = 0;
        uint32_t generation = 1;
        float rate = 0.0f;
        float remaining = 0.0f;
        float elapsed = 0.0f;
        bool is_interval = false;
        bool is_paused = false;
        bool in_use = false;
        std::function<void()> callback;
    };

    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries;
    std::vector<uint32_t> m_free_indices;
    float m_time_dilation = 1.0f;
};

} // namespace nf

namespace std {
template <>
struct hash<nf::TimerHandle> {
    size_t operator()(const nf::TimerHandle& h) const noexcept {
        return (static_cast<size_t>(h.id) << 32) ^ static_cast<size_t>(h.generation);
    }
};
} // namespace std
