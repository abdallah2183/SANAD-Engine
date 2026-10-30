#pragma once

#include <NF/Core/Math.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

namespace nf::gameplay {

enum class InterpolationMode : uint8_t {
    Step,
    Linear,
    CubicHermite
};

template<typename T>
struct Keyframe {
    float time = 0.0f;
    T value{};
    InterpolationMode interp = InterpolationMode::Linear;
};

/// Animates a single scalar floating point value over time.
class FloatTrack {
public:
    explicit FloatTrack(std::string name = "") : m_name(std::move(name)) {}

    [[nodiscard]] const std::string& name() const { return m_name; }
    [[nodiscard]] const std::vector<Keyframe<float>>& keys() const { return m_keys; }
    [[nodiscard]] size_t key_count() const { return m_keys.size(); }

    void add_key(float time, float value, InterpolationMode interp = InterpolationMode::Linear);
    [[nodiscard]] float evaluate(float time, float default_val = 0.0f) const;

private:
    std::string m_name;
    std::vector<Keyframe<float>> m_keys;
};

/// Animates a 3D vector property (e.g. translation, scale, RGB color) over time.
class VectorTrack {
public:
    explicit VectorTrack(std::string name = "") : m_name(std::move(name)) {}

    [[nodiscard]] const std::string& name() const { return m_name; }
    [[nodiscard]] const std::vector<Keyframe<Vec3>>& keys() const { return m_keys; }
    [[nodiscard]] size_t key_count() const { return m_keys.size(); }

    void add_key(float time, const Vec3& value, InterpolationMode interp = InterpolationMode::Linear);
    [[nodiscard]] Vec3 evaluate(float time, const Vec3& default_val = Vec3{}) const;

private:
    std::string m_name;
    std::vector<Keyframe<Vec3>> m_keys;
};

/// Fires discrete named gameplay cues, sound triggers, or camera switches during timeline playback.
class EventTrack {
public:
    struct EventKey {
        float time = 0.0f;
        std::string name;
        std::string payload;
    };

    explicit EventTrack(std::string name = "Events") : m_name(std::move(name)) {}

    [[nodiscard]] const std::string& name() const { return m_name; }
    [[nodiscard]] const std::vector<EventKey>& events() const { return m_events; }
    [[nodiscard]] size_t event_count() const { return m_events.size(); }

    void add_event(float time, std::string_view name, std::string_view payload = "");
    void collect_events(float start_time, float end_time, std::vector<EventKey>& out_triggered) const;

private:
    std::string m_name;
    std::vector<EventKey> m_events;
};

enum class PlaybackState : uint8_t {
    Stopped,
    Playing,
    Paused
};

enum class LoopMode : uint8_t {
    Once,
    Loop,
    PingPong
};

/// High-level cinematic cutscene and timeline sequencer.
class CutsceneSequence {
public:
    explicit CutsceneSequence(std::string name = "Sequence", float duration = 5.0f);

    [[nodiscard]] const std::string& name() const { return m_name; }
    [[nodiscard]] float duration() const { return m_duration; }
    void set_duration(float dur) { m_duration = std::max(0.01f, dur); }

    [[nodiscard]] float current_time() const { return m_time; }
    [[nodiscard]] float playback_speed() const { return m_playback_speed; }
    void set_playback_speed(float speed) { m_playback_speed = speed; }

    [[nodiscard]] PlaybackState state() const { return m_state; }
    [[nodiscard]] LoopMode loop_mode() const { return m_loop_mode; }
    void set_loop_mode(LoopMode mode) { m_loop_mode = mode; }

    void play();
    void pause();
    void stop();
    void scrub_to(float time);
    void step(float dt, std::vector<EventTrack::EventKey>* out_events = nullptr);

    FloatTrack& add_float_track(std::string name);
    VectorTrack& add_vector_track(std::string name);
    EventTrack& event_track() { return m_event_track; }
    [[nodiscard]] const EventTrack& event_track() const { return m_event_track; }

    [[nodiscard]] const FloatTrack* find_float_track(std::string_view name) const;
    [[nodiscard]] const VectorTrack* find_vector_track(std::string_view name) const;

    [[nodiscard]] float evaluate_float(std::string_view name, float default_val = 0.0f) const;
    [[nodiscard]] Vec3 evaluate_vector(std::string_view name, const Vec3& default_val = Vec3{}) const;

private:
    std::string m_name;
    float m_duration = 5.0f;
    float m_time = 0.0f;
    float m_playback_speed = 1.0f;
    PlaybackState m_state = PlaybackState::Stopped;
    LoopMode m_loop_mode = LoopMode::Once;
    bool m_ping_pong_reverse = false;

    std::vector<FloatTrack> m_float_tracks;
    std::vector<VectorTrack> m_vector_tracks;
    EventTrack m_event_track;
};

} // namespace nf::gameplay
