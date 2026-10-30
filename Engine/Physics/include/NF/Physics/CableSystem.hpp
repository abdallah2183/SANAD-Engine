#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <algorithm>
#include <span>
#include <vector>

namespace nf::physics {

/// A single lumped mass point along the cable
struct CableParticle {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 old_position{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    f32 inv_mass = 1.0f; // 0.0f = pinned/fixed anchor
};

/// Distance constraint connecting adjacent particles
struct CableSegmentConstraint {
    u32 p0 = 0;
    u32 p1 = 0;
    f32 rest_length = 0.5f;
    f32 current_tension = 0.0f; // Estimated tension in Newtons
    bool active = true;
};

/// Cable / Rope physics configuration
struct CableConfig {
    u32 particle_count = 16;             ///< Number of particles along the cable (>= 2)
    f32 total_length = 10.0f;            ///< Total rest length in meters
    f32 linear_density = 0.5f;           ///< Mass in kg per meter
    f32 stiffness = 0.95f;               ///< Distance constraint stiffness [0, 1]
    f32 damping = 0.03f;                 ///< Velocity damping per second
    f32 max_tension = 5000.0f;           ///< Breaking tension threshold (Newtons)
    bool breakable = false;              ///< Whether excessive tension breaks the cable
    u32 solver_iterations = 8;           ///< Gauss-Seidel constraint solver iterations
    u32 substeps = 2;                    ///< Physics substeps per step() call
    Vec3 gravity{0.0f, -9.81f, 0.0f};    ///< World gravity vector
};

/// Dynamic cable and rope simulator using Verlet integration and distance constraints
class CableSimulation {
public:
    CableSimulation();
    explicit CableSimulation(const CableConfig& config);

    /// Reset particles between two endpoints
    void initialize_endpoints(const Vec3& start_point, const Vec3& end_point);

    /// Physics step advancement
    void step(f32 dt);

    /// Pin / unpin endpoints or interior particles
    void pin_particle(u32 index, const Vec3& pos);
    void unpin_particle(u32 index);
    void move_pinned_particle(u32 index, const Vec3& new_pos);

    /// Convenience hooks for start and end anchors
    void set_start_point(const Vec3& pos, bool pinned = true);
    void set_end_point(const Vec3& pos, bool pinned = true);

    /// Configuration access
    [[nodiscard]] const CableConfig& config() const noexcept { return m_config; }
    void set_config(const CableConfig& config);

    /// Query state
    [[nodiscard]] usize particle_count() const noexcept { return m_particles.size(); }
    [[nodiscard]] const CableParticle& particle(usize index) const { return m_particles[index]; }
    [[nodiscard]] bool is_broken() const noexcept { return m_broken; }

    /// Tension forces at endpoints in Newtons
    [[nodiscard]] f32 start_tension() const noexcept;
    [[nodiscard]] f32 end_tension() const noexcept;
    [[nodiscard]] f32 max_current_tension() const noexcept;

    /// Read particle positions as a contiguous span
    [[nodiscard]] std::vector<Vec3> get_positions() const;

    /// Sample interpolated 3D position along the cable (t in [0, 1])
    [[nodiscard]] Vec3 sample_position(f32 t) const;

    /// Ground plane collision: prevents cable particles from falling below ground_y
    void set_ground_plane(bool enable, f32 ground_y = 0.0f) noexcept {
        m_ground_collision = enable;
        m_ground_y = ground_y;
    }

    /// Add spherical obstacle for cable deflection
    void add_sphere_obstacle(const Vec3& center, f32 radius);
    void clear_sphere_obstacles() noexcept { m_sphere_obstacles.clear(); }

private:
    struct SphereObstacle {
        Vec3 center;
        f32 radius = 1.0f;
    };

    void solve_substep(f32 sub_dt);
    void solve_constraints(f32 sub_dt);
    void solve_collisions();

    CableConfig m_config;
    std::vector<CableParticle> m_particles;
    std::vector<CableSegmentConstraint> m_constraints;
    std::vector<SphereObstacle> m_sphere_obstacles;

    bool m_broken = false;
    bool m_ground_collision = false;
    f32 m_ground_y = 0.0f;
};

} // namespace nf::physics
