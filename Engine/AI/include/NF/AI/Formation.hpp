#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <vector>

namespace nf::ai {

/// Tactical squad formation types
enum class FormationType : u8 {
    Line = 0,    ///< Ranks abreast perpendicular to leader heading
    Column,      ///< Single file trailing behind leader
    Wedge,       ///< Chevron / V-shape with leader at apex
    Box,         ///< Rectangular grid of rows and columns
    Circle,      ///< Radial surround pattern around leader
    Custom       ///< User-defined relative offsets
};

/// A designated slot in a tactical squad formation
struct FormationSlot {
    u32 slot_id = 0;
    Vec3 relative_offset{0.0f, 0.0f, 0.0f}; ///< Local offset relative to leader (X = right, Y = up, Z = forward)
    u64 assigned_agent_id = 0;              ///< 0 if unoccupied
    bool is_occupied = false;
};

/// Tactical squad formation manager
class FormationGroup {
public:
    FormationGroup() = default;

    /// Initialize squad formation pattern with a given unit count and spacing
    void generate(FormationType type, u32 slot_count, f32 spacing = 2.0f);

    /// Manually add a custom slot
    u32 add_custom_slot(const Vec3& local_offset);

    /// Clear all slots
    void clear_slots() noexcept;

    /// Update leader's world transform
    void set_leader(const Vec3& position, const Vec3& forward) noexcept;

    /// Assign an agent to the first available slot. Returns slot_id or -1 if full.
    i32 assign_agent(u64 agent_id);

    /// Assign an agent to the open slot closest to the agent's current position to minimize crossing
    i32 assign_agent_nearest(u64 agent_id, const Vec3& agent_world_pos);

    /// Release an agent from its assigned slot
    bool unassign_agent(u64 agent_id);

    /// Get slot world position and forward orientation
    bool get_slot_world_transform(u32 slot_id, Vec3& out_world_pos, Vec3& out_world_fwd) const;

    /// Find slot assigned to an agent
    [[nodiscard]] const FormationSlot* get_agent_slot(u64 agent_id) const;

    [[nodiscard]] usize slot_count() const noexcept { return m_slots.size(); }
    [[nodiscard]] usize occupied_count() const noexcept;
    [[nodiscard]] bool is_full() const noexcept;

    [[nodiscard]] FormationType formation_type() const noexcept { return m_type; }
    [[nodiscard]] f32 spacing() const noexcept { return m_spacing; }
    [[nodiscard]] const Vec3& leader_position() const noexcept { return m_leader_pos; }
    [[nodiscard]] const Vec3& leader_forward() const noexcept { return m_leader_fwd; }

    [[nodiscard]] const std::vector<FormationSlot>& slots() const noexcept { return m_slots; }

private:
    FormationType m_type = FormationType::Line;
    f32 m_spacing = 2.0f;
    Vec3 m_leader_pos{0.0f, 0.0f, 0.0f};
    Vec3 m_leader_fwd{0.0f, 0.0f, 1.0f};
    std::vector<FormationSlot> m_slots;
};

/// Individual boid / flocking agent
struct FlockingAgent {
    u64 id = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    f32 max_speed = 5.0f;
    f32 max_force = 10.0f;
};

/// Tuning parameters for Reynolds Boids flocking solver
struct FlockingConfig {
    f32 separation_radius = 2.0f;
    f32 separation_weight = 1.5f;
    f32 alignment_radius = 5.0f;
    f32 alignment_weight = 1.0f;
    f32 cohesion_radius = 5.0f;
    f32 cohesion_weight = 1.0f;
    f32 goal_weight = 1.0f;
    f32 boundary_radius = 0.0f;  ///< 0 = unbounded; >0 = soft tethering sphere around boundary_center
    Vec3 boundary_center{0.0f, 0.0f, 0.0f};
    f32 boundary_weight = 2.0f;
};

/// Autonomous group flocking and swarm simulation solver
class FlockingSolver {
public:
    explicit FlockingSolver(FlockingConfig config = {});

    /// Add an agent to the swarm
    void add_agent(u64 id, const Vec3& pos, const Vec3& vel = {0.0f, 0.0f, 0.0f}, f32 max_speed = 5.0f, f32 max_force = 10.0f);

    /// Remove agent
    bool remove_agent(u64 id);

    /// Clear all agents
    void clear() noexcept;

    /// Number of live agents
    [[nodiscard]] usize agent_count() const noexcept { return m_agents.size(); }

    /// Access agent by index
    [[nodiscard]] FlockingAgent* get_agent_by_index(usize index);
    [[nodiscard]] const FlockingAgent* get_agent_by_index(usize index) const;

    /// Access agent by id
    [[nodiscard]] FlockingAgent* get_agent_by_id(u64 id);
    [[nodiscard]] const FlockingAgent* get_agent_by_id(u64 id) const;

    /// Advance swarm simulation one time step
    void step(f32 dt, const Vec3* optional_goal = nullptr);

    /// Calculate steering force for a specific agent
    [[nodiscard]] Vec3 compute_steering(usize agent_index, const Vec3* optional_goal) const;

    /// Configuration access
    [[nodiscard]] const FlockingConfig& config() const noexcept { return m_config; }
    void set_config(FlockingConfig cfg) noexcept { m_config = cfg; }
    FlockingConfig& config() noexcept { return m_config; }

private:
    FlockingConfig m_config;
    std::vector<FlockingAgent> m_agents;
};

} // namespace nf::ai
