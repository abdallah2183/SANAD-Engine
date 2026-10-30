#include <NF/Gameplay/Sequencer.hpp>
#include <cmath>

namespace nf::gameplay {

namespace {

float smoothstep_interp(float t) {
    return t * t * (3.0f - 2.0f * t);
}

template<typename T>
T interpolate_keys(const Keyframe<T>& a, const Keyframe<T>& b, float time) {
    if (time <= a.time) return a.value;
    if (time >= b.time) return b.value;
    const float span = b.time - a.time;
    if (span <= 1e-6f) return a.value;

    const float t = (time - a.time) / span;
    switch (a.interp) {
        case InterpolationMode::Step:
            return a.value;
        case InterpolationMode::CubicHermite: {
            const float s = smoothstep_interp(t);
            return a.value + (b.value - a.value) * s;
        }
        case InterpolationMode::Linear:
        default:
            return a.value + (b.value - a.value) * t;
    }
}

} // namespace

void FloatTrack::add_key(float time, float value, InterpolationMode interp) {
    Keyframe<float> k{time, value, interp};
    auto it = std::lower_bound(m_keys.begin(), m_keys.end(), time,
        [](const Keyframe<float>& a, float t) { return a.time < t; });
    m_keys.insert(it, k);
}

float FloatTrack::evaluate(float time, float default_val) const {
    if (m_keys.empty()) return default_val;
    if (time <= m_keys.front().time) return m_keys.front().value;
    if (time >= m_keys.back().time) return m_keys.back().value;

    for (size_t i = 0; i + 1 < m_keys.size(); ++i) {
        if (time >= m_keys[i].time && time <= m_keys[i + 1].time) {
            return interpolate_keys(m_keys[i], m_keys[i + 1], time);
        }
    }
    return m_keys.back().value;
}

void VectorTrack::add_key(float time, const Vec3& value, InterpolationMode interp) {
    Keyframe<Vec3> k{time, value, interp};
    auto it = std::lower_bound(m_keys.begin(), m_keys.end(), time,
        [](const Keyframe<Vec3>& a, float t) { return a.time < t; });
    m_keys.insert(it, k);
}

Vec3 VectorTrack::evaluate(float time, const Vec3& default_val) const {
    if (m_keys.empty()) return default_val;
    if (time <= m_keys.front().time) return m_keys.front().value;
    if (time >= m_keys.back().time) return m_keys.back().value;

    for (size_t i = 0; i + 1 < m_keys.size(); ++i) {
        if (time >= m_keys[i].time && time <= m_keys[i + 1].time) {
            return interpolate_keys(m_keys[i], m_keys[i + 1], time);
        }
    }
    return m_keys.back().value;
}

void EventTrack::add_event(float time, std::string_view name, std::string_view payload) {
    EventKey k{time, std::string(name), std::string(payload)};
    auto it = std::lower_bound(m_events.begin(), m_events.end(), time,
        [](const EventKey& a, float t) { return a.time < t; });
    m_events.insert(it, k);
}

void EventTrack::collect_events(float start_time, float end_time, std::vector<EventKey>& out_triggered) const {
    const float t0 = std::min(start_time, end_time);
    const float t1 = std::max(start_time, end_time);
    for (const auto& ev : m_events) {
        if (ev.time >= t0 && ev.time <= t1) {
            out_triggered.push_back(ev);
        }
    }
}

CutsceneSequence::CutsceneSequence(std::string name, float duration)
    : m_name(std::move(name)), m_duration(std::max(0.01f, duration)) {}

void CutsceneSequence::play() {
    m_state = PlaybackState::Playing;
}

void CutsceneSequence::pause() {
    m_state = PlaybackState::Paused;
}

void CutsceneSequence::stop() {
    m_state = PlaybackState::Stopped;
    m_time = 0.0f;
    m_ping_pong_reverse = false;
}

void CutsceneSequence::scrub_to(float time) {
    m_time = std::clamp(time, 0.0f, m_duration);
}

void CutsceneSequence::step(float dt, std::vector<EventTrack::EventKey>* out_events) {
    if (m_state != PlaybackState::Playing) return;

    const float prev_time = m_time;
    const float delta = dt * m_playback_speed;

    if (m_loop_mode == LoopMode::PingPong) {
        if (m_ping_pong_reverse) {
            m_time -= delta;
            if (m_time <= 0.0f) {
                m_time = -m_time;
                m_ping_pong_reverse = false;
            }
        } else {
            m_time += delta;
            if (m_time >= m_duration) {
                m_time = m_duration - (m_time - m_duration);
                m_ping_pong_reverse = true;
            }
        }
    } else {
        m_time += delta;
        if (m_time >= m_duration) {
            if (m_loop_mode == LoopMode::Loop) {
                m_time = std::fmod(m_time, m_duration);
            } else {
                m_time = m_duration;
                m_state = PlaybackState::Stopped;
            }
        }
    }

    if (out_events) {
        m_event_track.collect_events(prev_time, m_time, *out_events);
    }
}

FloatTrack& CutsceneSequence::add_float_track(std::string name) {
    for (auto& tr : m_float_tracks) {
        if (tr.name() == name) return tr;
    }
    m_float_tracks.emplace_back(std::move(name));
    return m_float_tracks.back();
}

VectorTrack& CutsceneSequence::add_vector_track(std::string name) {
    for (auto& tr : m_vector_tracks) {
        if (tr.name() == name) return tr;
    }
    m_vector_tracks.emplace_back(std::move(name));
    return m_vector_tracks.back();
}

const FloatTrack* CutsceneSequence::find_float_track(std::string_view name) const {
    for (const auto& tr : m_float_tracks) {
        if (tr.name() == name) return &tr;
    }
    return nullptr;
}

const VectorTrack* CutsceneSequence::find_vector_track(std::string_view name) const {
    for (const auto& tr : m_vector_tracks) {
        if (tr.name() == name) return &tr;
    }
    return nullptr;
}

float CutsceneSequence::evaluate_float(std::string_view name, float default_val) const {
    const FloatTrack* tr = find_float_track(name);
    return tr ? tr->evaluate(m_time, default_val) : default_val;
}

Vec3 CutsceneSequence::evaluate_vector(std::string_view name, const Vec3& default_val) const {
    const VectorTrack* tr = find_vector_track(name);
    return tr ? tr->evaluate(m_time, default_val) : default_val;
}

} // namespace nf::gameplay
