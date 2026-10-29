#pragma once

// NF/Core/EventBus.hpp — Decoupled High-Performance Pub/Sub Event System
// Provides type-safe event subscription and publishing without tight coupling between systems.

#include <NF/Core/Types.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace nf {

using SubscriptionId = u64;
inline constexpr SubscriptionId kInvalidSubscriptionId = 0;

// --- Common Core Events ---
struct TickEvent {
    f32 delta_time = 0.0f;
    u64 frame_number = 0;
};

struct WindowResizeEvent {
    u32 width = 0;
    u32 height = 0;
};

struct EntityCreatedEvent {
    u32 entity_id = 0;
};

struct EntityDestroyedEvent {
    u32 entity_id = 0;
};

struct SceneChangedEvent {
    const char* scene_path = nullptr;
    bool is_saved = false;
};

// =========================================================================
// EventBus — Central event dispatch hub
// =========================================================================
class EventBus {
public:
    static EventBus& instance();

    template<typename EventType>
    SubscriptionId subscribe(std::function<void(const EventType&)> handler) {
        std::lock_guard<std::mutex> lock(m_mutex);
        SubscriptionId id = ++m_next_id;
        auto wrapper = [h = std::move(handler)](const void* data) {
            h(*static_cast<const EventType*>(data));
        };
        m_listeners[std::type_index(typeid(EventType))].push_back({id, std::move(wrapper)});
        return id;
    }

    template<typename EventType>
    bool unsubscribe(SubscriptionId id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_listeners.find(std::type_index(typeid(EventType)));
        if (it == m_listeners.end()) return false;
        auto& list = it->second;
        for (auto list_it = list.begin(); list_it != list.end(); ++list_it) {
            if (list_it->id == id) {
                list.erase(list_it);
                return true;
            }
        }
        return false;
    }

    template<typename EventType>
    void publish(const EventType& event) {
        std::vector<std::function<void(const void*)>> to_call;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_listeners.find(std::type_index(typeid(EventType)));
            if (it == m_listeners.end() || it->second.empty()) return;
            to_call.reserve(it->second.size());
            for (const auto& entry : it->second) {
                to_call.push_back(entry.callback);
            }
        }
        // Invoke outside lock to avoid deadlocks in re-entrant handlers
        for (const auto& cb : to_call) {
            cb(&event);
        }
    }

    template<typename EventType>
    void queue(const EventType& event) {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_queued.push_back([this, evt = event]() {
            this->publish<EventType>(evt);
        });
    }

    void dispatch_queued();
    void clear();

    usize listener_count() const;
    usize queued_count() const;

private:
    EventBus() = default;

    struct ListenerEntry {
        SubscriptionId id;
        std::function<void(const void*)> callback;
    };

    mutable std::mutex m_mutex;
    std::unordered_map<std::type_index, std::vector<ListenerEntry>> m_listeners;
    SubscriptionId m_next_id = 0;

    mutable std::mutex m_queue_mutex;
    std::vector<std::function<void()>> m_queued;
};

// Auto-unsubscribing RAII helper
class ScopedSubscription {
public:
    ScopedSubscription() = default;

    template<typename EventType>
    ScopedSubscription(EventBus& bus, std::function<void(const EventType&)> handler)
        : m_bus(&bus) {
        m_id = bus.subscribe<EventType>(std::move(handler));
        m_unsub = [this]() {
            if (m_bus && m_id != kInvalidSubscriptionId) {
                m_bus->unsubscribe<EventType>(m_id);
            }
        };
    }

    ~ScopedSubscription() {
        if (m_unsub) {
            m_unsub();
        }
    }

    ScopedSubscription(const ScopedSubscription&) = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;

    ScopedSubscription(ScopedSubscription&& o) noexcept
        : m_bus(o.m_bus), m_id(o.m_id), m_unsub(std::move(o.m_unsub)) {
        o.m_bus = nullptr;
        o.m_id = kInvalidSubscriptionId;
        o.m_unsub = nullptr;
    }

    ScopedSubscription& operator=(ScopedSubscription&& o) noexcept {
        if (this != &o) {
            if (m_unsub) m_unsub();
            m_bus = o.m_bus;
            m_id = o.m_id;
            m_unsub = std::move(o.m_unsub);
            o.m_bus = nullptr;
            o.m_id = kInvalidSubscriptionId;
            o.m_unsub = nullptr;
        }
        return *this;
    }

    void cancel() {
        if (m_unsub) {
            m_unsub();
            m_unsub = nullptr;
        }
        m_id = kInvalidSubscriptionId;
    }

    SubscriptionId id() const { return m_id; }

private:
    EventBus* m_bus = nullptr;
    SubscriptionId m_id = kInvalidSubscriptionId;
    std::function<void()> m_unsub;
};

} // namespace nf
