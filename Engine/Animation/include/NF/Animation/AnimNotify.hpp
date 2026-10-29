#pragma once

#include <NF/Core/Types.hpp>
#include <string>
#include <vector>
#include <functional>

namespace nf::animation {

enum class NotifyType : u8 {
    Instant = 0,
    State = 1
};

struct AnimNotify {
    std::string name;
    f32 trigger_time = 0.0f; // Seconds within animation clip
    f32 duration = 0.0f;     // Seconds (0 = instant point event)
    std::string payload;     // Optional data (sound name, particle, socket)
    int32_t user_data = 0;   // Custom integer tag or ID

    [[nodiscard]] NotifyType type() const noexcept {
        return duration > 0.0f ? NotifyType::State : NotifyType::Instant;
    }
};

struct TriggeredNotify {
    std::string name;
    f32 time = 0.0f;
    std::string payload;
    int32_t user_data = 0;
    bool is_state_begin = false;
    bool is_state_end = false;
};

/// AnimTimeline: manages an ordered set of notifies and dispatches crossed events
class AnimTimeline {
public:
    using NotifyCallback = std::function<void(const TriggeredNotify&)>;

    AnimTimeline() = default;

    void add_notify(AnimNotify notify);
    void add_instant_notify(std::string name, f32 trigger_time, std::string payload = "", int32_t user_data = 0);
    void add_state_notify(std::string name, f32 start_time, f32 duration, std::string payload = "", int32_t user_data = 0);

    [[nodiscard]] size_t notify_count() const noexcept { return m_notifies.size(); }
    [[nodiscard]] const AnimNotify& notify(size_t index) const { return m_notifies[index]; }

    /// Register a callback listener for a notify name (empty name listens to all)
    void register_listener(std::string notify_name, NotifyCallback cb);

    /// Advance timeline and return all events triggered in the interval [prev_time, curr_time).
    /// Handles loop wrapping correctly.
    std::vector<TriggeredNotify> advance(f32 prev_time, f32 curr_time, f32 duration, bool is_looping);

    void clear();

private:
    void dispatch_event(const TriggeredNotify& event);
    void check_interval(f32 start_t, f32 end_t, std::vector<TriggeredNotify>& out_events);

    std::vector<AnimNotify> m_notifies;
    std::vector<std::pair<std::string, NotifyCallback>> m_listeners;
};

} // namespace nf::animation
