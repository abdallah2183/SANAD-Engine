#pragma once

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace nf::ai {

/// Posture required or provided by the cover point
enum class CoverPosture : u8 {
    Low = 0,  ///< Half wall / sandbag / barrier (~1.0m height, crouch cover)
    High = 1  ///< Full wall / pillar / container (~2.0m height, standing cover)
};

/// Peeking and firing directions available at this cover point
struct CoverDirection {
    static constexpr u8 None  = 0;
    static constexpr u8 Left  = 1 << 0; ///< Can lean/peek out left to fire
    static constexpr u8 Right = 1 << 1; ///< Can lean/peek out right to fire
    static constexpr u8 Over  = 1 << 2; ///< Can pop up over top to fire (typical for Low cover)
    static constexpr u8 All   = Left | Right | Over;
};

/// A single tactical cover point in the game world
struct CoverPoint {
    u32 id = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};  ///< World position of the cover standing/crouching spot
    Vec3 normal{0.0f, 0.0f, 1.0f};    ///< Outward normal of the cover obstacle facing towards threat
    CoverPosture posture = CoverPosture::Low;
    u8 peek_directions = CoverDirection::None;
    u32 claimed_by = 0;               ///< ID of agent occupying this point (0 = unclaimed)
    f32 custom_score_modifier = 0.0f; ///< Optional designer authored score bias
    bool enabled = true;
};

/// Parameters for querying tactical cover
struct CoverQuery {
    Vec3 threat_position{0.0f, 0.0f, 0.0f};   ///< Position of the enemy or incoming fire
    Vec3 querier_position{0.0f, 0.0f, 0.0f};  ///< Position of the agent seeking cover
    f32 min_distance_to_threat = 4.0f;        ///< Reject cover too close to the threat
    f32 max_distance_to_threat = 1000.0f;     ///< Maximum distance to the threat
    f32 min_distance_from_querier = 0.0f;     ///< Minimum distance from agent
    f32 max_distance_from_querier = 35.0f;    ///< Search radius from the agent
    f32 flank_angle_degrees = 60.0f;          ///< Max angle between normal and threat before flagged as flanked
    bool require_unclaimed = true;            ///< If true, ignores points claimed by other agents
    bool filter_posture = false;              ///< If true, must match requested_posture
    CoverPosture requested_posture = CoverPosture::Low;
    u8 required_peek_direction = CoverDirection::None; ///< If non-zero, must have this peek flag

    // Scoring weights
    f32 weight_threat_facing = 1.0f;          ///< Weight for how directly the cover faces the threat
    f32 weight_distance_to_querier = 0.8f;    ///< Weight favoring cover closer to agent
    f32 weight_distance_to_threat = 0.4f;     ///< Weight favoring standoff distance from threat
};

/// Result of a cover query
struct CoverQueryResult {
    u32 cover_id = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 0.0f, 0.0f};
    CoverPosture posture = CoverPosture::Low;
    u8 peek_directions = CoverDirection::None;
    f32 total_score = -1.0f;
    bool valid = false;
};

/// Tactical Cover System managing smart cover points, claims, and spatial queries
class CoverSystem {
public:
    CoverSystem() = default;

    /// Register a new cover point. If point.id is 0, auto-assigns an incremental ID.
    u32 register_cover_point(CoverPoint point);

    /// Remove a cover point by ID
    bool unregister_cover_point(u32 id);

    /// Lookup a cover point by ID (returns nullptr if not found)
    [[nodiscard]] const CoverPoint* get_cover_point(u32 id) const;

    /// Total number of registered cover points
    [[nodiscard]] size_t cover_point_count() const noexcept { return m_cover_points.size(); }

    /// Claim a cover point for an agent. Fails if already claimed by another agent.
    bool claim_cover(u32 cover_id, u32 agent_id);

    /// Release a cover point claim
    bool release_cover(u32 cover_id);

    /// Release all covers currently claimed by an agent
    void release_all_for_agent(u32 agent_id);

    /// Get the cover ID claimed by an agent (or 0 if none)
    [[nodiscard]] u32 get_agent_claimed_cover(u32 agent_id) const;

    /// Test if a cover point is flanked by the threat (threat has an angle beyond flank_angle_degrees)
    [[nodiscard]] bool is_flanked(u32 cover_id, const Vec3& threat_pos, f32 max_flank_angle_deg = 60.0f) const;

    /// Evaluates if a cover point satisfies a query, returning its calculated score
    [[nodiscard]] bool evaluate_cover_point(const CoverPoint& point, const CoverQuery& query, f32* out_score = nullptr) const;

    /// Find the best cover point according to query parameters and weights
    [[nodiscard]] CoverQueryResult find_best_cover(const CoverQuery& query) const;

    /// Find all valid cover points satisfying query, sorted by score descending
    void find_all_valid_covers(const CoverQuery& query, std::vector<CoverQueryResult>& out_results) const;

    /// Procedurally generate a row of cover points along a wall / barrier
    /// Points are placed along the segment between wall_start and wall_end, offset by -wall_normal * standoff.
    std::vector<u32> generate_wall_cover(
        const Vec3& wall_start,
        const Vec3& wall_end,
        const Vec3& wall_normal,
        CoverPosture posture,
        f32 spacing = 2.0f,
        f32 standoff = 0.5f
    );

    /// Clear all cover points and claims
    void clear();

private:
    std::vector<CoverPoint> m_cover_points;
    u32 m_next_id = 1;
};

} // namespace nf::ai
