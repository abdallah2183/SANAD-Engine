#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <vector>
#include <functional>
#include <cmath>

namespace nf::scene {

/// Type of control point tangent generation and interpolation
enum class SplinePointType : u8 {
    Linear = 0,     // Sharp corner; chord straight lines
    CurveAuto,      // Catmull-Rom smooth tangent calculated from neighbors
    CurveClamped,   // Auto tangent with magnitude clamped to avoid overshoot
    CurveCustom     // Tangents manually authored by user/designer
};

/// Definition of a single control point along the 3D spline
struct SplinePoint {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 arrive_tangent{0.0f, 0.0f, 0.0f}; // Incoming tangent vector
    Vec3 leave_tangent{0.0f, 0.0f, 0.0f};  // Outgoing tangent vector
    Quat rotation{0.0f, 0.0f, 0.0f, 1.0f}; // Keyframe orientation (roll/pitch/yaw)
    Vec3 scale{1.0f, 1.0f, 1.0f};          // Scale multiplier along the curve
    SplinePointType type = SplinePointType::CurveAuto;

    constexpr SplinePoint() = default;
    constexpr explicit SplinePoint(const Vec3& pos, SplinePointType pt_type = SplinePointType::CurveAuto)
        : position(pos), type(pt_type) {}
};

/// Evaluated sample state at any point along the spline
struct SplineSample {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 tangent{0.0f, 0.0f, 1.0f};   // Forward direction vector (normalized)
    Vec3 up{0.0f, 1.0f, 0.0f};        // Up direction vector (normalized)
    Vec3 right{1.0f, 0.0f, 0.0f};     // Right direction vector (normalized)
    Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    f32 curvature = 0.0f;             // Signed/magnitude curvature (1/R) for banking
    f32 distance = 0.0f;              // Arc length along the curve from the start
    f32 t = 0.0f;                     // Normalized parameter [0, 1]
};

/// 3D Parametric Cubic Hermite / Catmull-Rom Spline with Arc-Length Parameterization
class Spline3D {
public:
    Spline3D() = default;

    /// Control points management
    void add_point(const SplinePoint& point);
    void add_point(const Vec3& pos, SplinePointType type = SplinePointType::CurveAuto);
    void insert_point(size_t index, const SplinePoint& point);
    void remove_point(size_t index);
    void clear();

    void set_point_position(size_t index, const Vec3& pos);
    void set_point_tangents(size_t index, const Vec3& arrive, const Vec3& leave);
    void set_point_rotation(size_t index, const Quat& rot);
    void set_point_scale(size_t index, const Vec3& scale);
    void set_point_type(size_t index, SplinePointType type);

    [[nodiscard]] size_t point_count() const noexcept { return m_points.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_points.empty(); }
    [[nodiscard]] const SplinePoint& get_point(size_t index) const;
    [[nodiscard]] SplinePoint& get_point(size_t index);

    /// Loop / Closed track toggle
    void set_closed(bool closed);
    [[nodiscard]] bool is_closed() const noexcept { return m_is_closed; }

    /// Number of interpolatable curve segments
    [[nodiscard]] size_t segment_count() const noexcept;

    /// Recalculates Catmull-Rom tangents for all CurveAuto/CurveClamped points
    void update_auto_tangents();

    /// Arc-Length parameterization table
    /// Samples each segment with uniform subdivisions to construct an arc-length LUT,
    /// enabling constant-speed motion regardless of non-uniform control point spacing.
    void rebuild_arc_length_table(size_t samples_per_segment = 32);

    [[nodiscard]] f32 total_length() const noexcept { return m_total_length; }

    /// Convert world distance [0, total_length] to normalized parameter t [0, 1]
    [[nodiscard]] f32 distance_to_time(f32 distance) const;

    /// Convert normalized parameter t [0, 1] to world distance [0, total_length]
    [[nodiscard]] f32 time_to_distance(f32 t) const;

    /// Evaluate spline by normalized parameter t in [0, 1]
    [[nodiscard]] SplineSample evaluate(f32 t) const;

    /// Evaluate spline by exact distance along the path in world units (constant speed)
    [[nodiscard]] SplineSample evaluate_at_distance(f32 distance) const;

    /// Finds the closest point on the spline to a world-space query point
    [[nodiscard]] SplineSample find_nearest_point(const Vec3& world_pos, f32& out_distance, size_t subdivisions = 64) const;

    /// Evaluate raw position and derivative in segment i with parameter u in [0, 1]
    [[nodiscard]] Vec3 evaluate_segment_position(size_t seg_idx, f32 u) const;
    [[nodiscard]] Vec3 evaluate_segment_tangent(size_t seg_idx, f32 u) const;
    [[nodiscard]] Vec3 evaluate_segment_acceleration(size_t seg_idx, f32 u) const;

private:
    struct ArcLengthSample {
        f32 t = 0.0f;
        f32 cumulative_distance = 0.0f;
    };

    std::vector<SplinePoint> m_points;
    std::vector<ArcLengthSample> m_arc_length_table;
    f32 m_total_length = 0.0f;
    bool m_is_closed = false;
    bool m_dirty = true;
};

/// Follow mode along a path
enum class SplineFollowMode : u8 {
    Once = 0,       // Stops at the end of the path
    Loop,           // Wraps around from the end to the start
    PingPong        // Reverses direction upon reaching either end
};

/// Motion controller for following a 3D spline curve with constant speed and bank angles
class SplineFollower {
public:
    SplineFollower() = default;
    explicit SplineFollower(const Spline3D* spline);

    void set_spline(const Spline3D* spline);
    [[nodiscard]] const Spline3D* spline() const noexcept { return m_spline; }

    void set_speed(f32 speed) noexcept { m_speed = speed; }
    [[nodiscard]] f32 speed() const noexcept { return m_speed; }

    void set_mode(SplineFollowMode mode) noexcept { m_mode = mode; }
    [[nodiscard]] SplineFollowMode mode() const noexcept { return m_mode; }

    /// Banking multiplier: tilts roll angle based on curve curvature (ideal for rollercoasters & aircraft)
    void set_bank_multiplier(f32 multiplier) noexcept { m_bank_multiplier = multiplier; }
    [[nodiscard]] f32 bank_multiplier() const noexcept { return m_bank_multiplier; }

    /// Progresses the follower forward by delta time dt
    void update(f32 dt);

    /// Seek to specific distance along spline
    void seek_distance(f32 distance);

    /// Seek to normalized progress [0, 1]
    void seek_ratio(f32 ratio);

    /// Reset back to start
    void reset();

    [[nodiscard]] const SplineSample& current_sample() const noexcept { return m_current_sample; }
    [[nodiscard]] f32 current_distance() const noexcept { return m_current_distance; }
    [[nodiscard]] bool is_finished() const noexcept { return m_finished; }
    [[nodiscard]] bool is_reverse() const noexcept { return m_reverse; }

private:
    const Spline3D* m_spline = nullptr;
    SplineSample m_current_sample;
    f32 m_speed = 5.0f;
    f32 m_current_distance = 0.0f;
    f32 m_bank_multiplier = 0.0f;
    SplineFollowMode m_mode = SplineFollowMode::Once;
    bool m_finished = false;
    bool m_reverse = false;
};

/// ECS component for entities carrying a spline path (e.g. patrol track, camera rail)
struct SplineComponent {
    Spline3D spline;
    bool auto_rebuild = true;
};

} // namespace nf::scene
