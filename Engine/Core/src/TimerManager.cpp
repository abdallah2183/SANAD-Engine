#include <NF/Core/TimerManager.hpp>
#include <algorithm>

namespace nf {

TimerManager& TimerManager::instance() {
    static TimerManager inst;
    return inst;
}

TimerManager::TimerManager() = default;

TimerHandle TimerManager::set_timeout(float delay_seconds, std::function<void()> callback) {
    if (!callback) {
        return TimerHandle();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    uint32_t idx = 0;
    if (!m_free_indices.empty()) {
        idx = m_free_indices.back();
        m_free_indices.pop_back();
    } else {
        idx = static_cast<uint32_t>(m_entries.size());
        m_entries.emplace_back();
    }

    auto& entry = m_entries[idx];
    entry.id = idx + 1;
    entry.rate = std::max(0.0f, delay_seconds);
    entry.remaining = entry.rate;
    entry.elapsed = 0.0f;
    entry.is_interval = false;
    entry.is_paused = false;
    entry.in_use = true;
    entry.callback = std::move(callback);

    return TimerHandle(entry.id, entry.generation);
}

TimerHandle TimerManager::set_interval(float interval_seconds, std::function<void()> callback) {
    if (!callback) {
        return TimerHandle();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    uint32_t idx = 0;
    if (!m_free_indices.empty()) {
        idx = m_free_indices.back();
        m_free_indices.pop_back();
    } else {
        idx = static_cast<uint32_t>(m_entries.size());
        m_entries.emplace_back();
    }

    auto& entry = m_entries[idx];
    entry.id = idx + 1;
    entry.rate = std::max(0.0001f, interval_seconds);
    entry.remaining = entry.rate;
    entry.elapsed = 0.0f;
    entry.is_interval = true;
    entry.is_paused = false;
    entry.in_use = true;
    entry.callback = std::move(callback);

    return TimerHandle(entry.id, entry.generation);
}

bool TimerManager::clear_timer(TimerHandle handle) {
    if (!handle.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (handle.id > m_entries.size()) {
        return false;
    }

    uint32_t idx = handle.id - 1;
    auto& entry = m_entries[idx];
    if (!entry.in_use || entry.generation != handle.generation) {
        return false;
    }

    entry.in_use = false;
    entry.callback = nullptr;
    entry.generation++;
    if (entry.generation == 0) {
        entry.generation = 1;
    }
    m_free_indices.push_back(idx);
    return true;
}

bool TimerManager::pause_timer(TimerHandle handle) {
    if (!handle.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (handle.id > m_entries.size()) {
        return false;
    }

    uint32_t idx = handle.id - 1;
    auto& entry = m_entries[idx];
    if (!entry.in_use || entry.generation != handle.generation) {
        return false;
    }

    entry.is_paused = true;
    return true;
}

bool TimerManager::resume_timer(TimerHandle handle) {
    if (!handle.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (handle.id > m_entries.size()) {
        return false;
    }

    uint32_t idx = handle.id - 1;
    auto& entry = m_entries[idx];
    if (!entry.in_use || entry.generation != handle.generation) {
        return false;
    }

    entry.is_paused = false;
    return true;
}

bool TimerManager::is_timer_active(TimerHandle handle) const {
    if (!handle.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (handle.id > m_entries.size()) {
        return false;
    }

    uint32_t idx = handle.id - 1;
    const auto& entry = m_entries[idx];
    return entry.in_use && entry.generation == handle.generation && !entry.is_paused;
}

bool TimerManager::is_timer_paused(TimerHandle handle) const {
    if (!handle.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (handle.id > m_entries.size()) {
        return false;
    }

    uint32_t idx = handle.id - 1;
    const auto& entry = m_entries[idx];
    return entry.in_use && entry.generation == handle.generation && entry.is_paused;
}

float TimerManager::get_time_remaining(TimerHandle handle) const {
    if (!handle.valid()) {
        return -1.0f;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (handle.id > m_entries.size()) {
        return -1.0f;
    }

    uint32_t idx = handle.id - 1;
    const auto& entry = m_entries[idx];
    if (!entry.in_use || entry.generation != handle.generation) {
        return -1.0f;
    }

    return entry.remaining;
}

float TimerManager::get_elapsed_time(TimerHandle handle) const {
    if (!handle.valid()) {
        return -1.0f;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (handle.id > m_entries.size()) {
        return -1.0f;
    }

    uint32_t idx = handle.id - 1;
    const auto& entry = m_entries[idx];
    if (!entry.in_use || entry.generation != handle.generation) {
        return -1.0f;
    }

    return entry.elapsed;
}

void TimerManager::set_time_dilation(float dilation) noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_time_dilation = std::max(0.0f, dilation);
}

float TimerManager::get_time_dilation() const noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_time_dilation;
}

void TimerManager::update(float dt) {
    if (dt <= 0.0f) {
        return;
    }

    float scaled_dt = 0.0f;
    std::vector<std::function<void()>> to_fire;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        scaled_dt = dt * m_time_dilation;
        if (scaled_dt <= 0.0f) {
            return;
        }

        for (size_t i = 0; i < m_entries.size(); ++i) {
            auto& e = m_entries[i];
            if (!e.in_use || e.is_paused) {
                continue;
            }

            e.elapsed += scaled_dt;
            e.remaining -= scaled_dt;

            if (e.remaining <= 0.0f) {
                if (e.callback) {
                    to_fire.push_back(e.callback);
                }

                if (e.is_interval) {
                    e.remaining += e.rate;
                    if (e.remaining <= 0.0f) {
                        e.remaining = e.rate;
                    }
                } else {
                    e.in_use = false;
                    e.callback = nullptr;
                    e.generation++;
                    if (e.generation == 0) {
                        e.generation = 1;
                    }
                    m_free_indices.push_back(static_cast<uint32_t>(i));
                }
            }
        }
    }

    for (const auto& cb : to_fire) {
        if (cb) {
            cb();
        }
    }
}

void TimerManager::clear_all() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = 0; i < m_entries.size(); ++i) {
        auto& e = m_entries[i];
        if (e.in_use) {
            e.in_use = false;
            e.callback = nullptr;
            e.generation++;
            if (e.generation == 0) {
                e.generation = 1;
            }
            m_free_indices.push_back(static_cast<uint32_t>(i));
        }
    }
}

size_t TimerManager::active_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t count = 0;
    for (const auto& e : m_entries) {
        if (e.in_use) {
            count++;
        }
    }
    return count;
}

} // namespace nf
