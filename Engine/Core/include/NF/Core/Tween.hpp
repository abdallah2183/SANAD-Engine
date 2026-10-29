#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Core/Easing.hpp>
#include <NF/Core/Math.hpp>

#include <functional>
#include <vector>
#include <memory>

namespace nf {

enum class TweenState : u8 {
    Playing = 0,
    Paused,
    Finished
};

class ITween {
public:
    virtual ~ITween() = default;
    virtual void update(float dt) = 0;
    virtual bool is_finished() const = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    virtual uint64_t id() const = 0;
};

/// High-performance scalar and vector tween interpolator.
class ScalarTween : public ITween {
public:
    ScalarTween(uint64_t id, float start, float end, float duration, EaseType ease = EaseType::OutQuad);

    void update(float dt) override;
    bool is_finished() const override { return m_state == TweenState::Finished; }
    void pause() override { if (m_state == TweenState::Playing) m_state = TweenState::Paused; }
    void resume() override { if (m_state == TweenState::Paused) m_state = TweenState::Playing; }
    void stop() override { m_state = TweenState::Finished; }
    uint64_t id() const override { return m_id; }

    ScalarTween& on_update(std::function<void(float val)> cb) { m_on_update = std::move(cb); return *this; }
    ScalarTween& on_complete(std::function<void()> cb) { m_on_complete = std::move(cb); return *this; }
    ScalarTween& set_delay(float delay) { m_delay = delay; return *this; }
    ScalarTween& set_loops(int loops, bool ping_pong = false) { m_loops = loops; m_ping_pong = ping_pong; return *this; }

    float current_value() const { return m_current; }

private:
    uint64_t m_id = 0;
    float m_start = 0.0f;
    float m_end = 0.0f;
    float m_current = 0.0f;
    float m_duration = 1.0f;
    float m_elapsed = 0.0f;
    float m_delay = 0.0f;
    EaseType m_ease = EaseType::OutQuad;
    TweenState m_state = TweenState::Playing;

    int m_loops = 1;
    int m_completed_loops = 0;
    bool m_ping_pong = false;
    bool m_forward = true;

    std::function<void(float)> m_on_update;
    std::function<void()> m_on_complete;
};

/// Vector3 tween interpolator
class Vector3Tween : public ITween {
public:
    Vector3Tween(uint64_t id, Vec3 start, Vec3 end, float duration, EaseType ease = EaseType::OutQuad);

    void update(float dt) override;
    bool is_finished() const override { return m_state == TweenState::Finished; }
    void pause() override { if (m_state == TweenState::Playing) m_state = TweenState::Paused; }
    void resume() override { if (m_state == TweenState::Paused) m_state = TweenState::Playing; }
    void stop() override { m_state = TweenState::Finished; }
    uint64_t id() const override { return m_id; }

    Vector3Tween& on_update(std::function<void(const Vec3& val)> cb) { m_on_update = std::move(cb); return *this; }
    Vector3Tween& on_complete(std::function<void()> cb) { m_on_complete = std::move(cb); return *this; }
    Vector3Tween& set_delay(float delay) { m_delay = delay; return *this; }
    Vector3Tween& set_loops(int loops, bool ping_pong = false) { m_loops = loops; m_ping_pong = ping_pong; return *this; }

    const Vec3& current_value() const { return m_current; }

private:
    uint64_t m_id = 0;
    Vec3 m_start{0.0f, 0.0f, 0.0f};
    Vec3 m_end{0.0f, 0.0f, 0.0f};
    Vec3 m_current{0.0f, 0.0f, 0.0f};
    float m_duration = 1.0f;
    float m_elapsed = 0.0f;
    float m_delay = 0.0f;
    EaseType m_ease = EaseType::OutQuad;
    TweenState m_state = TweenState::Playing;

    int m_loops = 1;
    int m_completed_loops = 0;
    bool m_ping_pong = false;
    bool m_forward = true;

    std::function<void(const Vec3&)> m_on_update;
    std::function<void()> m_on_complete;
};

/// Central tween manager ticking and managing active tweens.
class TweenManager {
public:
    static TweenManager& instance();

    TweenManager(const TweenManager&) = delete;
    TweenManager& operator=(const TweenManager&) = delete;
    TweenManager(TweenManager&&) = default;
    TweenManager& operator=(TweenManager&&) = default;

    /// Starts a scalar tween binding to an existing float pointer.
    uint64_t to(float* target, float target_val, float duration, EaseType ease = EaseType::OutQuad);

    /// Starts a Vec3 tween binding to an existing Vec3 pointer.
    uint64_t to(Vec3* target, const Vec3& target_val, float duration, EaseType ease = EaseType::OutQuad);

    /// Starts a custom callback-driven scalar tween.
    uint64_t custom(float start, float end, float duration,
                    std::function<void(float)> on_update,
                    EaseType ease = EaseType::OutQuad);

    /// Updates all active tweens by dt seconds. Cleans up finished tweens.
    void update(float dt);

    /// Cancels / stops a tween by ID.
    bool cancel(uint64_t tween_id);

    /// Pauses a tween by ID.
    bool pause(uint64_t tween_id);

    /// Resumes a paused tween by ID.
    bool resume(uint64_t tween_id);

    /// Cancels and clears all active tweens.
    void clear();

    /// Returns the number of currently active tweens.
    size_t active_count() const { return m_tweens.size(); }

private:
    TweenManager() = default;

    uint64_t m_next_id = 1;
    std::vector<std::unique_ptr<ITween>> m_tweens;
};

} // namespace nf
