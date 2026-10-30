#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Water/Water.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace nf::water {

/// Configuration options for the interactive 2D water ripple grid
struct WaterRippleOptions {
    u32 resolution = 128;          // Grid resolution (W = H = resolution), e.g. 128x128
    f32 world_size = 50.0f;        // Extent of the simulated grid in world units (meters)
    Vec2 world_center{0.0f, 0.0f}; // Center of the grid in world XZ
    f32 wave_speed = 8.0f;         // Propagation speed of ripples (c in wave equation)
    f32 damping = 0.985f;          // Energy retention factor per second [0.8, 0.999]
    f32 max_height = 2.0f;         // Maximum allowed ripple displacement (meters)
};

/// Combined water surface sample blending global Gerstner waves and local dynamic ripples
struct CombinedWaterSample {
    f32 height = 0.0f;
    Vec3 normal{0.0f, 1.0f, 0.0f};
};

/// 2D Shallow Water Wave Equation Simulation Grid
/// Evaluates interactive ripples caused by footsteps, splashes, boat wakes, and rain drops.
class WaterRippleGrid {
public:
    explicit WaterRippleGrid(const WaterRippleOptions& options = WaterRippleOptions{});
    ~WaterRippleGrid() = default;

    /// Reinitializes the grid with new configuration options
    void init(const WaterRippleOptions& options);

    /// Reset all ripple heights and velocities to zero
    void clear();

    /// Advance simulation by time step dt (substeps automatically for CFL numerical stability)
    void step(f32 dt);

    /// Inject an instantaneous circular disturbance (e.g. footstep splash, bullet hit, falling stone)
    void add_ripple(const Vec2& world_pos, f32 radius, f32 strength);

    /// Inject a wake impulse from a moving body (character wading, boat moving)
    void add_wake(const Vec3& world_pos, const Vec3& velocity, f32 radius, f32 strength, f32 dt);

    /// Sample interpolated ripple displacement at world coordinates (world_x, world_z)
    [[nodiscard]] f32 sample_height(f32 world_x, f32 world_z) const;

    /// Sample surface normal perturbation caused by ripples
    [[nodiscard]] Vec3 sample_normal(f32 world_x, f32 world_z) const;

    /// Sample local vertical surface velocity (dh/dt)
    [[nodiscard]] f32 sample_velocity(f32 world_x, f32 world_z) const;

    [[nodiscard]] const WaterRippleOptions& options() const noexcept { return m_options; }
    [[nodiscard]] u32 resolution() const noexcept { return m_options.resolution; }
    [[nodiscard]] f32 cell_size() const noexcept { return m_cell_size; }
    [[nodiscard]] f32 total_energy() const noexcept;

    /// Combined evaluation: blends global Gerstner waves with local interactive ripples
    [[nodiscard]] CombinedWaterSample evaluate_combined(
        f32 world_x,
        f32 world_z,
        const std::vector<Wave>& waves,
        const WaterOptions& water_opts) const;

private:
    [[nodiscard]] bool world_to_grid(f32 world_x, f32 world_z, f32& out_gx, f32& out_gz) const;
    [[nodiscard]] size_t index(u32 x, u32 z) const noexcept { return static_cast<size_t>(z * m_options.resolution + x); }

    WaterRippleOptions m_options;
    f32 m_cell_size = 1.0f;
    std::vector<f32> m_height_current;
    std::vector<f32> m_height_previous;
    std::vector<f32> m_height_next;
};

} // namespace nf::water
