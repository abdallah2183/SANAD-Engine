#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nf::scene2d {

/// Playback looping behavior
enum class SpriteLoopMode : u8 {
    Once = 0,     ///< Stop at the last frame
    Loop,         ///< Loop endlessly from frame 0
    PingPong      ///< Play forward to end, then backward to start, and repeat
};

/// A single keyframe within a 2D flipbook animation clip
struct SpriteFrame {
    std::string region_name;         ///< Atlas region name
    f32 duration = 0.1f;             ///< Frame display duration in seconds
    Vec2 origin_offset{0.0f, 0.0f};  ///< Pivot / origin offset in pixels
    std::string notify_event;        ///< Optional event name triggered when this frame begins
};

/// An ordered sequence of sprite frames forming a clip
class SpriteAnimationClip {
public:
    explicit SpriteAnimationClip(std::string name = "", SpriteLoopMode loop_mode = SpriteLoopMode::Loop);

    void set_name(std::string name) { m_name = std::move(name); }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }

    void set_loop_mode(SpriteLoopMode mode) noexcept { m_loop_mode = mode; }
    [[nodiscard]] SpriteLoopMode loop_mode() const noexcept { return m_loop_mode; }

    /// Add a frame
    void add_frame(std::string region_name, f32 duration = 0.1f, Vec2 origin_offset = {0.0f, 0.0f}, std::string notify = "");

    /// Add multiple frames with uniform frame duration from names
    void add_frames(const std::vector<std::string>& region_names, f32 uniform_duration = 0.1f);

    [[nodiscard]] usize frame_count() const noexcept { return m_frames.size(); }
    [[nodiscard]] const SpriteFrame* frame(usize index) const;
    [[nodiscard]] f32 total_duration() const noexcept { return m_total_duration; }

    /// Clear all frames
    void clear() noexcept;

private:
    void recalculate_duration() noexcept;

    std::string m_name;
    SpriteLoopMode m_loop_mode = SpriteLoopMode::Loop;
    std::vector<SpriteFrame> m_frames;
    f32 m_total_duration = 0.0f;
};

/// Evaluation state returned by the animator
struct SpritePlaybackState {
    usize current_frame_index = 0;
    std::string region_name;
    Vec2 origin_offset{0.0f, 0.0f};
    f32 normalized_time = 0.0f;      ///< [0, 1] through the clip
    bool is_finished = false;
    bool is_playing = false;
};

/// Flipbook animation player
class SpriteAnimator {
public:
    SpriteAnimator() = default;

    /// Play an animation clip
    void play(const SpriteAnimationClip* clip, bool restart = true);

    /// Stop playback and reset to first frame
    void stop() noexcept;

    /// Pause or resume
    void pause() noexcept { m_paused = true; }
    void resume() noexcept { m_paused = false; }
    [[nodiscard]] bool is_paused() const noexcept { return m_paused; }

    /// Playback rate multiplier (default 1.0f)
    void set_speed(f32 speed) noexcept { m_speed = speed; }
    [[nodiscard]] f32 speed() const noexcept { return m_speed; }

    /// Advance time and collect triggered notifies
    SpritePlaybackState update(f32 dt, std::vector<std::string>* out_notifies = nullptr);

    /// Explicitly seek to a specific time
    void set_time(f32 time_seconds) noexcept;

    [[nodiscard]] const SpritePlaybackState& current_state() const noexcept { return m_state; }
    [[nodiscard]] const SpriteAnimationClip* current_clip() const noexcept { return m_clip; }
    [[nodiscard]] f32 elapsed_time() const noexcept { return m_time; }

private:
    const SpriteAnimationClip* m_clip = nullptr;
    f32 m_time = 0.0f;
    f32 m_speed = 1.0f;
    bool m_paused = false;
    bool m_reverse = false;
    usize m_last_notified_frame = static_cast<usize>(-1);
    SpritePlaybackState m_state;
};

/// Comparison types for state transitions
enum class SpriteConditionType : u8 {
    Equals = 0,
    NotEquals,
    Greater,
    Less,
    HasFinished
};

/// Transition rule between two states in a 2D state machine
struct SpriteTransition {
    std::string to_state;
    std::string parameter_name;
    SpriteConditionType condition_type = SpriteConditionType::Equals;
    f32 threshold = 0.0f;
    bool has_exit_time = false;
};

/// 2D Sprite Flipbook State Machine
class SpriteStateMachine2D {
public:
    SpriteStateMachine2D() = default;

    /// Add a named state with its flipbook animation clip
    void add_state(std::string state_name, SpriteAnimationClip clip);

    /// Add transition from source state to target state
    void add_transition(const std::string& from_state, SpriteTransition transition);

    /// Set default starting state
    void set_default_state(const std::string& state_name);

    /// Set blackboard parameters
    void set_float(const std::string& name, f32 value);
    void set_bool(const std::string& name, bool value);
    void set_int(const std::string& name, i32 value);

    [[nodiscard]] f32 get_float(const std::string& name, f32 default_val = 0.0f) const;
    [[nodiscard]] bool get_bool(const std::string& name, bool default_val = false) const;
    [[nodiscard]] i32 get_int(const std::string& name, i32 default_val = 0) const;

    /// Force state change immediately
    void change_state(const std::string& state_name);

    /// Update state machine, evaluate transitions, advance animator
    SpritePlaybackState update(f32 dt, std::vector<std::string>* out_notifies = nullptr);

    /// Current active state name
    [[nodiscard]] const std::string& current_state_name() const noexcept { return m_current_state; }

    /// Animator accessor
    [[nodiscard]] const SpriteAnimator& animator() const noexcept { return m_animator; }

private:
    bool evaluate_transition(const SpriteTransition& tr) const;

    std::unordered_map<std::string, SpriteAnimationClip> m_states;
    std::unordered_map<std::string, std::vector<SpriteTransition>> m_transitions;
    std::unordered_map<std::string, f32> m_parameters;

    std::string m_current_state;
    std::string m_default_state;
    SpriteAnimator m_animator;
};

} // namespace nf::scene2d
