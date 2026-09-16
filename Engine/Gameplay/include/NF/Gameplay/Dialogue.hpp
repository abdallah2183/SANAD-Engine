#pragma once

// NF/Gameplay/Dialogue.hpp — data-driven dialogue trees (design doc 215).
//
// Nodes carry speaker + text with either a linear continuation or a choice
// list; choices gate on gameplay tags (requires_tag) and grant tags when
// picked (sets_tag), so dialogue plugs straight into quests and TagQueries.
// Text encode/decode round-trips for saves; validation names every dangling
// reference instead of crashing on it.

#include <NF/Core/Types.hpp>
#include <NF/Gameplay/Tags.hpp>

#include <string>
#include <vector>

namespace nf::gameplay {

struct DialogueChoice {
    std::string id;
    std::string text;
    std::string next_node; // empty = the conversation ends here
    std::string requires_tag; // empty = always available
    std::string sets_tag; // granted when picked (empty = none)
};

struct DialogueNode {
    std::string id;
    std::string speaker;
    std::string text;
    std::string next; // linear continuation (used when choices is empty)
    std::vector<DialogueChoice> choices;
};

class DialogueTree {
public:
    /// Adds a node; false when id is empty or duplicated (never replaces).
    bool add_node(DialogueNode node);
    const DialogueNode* find(const std::string& id) const;
    usize node_count() const { return m_nodes.size(); }

    /// Checks start-node presence and that every next/choice target exists.
    /// Cycles are legal (hub nodes); dangling references are not.
    bool validate(const std::string& start_node, std::string& out_error) const;

    std::string serialize() const;
    bool parse(const std::string& text);

private:
    std::vector<DialogueNode> m_nodes; // definition order (deterministic)
};

class DialogueRunner {
public:
    /// Begins at start_node; false when the node is missing (stays ended).
    bool start(const DialogueTree& tree, const std::string& start_node);
    bool ended() const { return m_current == nullptr; }
    const DialogueNode* current() const { return m_current; }

    /// Choices whose requires_tag is empty or held in `tags`.
    std::vector<const DialogueChoice*> available_choices(const TagContainer& tags) const;

    /// Picks a choice by id (must be currently available): grants sets_tag,
    /// advances to next_node (or ends). False when invalid/ended.
    bool choose(const std::string& choice_id, TagContainer& tags);

    /// Advances a choiceless node along next (or ends). False when the node
    /// has choices, has no next, or the run ended.
    bool advance();

private:
    const DialogueTree* m_tree = nullptr;
    const DialogueNode* m_current = nullptr;
};

} // namespace nf::gameplay
