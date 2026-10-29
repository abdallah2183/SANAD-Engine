#include <NF/Core/EventBus.hpp>

namespace nf {

EventBus& EventBus::instance() {
    static EventBus s_bus;
    return s_bus;
}

void EventBus::dispatch_queued() {
    std::vector<std::function<void()>> to_dispatch;
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        to_dispatch.swap(m_queued);
    }
    for (const auto& task : to_dispatch) {
        task();
    }
}

void EventBus::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::lock_guard<std::mutex> qlock(m_queue_mutex);
    m_listeners.clear();
    m_queued.clear();
}

usize EventBus::listener_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    usize total = 0;
    for (const auto& pair : m_listeners) {
        total += pair.second.size();
    }
    return total;
}

usize EventBus::queued_count() const {
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    return m_queued.size();
}

} // namespace nf
