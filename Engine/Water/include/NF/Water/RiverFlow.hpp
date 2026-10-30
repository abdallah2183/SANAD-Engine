#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace nf::water {

/// A control waypoint along a procedural river spline
struct RiverWaypoint {
    Vec3 position{0.0f, 0.0f, 0.0f}; ///< Centerline 3D point
    f32 width = 10.0f;               ///< Total channel width in meters
    f32 depth = 2.5f;                ///< Water depth at channel center in meters
    f32 flow_speed = 2.0f;           ///< Flow speed in meters per second
};

/// Query result from sampling a river velocity field
struct RiverSample {
    bool in_river = false;           ///< True if test position falls inside river channel
    Vec3 surface_point{0.0f, 0.0f, 0.0f}; ///< Closest point on the water surface
    Vec3 flow_velocity{0.0f, 0.0f, 0.0f}; ///< Directional current vector (m/s)
    f32 water_surface_y = 0.0f;      ///< Altitude of water surface at this point
    f32 riverbed_y = 0.0f;           ///< Altitude of riverbed at this point
    f32 depth = 0.0f;                ///< Water depth at this point
    f32 normalized_cross = 0.0f;     ///< Lateral position: -1.0 (left bank), 0.0 (center), +1.0 (right bank)
    f32 bank_foam_factor = 0.0f;     ///< [0, 1] foam intensity near shoreline
};

/// A river channel defined by an ordered series of spline waypoints
class RiverCorridor {
public:
    explicit RiverCorridor(std::string name = "");

    void set_name(std::string name) { m_name = std::move(name); }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }

    /// Add waypoint to the end of the river (upstream to downstream)
    void add_waypoint(const RiverWaypoint& wp);

    /// Clear all waypoints
    void clear() noexcept;

    [[nodiscard]] usize waypoint_count() const noexcept { return m_waypoints.size(); }
    [[nodiscard]] const RiverWaypoint* waypoint(usize index) const;

    /// Sample river flow, depth, and surface properties at world position
    [[nodiscard]] RiverSample sample(const Vec3& world_pos) const;

    /// Total path length of the river centerline
    [[nodiscard]] f32 total_length() const noexcept { return m_total_length; }

private:
    void update_spline_cache() noexcept;

    std::string m_name;
    std::vector<RiverWaypoint> m_waypoints;
    std::vector<f32> m_segment_lengths;
    f32 m_total_length = 0.0f;
};

/// River flow manager managing multiple corridors and buoyant drift kinematics
class RiverFlowSystem {
public:
    RiverFlowSystem() = default;

    /// Register a river corridor. Returns index.
    usize add_river(RiverCorridor river);

    /// Clear all rivers
    void clear() noexcept;

    [[nodiscard]] usize river_count() const noexcept { return m_rivers.size(); }
    [[nodiscard]] RiverCorridor* get_river(usize index);
    [[nodiscard]] const RiverCorridor* get_river(usize index) const;

    /// Sample all rivers at world position, returning the dominant river sample
    [[nodiscard]] RiverSample sample(const Vec3& world_pos) const;

    /// Simulate physical water drift, buoyancy, and current drag on a floating object
    /// object_pos: world position (updated in-place)
    /// object_vel: world velocity (updated in-place)
    /// buoyancy: vertical upward buoyant force coefficient (e.g. 15.0 m/s^2)
    /// water_drag: current coupling coefficient (e.g. 4.0 1/s)
    /// gravity: downward gravitational acceleration (default 9.81 m/s^2)
    /// dt: delta time in seconds
    void apply_drift(
        Vec3& object_pos,
        Vec3& object_vel,
        f32 buoyancy,
        f32 water_drag,
        f32 dt,
        f32 gravity = 9.81f
    ) const;

private:
    std::vector<RiverCorridor> m_rivers;
};

} // namespace nf::water
