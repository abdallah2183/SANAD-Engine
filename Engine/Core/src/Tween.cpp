#include <NF/Core/Tween.hpp>
#include <algorithm>

namespace nf {

// --- ScalarTween ---

ScalarTween::ScalarTween(uint64_t id, float start, float end, float duration, EaseType ease)
    : m_id(id)
    , m_start(start)
    , m_end(end)
    , m_current(start)
    , m_duration(std::max(1e-5f, duration))
    , m_ease(ease)
{
}

void ScalarTween::update(float dt) {
    if (m_state != TweenState::Playing) return;

    if (m_delay > 0.0f) {
        m_delay -= dt;
        if (m_delay > 0.0f) return;
        // Overflow past delay into elapsed
        dt = -m_delay;
        m_delay = 0.0f;
    }

    m_elapsed += dt;
    float progress = std::clamp(m_elapsed / m_duration, 0.0f, 1.0f);
    float t = m_forward ? progress : (1.0f - progress);
    float eased_t = evaluate_ease(m_ease, t);

    m_current = m_start + (m_end - m_start) * eased_t;

    if (m_on_update) {
        m_on_update(m_current);
    }

    if (progress >= 1.0f) {
        ++m_completed_loops;
        if (m_loops > 0 && m_completed_loops >= m_loops) {
            m_state = TweenState::Finished;
            if (m_on_complete) {
                m_on_complete();
            }
        } else {
            m_elapsed = 0.0f;
            if (m_ping_pong) {
                m_forward = !m_forward;
            }
        }
    }
}

// --- Vector3Tween ---

Vector3Tween::Vector3Tween(uint64_t id, Vec3 start, Vec3 end, float duration, EaseType ease)
    : m_id(id)
    , m_start(start)
    , m_end(end)
    , m_current(start)
    , m_duration(std::max(1e-5f, duration))
    , m_ease(ease)
{
}

void Vector3Tween::update(float dt) {
    if (m_state != TweenState::Playing) return;

    if (m_delay > 0.0f) {
        m_delay -= dt;
        if (m_delay > 0.0f) return;
        dt = -m_delay;
        m_delay = 0.0f;
    }

    m_elapsed += dt;
    float progress = std::clamp(m_elapsed / m_duration, 0.0f, 1.0f);
    float t = m_forward ? progress : (1.0f - progress);
    float eased_t = evaluate_ease(m_ease, t);

    m_current.x = m_start.x + (m_end.x - m_start.x) * eased_t;
    m_current.y = m_start.y + (m_end.y - m_start.y) * eased_t;
    m_current.z = m_start.z + (m_end.z - m_start.z) * eased_t;

    if (m_on_update) {
        m_on_update(m_current);
    }

    if (progress >= 1.0f) {
        ++m_completed_loops;
        if (m_loops > 0 && m_completed_loops >= m_loops) {
            m_state = TweenState::Finished;
            if (m_on_complete) {
                m_on_complete();
            }
        } else {
            m_elapsed = 0.0f;
            if (m_ping_pong) {
                m_forward = !m_forward;
            }
        }
    }
}

// --- TweenManager ---

TweenManager& TweenManager::instance() {
    static TweenManager mgr;
    return mgr;
}

uint64_t TweenManager::to(float* target, float target_val, float duration, EaseType ease) {
    if (!target) return 0;
    uint64_t id = m_next_id++;
    auto tween = std::make_unique<ScalarTween>(id, *target, target_val, duration, ease);
    tween->on_update([target](float val) {
        *target = val;
    });
    m_tweens.push_back(std::move(tween));
    return id;
}

uint64_t TweenManager::to(Vec3* target, const Vec3& target_val, float duration, EaseType ease) {
    if (!target) return 0;
    uint64_t id = m_next_id++;
    auto tween = std::make_unique<Vector3Tween>(id, *target, target_val, duration, ease);
    tween->on_update([target](const Vec3& val) {
        *target = val;
    });
    m_tweens.push_back(std::move(tween));
    return id;
}

uint64_t TweenManager::custom(float start, float end, float duration,
                              std::function<void(float)> on_update,
                              EaseType ease) {
    uint64_t id = m_next_id++;
    auto tween = std::make_unique<ScalarTween>(id, start, end, duration, ease);
    if (on_update) {
        tween->on_update(std::move(on_update));
    }
    m_tweens.push_back(std::move(tween));
    return id;
}

void TweenManager::update(float dt) {
    for (auto& tween : m_tweens) {
        tween->update(dt);
    }

    // Prune finished tweens
    m_tweens.erase(
        std::remove_if(m_tweens.begin(), m_tweens.end(), [](const std::unique_ptr<ITween>& t) {
            return t->is_finished();
        }),
        m_tweens.end()
    );
}

bool TweenManager::cancel(uint64_t tween_id) {
    for (auto& tween : m_tweens) {
        if (tween->id() == tween_id) {
            tween->stop();
            return true;
        }
    }
    return false;
}

bool TweenManager::pause(uint64_t tween_id) {
    for (auto& tween : m_tweens) {
        if (tween->id() == tween_id) {
            tween->pause();
            return true;
        }
    }
    return false;
}

bool TweenManager::resume(uint64_t tween_id) {
    for (auto& tween : m_tweens) {
        if (tween->id() == tween_id) {
            tween->resume();
            return true;
        }
    }
    return false;
}

void TweenManager::clear() {
    m_tweens.clear();
}

} // namespace nf
