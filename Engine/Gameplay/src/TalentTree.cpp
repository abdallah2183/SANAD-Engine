#include <NF/Gameplay/TalentTree.hpp>

#include <algorithm>
#include <sstream>

namespace nf::gameplay {

TalentTree::TalentTree(std::string id, std::string name)
    : m_id(std::move(id))
    , m_name(std::move(name)) {}

bool TalentTree::add_node(const TalentNode& node) {
    if (node.id.empty() || get_node(node.id) != nullptr) {
        return false;
    }
    m_nodes.push_back(node);
    return true;
}

const TalentNode* TalentTree::get_node(const std::string& node_id) const {
    for (const auto& n : m_nodes) {
        if (n.id == node_id) {
            return &n;
        }
    }
    return nullptr;
}

TalentNode* TalentTree::get_node(const std::string& node_id) {
    for (auto& n : m_nodes) {
        if (n.id == node_id) {
            return &n;
        }
    }
    return nullptr;
}

bool TalentTree::can_invest_point(const std::string& node_id) const {
    const TalentNode* node = get_node(node_id);
    if (!node) {
        return false;
    }

    if (node->current_rank >= node->max_rank) {
        return false;
    }

    if (m_available_points < node->cost_per_rank) {
        return false;
    }

    if (m_spent_points < node->points_required_in_tree) {
        return false;
    }

    for (const auto& prereq_id : node->prerequisites) {
        const TalentNode* prereq = get_node(prereq_id);
        if (!prereq || !prereq->is_maxed()) {
            return false;
        }
    }

    return true;
}

bool TalentTree::can_refund_point(const std::string& node_id) const {
    const TalentNode* node = get_node(node_id);
    if (!node || node->current_rank == 0) {
        return false;
    }

    u32 new_rank = node->current_rank - 1;

    // 1. Direct prerequisite check: downstream unlocked nodes cannot be orphaned
    if (new_rank < node->max_rank) {
        for (const auto& other : m_nodes) {
            if (other.current_rank > 0) {
                for (const auto& prereq_id : other.prerequisites) {
                    if (prereq_id == node_id) {
                        return false;
                    }
                }
            }
        }
    }

    // 2. Tree-tier gating check: points spent below higher-tier nodes must not drop below requirements
    for (const auto& other : m_nodes) {
        if (other.current_rank > 0 && other.points_required_in_tree > 0) {
            u32 points_below_tier = 0;
            for (const auto& cand : m_nodes) {
                if (cand.tier < other.tier) {
                    u32 r = cand.current_rank;
                    if (cand.id == node_id) {
                        r = new_rank;
                    }
                    points_below_tier += r * cand.cost_per_rank;
                }
            }
            if (points_below_tier < other.points_required_in_tree) {
                return false;
            }
        }
    }

    return true;
}

bool TalentTree::invest_point(const std::string& node_id) {
    if (!can_invest_point(node_id)) {
        return false;
    }

    TalentNode* node = get_node(node_id);
    node->current_rank++;
    m_available_points -= node->cost_per_rank;
    m_spent_points += node->cost_per_rank;
    return true;
}

bool TalentTree::refund_point(const std::string& node_id) {
    if (!can_refund_point(node_id)) {
        return false;
    }

    TalentNode* node = get_node(node_id);
    node->current_rank--;
    m_available_points += node->cost_per_rank;
    m_spent_points -= node->cost_per_rank;
    return true;
}

void TalentTree::reset_tree() {
    m_available_points += m_spent_points;
    m_spent_points = 0;
    for (auto& n : m_nodes) {
        n.current_rank = 0;
    }
}

std::vector<std::string> TalentTree::get_active_tags() const {
    std::vector<std::string> tags;
    for (const auto& n : m_nodes) {
        if (n.current_rank > 0 && !n.granted_tag.empty()) {
            tags.push_back(n.granted_tag);
        }
    }
    return tags;
}

std::vector<std::string> TalentTree::get_active_abilities() const {
    std::vector<std::string> abilities;
    for (const auto& n : m_nodes) {
        if (n.current_rank > 0 && !n.granted_ability.empty()) {
            abilities.push_back(n.granted_ability);
        }
    }
    return abilities;
}

f32 TalentTree::get_total_stat_modifier(const std::string& node_id) const {
    const TalentNode* node = get_node(node_id);
    if (node) {
        return node->total_stat_modifier();
    }
    return 0.0f;
}

std::string TalentTree::serialize() const {
    std::ostringstream ss;
    ss << "TREE " << m_id << " " << m_available_points << " " << m_spent_points << "\n";
    for (const auto& n : m_nodes) {
        if (n.current_rank > 0) {
            ss << "RANK " << n.id << " " << n.current_rank << "\n";
        }
    }
    ss << "END";
    return ss.str();
}

bool TalentTree::parse(const std::string& text) {
    std::istringstream ss(text);
    std::string tag;
    if (!(ss >> tag) || tag != "TREE") {
        return false;
    }

    std::string tree_id;
    u32 avail = 0;
    u32 spent = 0;
    if (!(ss >> tree_id >> avail >> spent)) {
        return false;
    }

    m_id = tree_id;
    m_available_points = avail;
    m_spent_points = spent;

    // Reset ranks
    for (auto& n : m_nodes) {
        n.current_rank = 0;
    }

    while (ss >> tag) {
        if (tag == "END") {
            break;
        }
        if (tag == "RANK") {
            std::string node_id;
            u32 rank = 0;
            if (ss >> node_id >> rank) {
                TalentNode* n = get_node(node_id);
                if (n) {
                    n->current_rank = std::min(rank, n->max_rank);
                }
            }
        }
    }

    return true;
}

} // namespace nf::gameplay
