#pragma once

// NF/Gameplay/Quest.hpp — quest log with staged objectives (design doc 216).
//
// A quest is an ordered stage list; each stage has named objectives with a
// target count (kill 5 wolves, collect 3 herbs). The log tracks progress,
// advances stages when all objectives complete, and records terminal states.
// Text encode/decode round-trips the whole log for saves (GameplayModuleComponent
// properties carry the lines; see serialize()/parse()).

#include <NF/Core/Types.hpp>

#include <string>
#include <vector>

namespace nf::gameplay {

enum class QuestState : u8 {
    Inactive = 0, // defined but not started
    Active = 1,
    Completed = 2,
    Failed = 3,
};

struct QuestObjective {
    std::string id;
    std::string label;
    u32 target = 1;
    u32 progress = 0;

    bool complete() const { return progress >= target; }
};

struct QuestStage {
    std::string id;
    std::string label;
    std::vector<QuestObjective> objectives;

    bool complete() const;
};

struct Quest {
    std::string id;
    std::string title;
    QuestState state = QuestState::Inactive;
    usize stage_index = 0; // into stages; == stages.size() when completed
    std::vector<QuestStage> stages;

    const QuestStage* current_stage() const;
    bool complete() const { return state == QuestState::Completed; }
};

class QuestLog {
public:
    /// Adds a quest definition (Inactive). Returns false when id is empty
    /// or already present — definitions never silently replace.
    bool define(Quest quest);
    bool start(const std::string& quest_id);
    bool fail(const std::string& quest_id);

    /// Adds progress to one objective of the quest's current stage.
    /// Completing every objective advances the stage; finishing the last
    /// stage completes the quest. Returns false when nothing matched
    /// (unknown quest/objective, quest not active, amount 0).
    bool advance(const std::string& quest_id, const std::string& objective_id,
                 u32 amount = 1);

    const Quest* find(const std::string& quest_id) const;
    usize quest_count() const { return m_quests.size(); }
    usize active_count() const;
    usize completed_count() const;

    /// Deterministic text form (one quest per line). parse() restores it;
    /// malformed lines are skipped, never fatal.
    std::string serialize() const;
    bool parse(const std::string& text);

private:
    Quest* find_mut(const std::string& quest_id);
    std::vector<Quest> m_quests; // definition order (deterministic)
};

} // namespace nf::gameplay
