#pragma once

// NF/Runtime/Factions.hpp — who is allowed to hurt whom, as arithmetic.
//
// Phase 45 gave an NPC a target name and a trigger. It deliberately did not
// invent a social model: "who is hostile" is a game's decision, and a scene
// format that guessed it would be wrong for every game that disagreed. The
// cost of that restraint was visible in play: two guards on the same side would
// happily shoot through each other to reach the player, because the only rule
// the engine knew was "the ray hit a box".
//
// This is the smallest social model that fixes that without taking the decision
// away from the level:
//
//   * an entity may carry a team name (`Faction: team=Blue`);
//   * no team, or an empty one, is NEUTRAL — the pre-Phase-46 behaviour, so a
//     scene that never writes a Faction line keeps every existing hit;
//   * two equal, non-empty teams are FRIENDLY, and friendly damage is blocked
//     unless the ATTACKER opts in with `friendly_fire=true`;
//   * two different non-empty teams are HOSTILE.
//
// The rules are pure string comparisons, not ECS lookups, so a test can pin
// every corner without building a world. The runtime is responsible for
// fetching the components and handing their identities here.

#include <NF/Core/Types.hpp>

#include <string_view>

namespace nf::runtime {

/// The relationship between two team names.
enum class TeamRelation : u8 {
    /// At least one side has no team. Everyone may hurt everyone, which is the
    /// behaviour every scene authored before this phase already had.
    Neutral = 0,
    /// The same non-empty team.
    Friendly = 1,
    /// Two different non-empty teams.
    Hostile = 2,
};

/// Stable technical name for logs, tests and debug overlays.
[[nodiscard]] const char* team_relation_name(TeamRelation relation) noexcept;

/// The relationship between two authored team names.
///
/// Empty names are neutral, including the case where both are empty: two
/// anonymous entities are not secretly teammates.
[[nodiscard]] TeamRelation team_relation(std::string_view a, std::string_view b) noexcept;

/// The part of an entity that matters to a damage decision.
///
/// `friendly_fire` belongs to the ATTACKER: it is a licence the shooter's
/// author grants, not a property the victim can claim. A victim cannot make
/// itself shootable by its own flag.
struct TeamIdentity {
    std::string_view team;
    bool friendly_fire = false;
};

/// Whether an attack from `attacker` may damage `victim`.
///
/// Neutral and hostile pairs are allowed. A friendly pair is blocked unless
/// the attacker authored `friendly_fire=true` — a deliberate opt-in for a
/// betrayal script or a training dummy, not a default.
[[nodiscard]] bool team_damage_allowed(TeamIdentity attacker, TeamIdentity victim) noexcept;

} // namespace nf::runtime
