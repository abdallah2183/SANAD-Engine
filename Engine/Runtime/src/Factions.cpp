#include <NF/Runtime/Factions.hpp>

namespace nf::runtime {

const char* team_relation_name(TeamRelation relation) noexcept {
    switch (relation) {
    case TeamRelation::Neutral: return "neutral";
    case TeamRelation::Friendly: return "friendly";
    case TeamRelation::Hostile: return "hostile";
    }
    return "neutral";
}

TeamRelation team_relation(std::string_view a, std::string_view b) noexcept {
    if (a.empty() || b.empty()) {
        return TeamRelation::Neutral;
    }
    return a == b ? TeamRelation::Friendly : TeamRelation::Hostile;
}

bool team_damage_allowed(TeamIdentity attacker, TeamIdentity victim) noexcept {
    switch (team_relation(attacker.team, victim.team)) {
    case TeamRelation::Neutral:
    case TeamRelation::Hostile:
        return true;
    case TeamRelation::Friendly:
        return attacker.friendly_fire;
    }
    return true;
}

} // namespace nf::runtime
