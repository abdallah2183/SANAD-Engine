#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Scene2D/Camera2D.hpp>

#include <algorithm>
#include <cmath>

namespace nf::scene2d {

/// Configuration for 2D Camera Follower
struct CameraFollower2DConfig {
    /// Half-extents of the deadzone box around camera position in world units.
    /// While the target is inside this box, the camera does not move.
    Vec2 deadzone{1.0f, 0.5f};

    /// Smoothing time (seconds) along X and Y axes for smooth critically damped tracking.
    Vec2 smooth_time{0.15f, 0.15f};

    /// Maximum tracking speed in world units per second.
    f32 max_speed = 1000.0f;

    /// Lookahead scale factor based on target velocity.
    Vec2 lookahead_factor{0.2f, 0.1f};

    /// Smoothing time for lookahead offset.
    f32 lookahead_smooth_time = 0.25f;

    /// Optional level boundaries.
    bool bounds_enabled = false;
    Vec2 bounds_min{-1000.0f, -1000.0f};
    Vec2 bounds_max{1000.0f, 1000.0f};

    /// Trauma-based screen shake configuration.
    f32 trauma_decay = 1.2f;            ///< Trauma reduction per second.
    Vec2 max_shake_offset{1.5f, 1.5f};  ///< Maximum translational shake offset at trauma = 1.
    f32 max_shake_roll_deg = 4.0f;      ///< Maximum rotational roll angle at trauma = 1.
    f32 shake_frequency = 28.0f;        ///< Oscillation frequency in radians/sec.
};

/// 2D cinematic camera follower with deadzone, velocity lookahead,
/// critically damped smoothing, boundary clamping, and trauma-based screen shake.
class CameraFollower2D {
public:
    explicit CameraFollower2D(CameraFollower2DConfig config = {});

    /// Set initial position immediately without smoothing or velocity build-up.
    void snap_to(Vec2 pos);

    /// Step the follower forward in time.
    /// @param target_pos Current position of the tracked entity.
    /// @param target_vel Velocity of the tracked entity (for lookahead).
    /// @param dt Delta time in seconds.
    void update(Vec2 target_pos, Vec2 target_vel, f32 dt);

    /// Apply final smoothed position and shake roll to Camera2D.
    void apply_to_camera(Camera2D& camera) const;

    /// Add trauma to trigger screen shake (clamped to [0, 1]).
    void add_trauma(f32 amount) noexcept;

    /// Current trauma value in [0, 1].
    [[nodiscard]] f32 trauma() const noexcept { return m_trauma; }

    /// Smoothed camera position before screen shake.
    [[nodiscard]] Vec2 current_position() const noexcept { return m_current_pos; }

    /// Final camera position after adding screen shake offset.
    [[nodiscard]] Vec2 final_position() const noexcept { return m_current_pos + m_shake_offset; }

    /// Shake roll angle offset (degrees).
    [[nodiscard]] f32 shake_roll_deg() const noexcept { return m_shake_roll; }

    /// Target focus point including deadzone push and lookahead.
    [[nodiscard]] Vec2 target_focus() const noexcept { return m_target_focus; }

    /// Configuration access
    [[nodiscard]] const CameraFollower2DConfig& config() const noexcept { return m_config; }
    void set_config(CameraFollower2DConfig cfg) noexcept { m_config = cfg; }

    /// Reset internal velocity and shake state.
    void reset();

private:
    CameraFollower2DConfig m_config;

    Vec2 m_current_pos{0.0f, 0.0f};
    Vec2 m_velocity{0.0f, 0.0f};

    Vec2 m_target_focus{0.0f, 0.0f};
    Vec2 m_lookahead_offset{0.0f, 0.0f};
    Vec2 m_lookahead_velocity{0.0f, 0.0f};

    f32 m_trauma = 0.0f;
    f32 m_shake_timer = 0.0f;
    Vec2 m_shake_offset{0.0f, 0.0f};
    f32 m_shake_roll = 0.0f;
};

} // namespace nf::scene2d
