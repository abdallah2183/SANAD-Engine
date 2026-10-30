#include <NF/Scene2D/SpriteAnimation.hpp>

#include <algorithm>
#include <cmath>

namespace nf::scene2d {

// =========================================================================
// SpriteAnimationClip
// =========================================================================

SpriteAnimationClip::SpriteAnimationClip(std::string name, SpriteLoopMode loop_mode)
    : m_name(std::move(name)), m_loop_mode(loop_mode) {}

void SpriteAnimationClip::add_frame(std::string region_name, f32 duration, Vec2 origin_offset, std::string notify) {
    SpriteFrame f;
    f.region_name = std::move(region_name);
    f.duration = std::max(1e-4f, duration);
    f.origin_offset = origin_offset;
    f.notify_event = std::move(notify);
    m_frames.push_back(std::move(f));
    recalculate_duration();
}

void SpriteAnimationClip::add_frames(const std::vector<std::string>& region_names, f32 uniform_duration) {
    for (const auto& name : region_names) {
        add_frame(name, uniform_duration);
    }
}

const SpriteFrame* SpriteAnimationClip::frame(usize index) const {
    if (index >= m_frames.size()) {
        return nullptr;
    }
    return &m_frames[index];
}

void SpriteAnimationClip::clear() noexcept {
    m_frames.clear();
    m_total_duration = 0.0f;
}

void SpriteAnimationClip::recalculate_duration() noexcept {
    m_total_duration = 0.0f;
    for (const auto& f : m_frames) {
        m_total_duration += f.duration;
    }
}

// =========================================================================
// SpriteAnimator
// =========================================================================

void SpriteAnimator::play(const SpriteAnimationClip* clip, bool restart) {
    if (!clip || clip->frame_count() == 0) {
        stop();
        return;
    }

    if (m_clip != clip || restart) {
        m_time = 0.0f;
        m_reverse = false;
        m_last_notified_frame = static_cast<usize>(-1);
    }

    m_clip = clip;
    m_paused = false;
    m_state.is_playing = true;
    m_state.is_finished = false;

    // Immediately resolve first frame
    const auto* f = m_clip->frame(0);
    if (f) {
        m_state.current_frame_index = 0;
        m_state.region_name = f->region_name;
        m_state.origin_offset = f->origin_offset;
        m_state.normalized_time = 0.0f;
    }
}

void SpriteAnimator::stop() noexcept {
    m_clip = nullptr;
    m_time = 0.0f;
    m_reverse = false;
    m_paused = false;
    m_last_notified_frame = static_cast<usize>(-1);
    m_state = SpritePlaybackState{};
}

void SpriteAnimator::set_time(f32 time_seconds) noexcept {
    m_time = std::max(0.0f, time_seconds);
}

SpritePlaybackState SpriteAnimator::update(f32 dt, std::vector<std::string>* out_notifies) {
    if (!m_clip || m_clip->frame_count() == 0) {
        m_state = SpritePlaybackState{};
        return m_state;
    }

    f32 dur = m_clip->total_duration();
    if (!m_paused && dt > 0.0f && dur > 0.0f) {
        f32 step = dt * m_speed;
        switch (m_clip->loop_mode()) {
            case SpriteLoopMode::Once: {
                m_time += step;
                if (m_time >= dur) {
                    m_time = dur;
                    m_state.is_finished = true;
                    m_state.is_playing = false;
                }
                break;
            }
            case SpriteLoopMode::Loop: {
                m_time += step;
                while (m_time >= dur) {
                    m_time -= dur;
                    m_last_notified_frame = static_cast<usize>(-1);
                }
                while (m_time < 0.0f) {
                    m_time += dur;
                }
                break;
            }
            case SpriteLoopMode::PingPong: {
                if (!m_reverse) {
                    m_time += step;
                    if (m_time >= dur) {
                        m_time = std::max(0.0f, 2.0f * dur - m_time);
                        m_reverse = true;
                        m_last_notified_frame = static_cast<usize>(-1);
                    }
                } else {
                    m_time -= step;
                    if (m_time <= 0.0f) {
                        m_time = std::min(dur, -m_time);
                        m_reverse = false;
                        m_last_notified_frame = static_cast<usize>(-1);
                    }
                }
                break;
            }
        }
    }

    // Determine active frame from elapsed time
    usize active_idx = 0;
    f32 accum = 0.0f;
    usize count = m_clip->frame_count();
    for (usize i = 0; i < count; ++i) {
        const auto* fr = m_clip->frame(i);
        accum += fr->duration;
        if (m_time < accum || i == count - 1) {
            active_idx = i;
            break;
        }
    }

    const auto* fr = m_clip->frame(active_idx);
    if (fr) {
        m_state.current_frame_index = active_idx;
        m_state.region_name = fr->region_name;
        m_state.origin_offset = fr->origin_offset;
        m_state.normalized_time = (dur > 0.0f) ? std::clamp(m_time / dur, 0.0f, 1.0f) : 1.0f;

        if (active_idx != m_last_notified_frame) {
            m_last_notified_frame = active_idx;
            if (out_notifies && !fr->notify_event.empty()) {
                out_notifies->push_back(fr->notify_event);
            }
        }
    }

    return m_state;
}

// =========================================================================
// SpriteStateMachine2D
// =========================================================================

void SpriteStateMachine2D::add_state(std::string state_name, SpriteAnimationClip clip) {
    if (m_default_state.empty()) {
        m_default_state = state_name;
    }
    m_states.insert_or_assign(state_name, std::move(clip));
}

void SpriteStateMachine2D::add_transition(const std::string& from_state, SpriteTransition transition) {
    m_transitions[from_state].push_back(std::move(transition));
}

void SpriteStateMachine2D::set_default_state(const std::string& state_name) {
    m_default_state = state_name;
    if (m_current_state.empty()) {
        change_state(state_name);
    }
}

void SpriteStateMachine2D::set_float(const std::string& name, f32 value) {
    m_parameters[name] = value;
}

void SpriteStateMachine2D::set_bool(const std::string& name, bool value) {
    m_parameters[name] = value ? 1.0f : 0.0f;
}

void SpriteStateMachine2D::set_int(const std::string& name, i32 value) {
    m_parameters[name] = static_cast<f32>(value);
}

f32 SpriteStateMachine2D::get_float(const std::string& name, f32 default_val) const {
    auto it = m_parameters.find(name);
    return (it != m_parameters.end()) ? it->second : default_val;
}

bool SpriteStateMachine2D::get_bool(const std::string& name, bool default_val) const {
    auto it = m_parameters.find(name);
    return (it != m_parameters.end()) ? (it->second > 0.5f) : default_val;
}

i32 SpriteStateMachine2D::get_int(const std::string& name, i32 default_val) const {
    auto it = m_parameters.find(name);
    return (it != m_parameters.end()) ? static_cast<i32>(std::round(it->second)) : default_val;
}

void SpriteStateMachine2D::change_state(const std::string& state_name) {
    auto it = m_states.find(state_name);
    if (it == m_states.end()) {
        return;
    }
    m_current_state = state_name;
    m_animator.play(&it->second, true);
}

bool SpriteStateMachine2D::evaluate_transition(const SpriteTransition& tr) const {
    if (tr.has_exit_time && !m_animator.current_state().is_finished) {
        return false;
    }

    if (tr.condition_type == SpriteConditionType::HasFinished) {
        return m_animator.current_state().is_finished;
    }

    f32 val = get_float(tr.parameter_name, 0.0f);
    switch (tr.condition_type) {
        case SpriteConditionType::Equals:
            return std::abs(val - tr.threshold) < 1e-4f;
        case SpriteConditionType::NotEquals:
            return std::abs(val - tr.threshold) >= 1e-4f;
        case SpriteConditionType::Greater:
            return val > tr.threshold;
        case SpriteConditionType::Less:
            return val < tr.threshold;
        default:
            return false;
    }
}

SpritePlaybackState SpriteStateMachine2D::update(f32 dt, std::vector<std::string>* out_notifies) {
    if (m_current_state.empty()) {
        if (!m_default_state.empty()) {
            change_state(m_default_state);
        } else if (!m_states.empty()) {
            change_state(m_states.begin()->first);
        } else {
            return SpritePlaybackState{};
        }
    }

    // First check parameter-driven transitions (e.g. interruption by player input)
    auto tr_it = m_transitions.find(m_current_state);
    if (tr_it != m_transitions.end()) {
        for (const auto& tr : tr_it->second) {
            if (!tr.has_exit_time && evaluate_transition(tr)) {
                change_state(tr.to_state);
                tr_it = m_transitions.find(m_current_state);
                break;
            }
        }
    }

    SpritePlaybackState st = m_animator.update(dt, out_notifies);

    // After advancing, check transitions that wait for exit time / finish
    tr_it = m_transitions.find(m_current_state);
    if (tr_it != m_transitions.end()) {
        for (const auto& tr : tr_it->second) {
            if (tr.has_exit_time && evaluate_transition(tr)) {
                change_state(tr.to_state);
                st = m_animator.update(0.0f, out_notifies);
                break;
            }
        }
    }

    return st;
}

} // namespace nf::scene2d
