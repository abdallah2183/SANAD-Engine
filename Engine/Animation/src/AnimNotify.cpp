#include <NF/Animation/AnimNotify.hpp>
#include <algorithm>

namespace nf::animation {

void AnimTimeline::add_notify(AnimNotify notify) {
    m_notifies.push_back(std::move(notify));
    // Keep notifies sorted by trigger_time
    std::sort(m_notifies.begin(), m_notifies.end(), [](const AnimNotify& a, const AnimNotify& b) {
        return a.trigger_time < b.trigger_time;
    });
}

void AnimTimeline::add_instant_notify(std::string name, f32 trigger_time, std::string payload, int32_t user_data) {
    AnimNotify n;
    n.name = std::move(name);
    n.trigger_time = std::max(0.0f, trigger_time);
    n.duration = 0.0f;
    n.payload = std::move(payload);
    n.user_data = user_data;
    add_notify(std::move(n));
}

void AnimTimeline::add_state_notify(std::string name, f32 start_time, f32 duration, std::string payload, int32_t user_data) {
    AnimNotify n;
    n.name = std::move(name);
    n.trigger_time = std::max(0.0f, start_time);
    n.duration = std::max(0.0f, duration);
    n.payload = std::move(payload);
    n.user_data = user_data;
    add_notify(std::move(n));
}

void AnimTimeline::register_listener(std::string notify_name, NotifyCallback cb) {
    if (cb) {
        m_listeners.emplace_back(std::move(notify_name), std::move(cb));
    }
}

void AnimTimeline::dispatch_event(const TriggeredNotify& event) {
    for (const auto& [name_filter, cb] : m_listeners) {
        if (name_filter.empty() || name_filter == event.name) {
            cb(event);
        }
    }
}

void AnimTimeline::check_interval(f32 start_t, f32 end_t, std::vector<TriggeredNotify>& out_events) {
    for (const auto& n : m_notifies) {
        if (n.type() == NotifyType::Instant) {
            // Instant event: fire if within [start_t, end_t)
            if (n.trigger_time >= start_t && n.trigger_time <= end_t) {
                TriggeredNotify evt;
                evt.name = n.name;
                evt.time = n.trigger_time;
                evt.payload = n.payload;
                evt.user_data = n.user_data;
                evt.is_state_begin = false;
                evt.is_state_end = false;
                dispatch_event(evt);
                out_events.push_back(std::move(evt));
            }
        } else {
            // State notify: check start boundary
            const f32 state_end = n.trigger_time + n.duration;
            if (n.trigger_time >= start_t && n.trigger_time <= end_t) {
                TriggeredNotify evt;
                evt.name = n.name;
                evt.time = n.trigger_time;
                evt.payload = n.payload;
                evt.user_data = n.user_data;
                evt.is_state_begin = true;
                evt.is_state_end = false;
                dispatch_event(evt);
                out_events.push_back(std::move(evt));
            }
            // Check end boundary
            if (state_end >= start_t && state_end <= end_t) {
                TriggeredNotify evt;
                evt.name = n.name;
                evt.time = state_end;
                evt.payload = n.payload;
                evt.user_data = n.user_data;
                evt.is_state_begin = false;
                evt.is_state_end = true;
                dispatch_event(evt);
                out_events.push_back(std::move(evt));
            }
        }
    }
}

std::vector<TriggeredNotify> AnimTimeline::advance(f32 prev_time, f32 curr_time, f32 duration, bool is_looping) {
    std::vector<TriggeredNotify> events;

    if (duration <= 0.0f) {
        return events;
    }

    if (!is_looping || curr_time >= prev_time) {
        // Normal linear progression
        check_interval(prev_time, curr_time, events);
    } else {
        // Loop wrap occurred: interval [prev_time, duration] followed by [0.0, curr_time]
        check_interval(prev_time, duration, events);
        check_interval(0.0f, curr_time, events);
    }

    return events;
}

void AnimTimeline::clear() {
    m_notifies.clear();
    m_listeners.clear();
}

} // namespace nf::animation
