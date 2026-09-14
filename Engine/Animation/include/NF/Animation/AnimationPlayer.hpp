#pragma once

#include <NF/Animation/AnimationClip.hpp>
#include <NF/Animation/Skeleton.hpp>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::animation {

/// Animation playback state.
enum class PlayState {
    Stopped,
    Playing,
    Paused,
};

/// Loop mode for the player.
enum class LoopMode {
    None,       // Stop at the end
    Loop,       // Wrap around
    PingPong,   // Reverse and play back
};

/// AnimationPlayer: drives a single clip forward in time, with speed control,
/// loop modes, and play/pause/stop. Does not own the clip — it references
/// one by name from a clip table.
class AnimationPlayer {
public:
    AnimationPlayer() = default;

    /// Set the clip to play (by name from the clip table the caller owns).
    void set_clip(const std::string& name) { m_clip_name = name; m_time = 0.0f; m_ping_pong_dir = 1.0f; }
    const std::string& clip_name() const { return m_clip_name; }

    void play() { m_state = PlayState::Playing; }
    void pause() { if (m_state == PlayState::Playing) m_state = PlayState::Paused; }
    void stop() { m_state = PlayState::Stopped; m_time = 0.0f; m_ping_pong_dir = 1.0f; }

    PlayState state() const { return m_state; }
    bool is_playing() const { return m_state == PlayState::Playing; }

    void set_time(f32 t) { m_time = t; }
    f32 time() const { return m_time; }

    void set_speed(f32 s) { m_speed = s; }
    f32 speed() const { return m_speed; }

    void set_loop_mode(LoopMode mode) { m_loop = mode; }
    LoopMode loop_mode() const { return m_loop; }

    /// Advance the player by `dt` seconds. Returns the effective time to
    /// sample at. Handles looping, ping-pong, and speed.
    /// If the clip is finished (LoopMode::None and time reached duration),
    /// sets state to Stopped and returns the clamped time.
    f32 update(f32 dt, f32 clip_duration);

    /// Resample: returns the current sampling time without advancing.
    f32 sample_time() const { return m_time; }

private:
    std::string m_clip_name;
    PlayState m_state = PlayState::Stopped;
    f32 m_time = 0.0f;
    f32 m_speed = 1.0f;
    LoopMode m_loop = LoopMode::Loop;
    f32 m_ping_pong_dir = 1.0f; // +1 forward, -1 reverse (for ping-pong)
};

} // namespace nf::animation
