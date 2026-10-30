#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>
#include <NF/Foliage/Foliage.hpp>

#include <span>
#include <vector>

namespace nf::foliage {

/// Shape or mode of a dynamic foliage interactor.
enum class InteractorShape : u32 {
    Sphere = 0,   // Point-radius influence (character feet, rolling ball, explosion)
    Capsule,      // Swept line segment between start and end (character body, long vehicle axle)
};

/// Dynamic entity capable of flattening or displacing foliage (player, vehicle, projectile, blast).
struct FoliageInteractor {
    u32 id = 0u;
    InteractorShape shape = InteractorShape::Sphere;
    Vec3 position{0.0f, 0.0f, 0.0f};      // Center of sphere or start of capsule
    Vec3 end_position{0.0f, 0.0f, 0.0f};  // End of capsule (if shape == Capsule)
    Vec3 velocity{0.0f, 0.0f, 0.0f};      // Directional velocity for wake / forward push
    f32 radius = 1.0f;                    // Radius of influence (meters)
    f32 strength = 1.0f;                  // Displacement strength multiplier [0, inf)
    f32 push_down_ratio = 0.5f;           // [0, 1]: fraction pushed downwards vs laterally outwards
    bool active = true;
};

/// Result of evaluating foliage interaction at a world position.
struct FoliageBendResult {
    Vec3 displacement{0.0f, 0.0f, 0.0f}; // 3D world displacement vector (applied to plant tips/vertices)
    f32 bend_angle = 0.0f;               // Radians tilted away from normal
    f32 depression = 0.0f;               // Flattening ratio [0, 1] (0 = fully upright, 1 = fully flattened)
    f32 influence = 0.0f;                // Weight of active interactors [0, 1]
};

/// Persistent impression left on foliage (e.g. footsteps, vehicle tire tracks, explosion craters).
struct TrampleStamp {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 push_dir{0.0f, -1.0f, 0.0f};
    f32 radius = 0.5f;
    f32 depth = 1.0f;       // Initial depression depth [0, 1]
    f32 age = 0.0f;         // Elapsed seconds
    f32 lifetime = 5.0f;    // Seconds until completely recovered
};

/// Spring-damper restitution parameters for plants springing back to vertical.
struct FoliageSpringParams {
    f32 stiffness = 25.0f;     // Spring stiffness k
    f32 damping = 6.0f;        // Damping coefficient c
    f32 mass = 1.0f;           // Effective blade mass
};

/// Field managing active interactors, persistent trample trails, and dynamic recovery.
class FoliageBendField {
public:
    FoliageBendField() = default;

    /// Registers a new interactor and returns its unique ID.
    u32 register_interactor(const FoliageInteractor& interactor);

    /// Updates existing interactor state (e.g. moved position, velocity).
    bool update_interactor(u32 id, const Vec3& new_pos, const Vec3& velocity);
    bool set_interactor(const FoliageInteractor& interactor);

    /// Removes an interactor by ID.
    bool unregister_interactor(u32 id);

    /// Clears all interactors and trample stamps.
    void clear();

    /// Adds a persistent trample mark (e.g. vehicle wheel track stamp).
    void add_trample_stamp(const Vec3& position, f32 radius, f32 depth = 1.0f,
                           f32 lifetime = 5.0f, const Vec3& push_dir = Vec3{0.0f, -1.0f, 0.0f});

    /// Advances time: decays trample stamps and updates internal state.
    void update(f32 dt);

    /// Evaluates bend displacement at a specific world coordinate.
    FoliageBendResult evaluate(const Vec3& world_pos, f32 plant_height = 1.0f, f32 plant_stiffness = 1.0f) const;

    /// Evaluates combined displacement (wind sway + interactor push) for a FoliageInstance.
    Vec3 evaluate_combined(const FoliageInstance& instance,
                           const FoliageType& type,
                           const WindState& wind,
                           f32 plant_stiffness = 1.0f) const;

    /// Fast batch evaluation for instanced renderers.
    void evaluate_batch(std::span<const FoliageInstance> instances,
                        const FoliageType& type,
                        const WindState& wind,
                        std::span<Vec3> out_displacements,
                        f32 plant_stiffness = 1.0f) const;

    size_t active_interactor_count() const;
    size_t trample_stamp_count() const { return m_stamps.size(); }

    void set_spring_params(const FoliageSpringParams& params) { m_spring_params = params; }
    const FoliageSpringParams& spring_params() const { return m_spring_params; }

    void set_max_displacement(f32 max_dist) { m_max_displacement = max_dist; }
    f32 max_displacement() const { return m_max_displacement; }

private:
    u32 m_next_id = 1u;
    std::vector<FoliageInteractor> m_interactors;
    std::vector<TrampleStamp> m_stamps;
    FoliageSpringParams m_spring_params{};
    f32 m_max_displacement = 3.0f;
};

} // namespace nf::foliage
