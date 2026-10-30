#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace nf::ai {

/// An interaction slot on a smart object (e.g., chair seat, workbench station)
struct SmartObjectSlot {
    u32 slot_index = 0;
    Vec3 offset{0.0f, 0.0f, 0.0f};            ///< Local offset relative to object
    Quat rotation_offset = Quat::identity();  ///< Local orientation facing during interaction
    u32 claimed_by = 0;                       ///< Agent ID currently occupying slot (0 = free)
    bool enabled = true;

    [[nodiscard]] bool is_available() const noexcept {
        return enabled && claimed_by == 0;
    }
};

/// Authoring definition and behavior traits of a smart object
struct SmartObjectDef {
    std::string activity_name;                ///< e.g. "Sit", "Work", "Rest", "Eat", "PatrolStand"
    std::vector<std::string> required_tags;   ///< Tags the actor must possess to use this object
    std::vector<std::string> granted_tags;    ///< Tags granted to the actor during interaction
    f32 interaction_duration = 5.0f;          ///< Nominal duration of the interaction in seconds
    f32 cooldown = 0.0f;                      ///< Cooldown before object can be used again after release
    f32 priority = 1.0f;                      ///< Scoring priority multiplier
};

/// An instantiated smart object in the game world
struct SmartObjectInstance {
    u32 id = 0;
    Vec3 world_position{0.0f, 0.0f, 0.0f};
    Quat world_rotation = Quat::identity();
    SmartObjectDef definition;
    std::vector<SmartObjectSlot> slots;
    f32 current_cooldown = 0.0f;
    bool is_enabled = true;

    /// Get world-space transform of a specific slot
    bool get_slot_transform(u32 slot_index, Vec3& out_pos, Quat& out_rot) const {
        if (slot_index >= slots.size()) return false;
        const auto& s = slots[slot_index];
        out_pos = world_position + world_rotation.rotate(s.offset);
        out_rot = world_rotation * s.rotation_offset;
        return true;
    }

    /// Check if object has at least one free slot and is off cooldown
    [[nodiscard]] bool has_available_slot() const noexcept {
        if (!is_enabled || current_cooldown > 0.0f) return false;
        for (const auto& s : slots) {
            if (s.is_available()) return true;
        }
        return false;
    }
};

/// Parameters for querying suitable smart objects
struct SmartObjectQuery {
    Vec3 querier_position{0.0f, 0.0f, 0.0f};  ///< Position of the AI searching for interaction
    f32 max_distance = 50.0f;                 ///< Search radius in world meters
    std::string activity_name;                ///< Activity required (empty matches any)
    std::vector<std::string> actor_tags;      ///< Tags held by the actor
    bool require_available_slot = true;       ///< Whether an unoccupied slot is required
};

/// Result of a smart object query
struct SmartObjectQueryResult {
    u32 object_id = 0;
    u32 slot_index = 0;
    Vec3 slot_world_pos{0.0f, 0.0f, 0.0f};
    Quat slot_world_rot = Quat::identity();
    std::string activity_name;
    f32 score = -1.0f;
    bool valid = false;
};

/// Smart Object Manager handling world smart object registration,
/// spatial queries, slot claims, and cooldown tracking.
class SmartObjectManager {
public:
    SmartObjectManager() = default;

    /// Register a new smart object. If obj.id is 0, auto-assigns an incremental ID.
    u32 register_smart_object(SmartObjectInstance obj);

    /// Remove a smart object by ID.
    bool unregister_smart_object(u32 id);

    /// Lookup smart object by ID (returns nullptr if not found).
    [[nodiscard]] const SmartObjectInstance* get_smart_object(u32 id) const;
    [[nodiscard]] SmartObjectInstance* get_smart_object_mut(u32 id);

    /// Total count of registered smart objects.
    [[nodiscard]] size_t object_count() const noexcept { return m_objects.size(); }

    /// Claim a slot on a smart object for an agent.
    bool claim_slot(u32 object_id, u32 slot_index, u32 agent_id);

    /// Release a slot on a smart object, triggering cooldown if configured.
    bool release_slot(u32 object_id, u32 slot_index);

    /// Release all slots currently claimed by an agent.
    void release_all_for_agent(u32 agent_id);

    /// Find which object and slot an agent has claimed (returns false if none).
    bool get_agent_claim(u32 agent_id, u32& out_object_id, u32& out_slot_index) const;

    /// Evaluate if a smart object satisfies query requirements and calculate score.
    [[nodiscard]] bool evaluate_object(
        const SmartObjectInstance& obj,
        const SmartObjectQuery& query,
        u32& out_best_slot,
        f32& out_score
    ) const;

    /// Find the highest scoring smart object and slot matching the query.
    [[nodiscard]] SmartObjectQueryResult find_best_smart_object(const SmartObjectQuery& query) const;

    /// Find all matching smart objects sorted by score descending.
    void find_all_matching(
        const SmartObjectQuery& query,
        std::vector<SmartObjectQueryResult>& out_results
    ) const;

    /// Advance cooldown timers.
    void update(f32 dt);

    /// Clear all registered smart objects.
    void clear();

private:
    std::vector<SmartObjectInstance> m_objects;
    u32 m_next_id = 1;
};

} // namespace nf::ai
