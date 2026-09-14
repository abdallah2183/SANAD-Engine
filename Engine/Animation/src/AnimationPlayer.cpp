#include <NF/Animation/AnimationPlayer.hpp>
#include <algorithm>
#include <cmath>

namespace nf::animation {

f32 AnimationPlayer::update(f32 dt, f32 clip_duration) {
    if (m_state != PlayState::Playing) {
        return m_time;
    }
    if (clip_duration <= 0.0f) {
        return 0.0f;
    }

    f32 new_time = m_time + dt * m_speed * m_ping_pong_dir;

    switch (m_loop) {
        case LoopMode::None: {
            if (new_time >= clip_duration) {
                m_time = clip_duration;
                m_state = PlayState::Stopped;
            } else if (new_time < 0.0f) {
                m_time = 0.0f;
                m_state = PlayState::Stopped;
            } else {
                m_time = new_time;
            }
            break;
        }
        case LoopMode::Loop: {
            if (new_time >= clip_duration) {
                m_time = std::fmod(new_time, clip_duration);
                if (m_time < 0.0f) m_time += clip_duration;
            } else if (new_time < 0.0f) {
                m_time = clip_duration + std::fmod(new_time, clip_duration);
            } else {
                m_time = new_time;
            }
            break;
        }
        case LoopMode::PingPong: {
            if (new_time >= clip_duration) {
                f32 overshoot = new_time - clip_duration;
                m_time = clip_duration - overshoot;
                m_ping_pong_dir = -1.0f;
                if (m_time < 0.0f) {
                    m_time = -m_time;
                    m_ping_pong_dir = 1.0f;
                }
            } else if (new_time < 0.0f) {
                m_time = -new_time;
                m_ping_pong_dir = 1.0f;
                if (m_time >= clip_duration) {
                    m_time = clip_duration - (m_time - clip_duration);
                    m_ping_pong_dir = -1.0f;
                }
            } else {
                m_time = new_time;
            }
            break;
        }
    }

    return m_time;
}

} // namespace nf::animation
