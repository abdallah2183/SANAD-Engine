#pragma once

#include <NF/Core/Types.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nf::gameplay {

/// Type of progression benefit granted by a talent node
enum class TalentNodeType : u8 {
    PassiveStat = 0,    ///< Grants quantitative attribute or multiplier increase
    ActiveAbility,      ///< Unlocks a new gameplay ability
    KeystonePerk        ///< Major game-altering passive rule or mechanic
};

/// A single node in the talent progression graph
struct TalentNode {
    std::string id;                          ///< Unique node identifier
    std::string name;                        ///< Display name
    std::string description;                 ///< Tooltip description
    TalentNodeType type = TalentNodeType::PassiveStat;
    u32 tier = 0;                            ///< Tree-level tier (0 = root tier)
    u32 points_required_in_tree = 0;         ///< Cumulative tree points required before unlocking
    u32 max_rank = 1;                        ///< Maximum investable ranks
    u32 current_rank = 0;                    ///< Currently invested ranks
    u32 cost_per_rank = 1;                   ///< Talent points per rank
    std::vector<std::string> prerequisites;  ///< Node IDs that must be at max_rank to unlock this node
    std::string granted_ability;             ///< Ability ID granted when rank >= 1
    std::string granted_tag;                 ///< Tag granted when rank >= 1
    f32 stat_modifier_per_rank = 0.0f;       ///< Numerical bonus multiplied by current_rank

    [[nodiscard]] bool is_unlocked() const noexcept { return current_rank > 0; }
    [[nodiscard]] bool is_maxed() const noexcept { return current_rank >= max_rank; }
    [[nodiscard]] f32 total_stat_modifier() const noexcept {
        return stat_modifier_per_rank * static_cast<f32>(current_rank);
    }
};

/// Talent tree progression graph managing nodes, tiers, dependencies, and point allocation
class TalentTree {
public:
    TalentTree(std::string id = "default_tree", std::string name = "Talent Tree");

    [[nodiscard]] const std::string& id() const noexcept { return m_id; }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void set_name(std::string name) { m_name = std::move(name); }

    /// Register a new node definition in the tree
    bool add_node(const TalentNode& node);
    [[nodiscard]] const TalentNode* get_node(const std::string& node_id) const;
    [[nodiscard]] TalentNode* get_node(const std::string& node_id);
    [[nodiscard]] const std::vector<TalentNode>& nodes() const noexcept { return m_nodes; }

    /// Available unspent talent points
    [[nodiscard]] u32 available_points() const noexcept { return m_available_points; }
    void set_available_points(u32 pts) noexcept { m_available_points = pts; }
    void add_points(u32 pts) noexcept { m_available_points += pts; }

    /// Total points invested in this tree
    [[nodiscard]] u32 total_spent_points() const noexcept { return m_spent_points; }

    /// Check if a node is currently eligible to receive an investment point
    [[nodiscard]] bool can_invest_point(const std::string& node_id) const;

    /// Check if a point can be refunded without violating tree dependencies or tier gates
    [[nodiscard]] bool can_refund_point(const std::string& node_id) const;

    /// Invest one point into the node. Returns true on success.
    bool invest_point(const std::string& node_id);

    /// Refund one point from the node. Returns true on success.
    bool refund_point(const std::string& node_id);

    /// Resets all node ranks and refunds all spent points back to available_points
    void reset_tree();

    /// Collect all active tags granted by currently invested nodes
    [[nodiscard]] std::vector<std::string> get_active_tags() const;

    /// Collect all active abilities granted by currently invested nodes
    [[nodiscard]] std::vector<std::string> get_active_abilities() const;

    /// Calculate sum of stat modifiers for all nodes matching a specific ability or tag
    [[nodiscard]] f32 get_total_stat_modifier(const std::string& node_id) const;

    /// Serialize current talent allocation state
    [[nodiscard]] std::string serialize() const;

    /// Parse and apply talent allocation state
    bool parse(const std::string& text);

private:
    std::string m_id;
    std::string m_name;
    u32 m_available_points = 0;
    u32 m_spent_points = 0;
    std::vector<TalentNode> m_nodes;
};

} // namespace nf::gameplay
