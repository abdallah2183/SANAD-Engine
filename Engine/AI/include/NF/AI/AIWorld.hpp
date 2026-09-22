#pragma once

// NF/AI/AIWorld.hpp — world-scale AI: sectors, simulation tiers and a budget
// (design doc Section 46 "AI World" and Section 47 "Crowd System").
//
// The modules beside this one answer "what does one NPC decide". This one
// answers the question that makes an open world feasible at all: *which* NPCs
// decide, and how often. Running perception, a behavior tree and utility
// scoring for every actor every frame is not a budget; it is a frame-rate
// target. Section 46 is explicit: "لا يجب تشغيل كل NPC بنفس مستوى الذكاء
// دائمًا" — not every NPC runs at the same level of intelligence all the time.
//
// The model has two independent axes, and conflating them is the usual mistake:
//
//   Tier — *what* runs for an actor. Section 47's four levels:
//     RenderOnly     drawn, simulated not at all (Level 0)
//     CheapMovement  steer toward a waypoint; no perception, no decisions (1)
//     LightweightAI  state machine, no perception, a fraction of frames (2)
//     FullAI         perception + behavior tree, every granted frame (3)
//
//   Grant — *whether it runs at all this plan*, which for a steering actor is
//     still yes. The two axes come apart in exactly one place: an actor that the
//     bands put at FullAI but the budget had no slot for is *recorded* at
//     CheapMovement, because that is what it actually does this plan — it
//     steers. `tier_of` is therefore the single answer a game needs for "what do
//     I run", and `wants_tick` is the cheap pre-filter for "do I bother".
//
// How a tier is chosen
// --------------------
// Actors live on a uniform sector grid. A *focus* is a world position that
// demands full intelligence nearby — the player, a co-op partner, or a noise
// loud enough that Perception put an event on the world. Each focus has a
// radius; a sector touched by any radius is a candidate sector, and only actors
// standing in a candidate sector are even considered for LightweightAI or
// above. An actor in an untouched sector is CheapMovement at best, whatever its
// distance happens to be, because the point of the grid is to never iterate
// the world's quiet half.
//
// Within the candidate set the tier is a distance band from the nearest focus,
// with hysteresis: upgrading needs `d <= r`, downgrading needs
// `d > r * (1 + hysteresis)`. Without the band an actor jittering on the FullAI
// radius flips tier every frame, which reads to the player as pop-in.
//
// The budget
// ----------
// Rates, not per-frame counts: `full_ai_per_second` accrues over `dt`, the
// integer part is spent this plan and the fraction carries forward, so 2.5
// granted per second yields 2, 3, 2, 3... rather than a steady 2 with a lost
// half every frame. Candidates are ordered by priority, then nearness, then
// id; the front of the list spends the budget.
//
// Determinism
// -----------
// Same contract as Perception, StateMachine and UtilityAI:
//   - actors, focuses and sectors are all visited in insertion or index order,
//     never pointer or hash order;
//   - the candidate ordering ends in the actor id, which is unique, so the
//     comparator is a total order and *any* sort is reproducible — stability
//     is not needed and not relied on;
//   - ids strictly increase and are never recycled, so a tier recorded against
//     id 4 always means the fifth actor added;
//   - no RNG and no wall clock. `dt` is the only time input, clamped
//     non-negative.
//
// Units: distances are world metres, rates are actors per second, dt is
// seconds.

#include <NF/Core/Math.hpp>
#include <NF/Core/Types.hpp>

#include <array>
#include <vector>

namespace nf::ai {

/// Section 47's four simulation levels. Kept as an enum rather than flags
/// because an actor is on exactly one level; "which level" is a single answer
/// the game asks for, exactly as `StimulusKind` is in Perception.
enum class SimTier : u8 {
    RenderOnly    = 0, // Level 0: drawn only
    CheapMovement = 1, // Level 1: steer, no decisions
    LightweightAI = 2, // Level 2: decisions, no perception
    FullAI        = 3, // Level 3: the whole stack
};

/// One AI actor in the world. This is the authored input — position and tuning
/// — and deliberately carries no derived state; the tier and grant computed
/// each plan are stored privately, indexed the same way, so an actor read back
/// from `actors()` is exactly what was put in.
struct AIActor {
    u32 id = 0;
    Vec3 position{};

    /// Budget tie-break: higher priority spends the budget first. A boss, a
    /// quest giver, anything the camera is looking at.
    u32 priority = 0;

    /// Scripted full intelligence regardless of distance or budget. A pinned
    /// actor is outside the budget entirely — the budget exists to cap *the
    /// world's* actors, and the encounter's stars are not the world's problem.
    bool pinned_full = false;
};

/// A position that demands intelligence around it. The player is one. A second
/// is a co-op partner. A third is the rifle shot the Perception module just
/// put on the world: it should promote the guards near it for a few seconds,
/// which is what a focus is for and why focuses can come and go.
struct AIFocus {
    u32 id = 0;
    Vec3 position{};
    float radius = 30.0f;
};

class AIWorld {
public:
    /// `sector_size` is the grid's cell in metres; `sectors_x`/`sectors_z` are
    /// the grid's extents. The grid is fixed at construction because resizing
    /// it would rehash every actor's sector mid-run.
    AIWorld(float sector_size, i32 sectors_x, i32 sectors_z, Vec3 origin = Vec3{});

    /// Registers an actor and returns the id it was given. Ids strictly
    /// increase and are never recycled — the same rule as PerceptionScene, for
    /// the same reason: a recycled id would let a tier recorded against a dead
    /// actor be read back as though a new one had earned it.
    u32 add_actor(AIActor actor);

    /// Removes the actor with this id, if present. Removal shifts later actors
    /// down, which changes iteration order; that is fine, order only has to be
    /// fixed between edits, and a removal is a game decision, not a per-plan
    /// occurrence.
    void remove_actor(u32 id);

    /// Moves an actor. This is the only per-frame mutation — the tier is
    /// recomputed from it on the next plan, so the game never sets a tier by
    /// hand. Unknown id is ignored.
    void set_actor_position(u32 id, Vec3 position);

    void clear();

    /// Adds a focus. Returns its id; ids strictly increase and are never
    /// recycled. An empty focus list means the world is quiet: every actor
    /// settles to CheapMovement or below.
    u32 add_focus(AIFocus focus);
    void remove_focus(u32 id);
    void set_focus_position(u32 id, Vec3 position);
    void clear_focuses();

    /// Simulation rates, in actors per second. Zero disables that tier
    /// outright: `full_ai_per_second = 0` makes the whole world cheap, which is
    /// what a cutscene wants.
    void set_budget(float full_ai_per_second, float lightweight_per_second);

    /// Distance bands from the nearest focus, in metres. `cheap` is the outer
    /// edge of Level 1; beyond it an actor is rendered only.
    void set_tier_radii(float full_ai, float lightweight, float cheap);

    /// Fraction of a radius an actor must exceed to be *demoted* from a tier.
    /// Upgrading happens at the radius itself, so the band is asymmetric: an
    /// actor on the edge is promoted eagerly and kept generously.
    void set_tier_hysteresis(float fraction);

    /// Evaluates every actor's tier and grant for this plan. Reads only the
    /// actors, focuses and config; it never mutates them. Negative dt clamps
    /// to zero, which freezes the budget accumulator rather than running it
    /// backwards.
    void plan(float dt);

    /// The tier an actor currently simulates at, or RenderOnly for an unknown
    /// id. Before the first plan every actor is RenderOnly, so a game that
    /// forgot to plan degrades to "the world stands still" rather than to full
    /// intelligence by accident.
    SimTier tier_of(u32 id) const;

    /// Whether the actor should simulate at all this plan. CheapMovement counts
    /// — steering is a real level (§47 Level 1) and is never billed — so this is
    /// true for every tier above RenderOnly, including an actor that wanted
    /// FullAI, found no slot, and fell back. Read it as "does this actor do
    /// anything", and read `tier_of` for how much.
    bool wants_tick(u32 id) const;

    /// Actors in insertion order, exactly as registered, for editors and
    /// debuggers.
    const std::vector<AIActor>& actors() const { return m_actors; }
    const std::vector<AIFocus>& focuses() const { return m_foci; }

    /// Population per tier as of the most recent plan. A replay asserts these
    /// alongside every actor's grant: same *distribution* would hide a change
    /// in *who* runs, and who runs is the whole contract.
    std::array<u32, 4> tier_counts() const { return m_tier_counts; }

    /// Slots the budget actually spent last plan, and the fraction it is
    /// carrying into the next. A debugger graphs the carry to show whether the
    /// rate is landing where the designer asked.
    u32 last_full_grants() const { return m_last_full_grants; }
    u32 last_light_grants() const { return m_last_light_grants; }

    i32 sectors_x() const { return m_sectors_x; }
    i32 sectors_z() const { return m_sectors_z; }
    float sector_size() const { return m_sector_size; }

private:
    /// Sector index for a world position, clamped into bounds. An actor outside
    /// the grid is folded onto its edge rather than dropped: the tier it earns
    /// there is still correct, and dropping it would make an off-world actor
    /// invisible to the budget forever.
    i32 sector_index(Vec3 world) const;

    /// True if `sector` intersects any focus radius. The check is a box overlap
    /// in sector space, so a focus on a sector boundary touches all four.
    bool sector_touched(i32 sector) const;

    /// Outer edge of `t`'s distance band, widened by the hysteresis fraction.
    /// This is the line an actor must be *beyond* to lose the tier — the plain
    /// radius is the line it must be *within* to gain it, and the difference is
    /// the whole anti-pop-in mechanism.
    float demote_edge(SimTier t) const;

    /// Applies hysteresis to a plain band result: `want` wins outright when it
    /// is not a demotion, otherwise the actor keeps `current` until it is past
    /// `demote_edge(current)`. Centralised because both the touched and the
    /// untouched sector paths demote by the same rule.
    SimTier hysteresis_tier(SimTier current, SimTier want, float d2) const;

    /// Squared distance to the nearest focus, or a large sentinel when there
    /// are no foci — no foci means no candidate is near anything, and the
    /// sentinel keeps that case out of the caller as a branch.
    float nearest_focus_d2(Vec3 world) const;

    /// Total comparator: priority descending, then nearness ascending, then id
    /// ascending. The id is unique, so the ordering is total and no sort
    /// relying on it needs to be stable.
    static bool candidate_less(const AIActor& a, const AIActor& b, float a_d2, float b_d2);

    std::vector<AIActor> m_actors;
    std::vector<AIFocus> m_foci;

    /// Derived state, parallel to m_actors. Kept private so the authored struct
    /// cannot fall out of step with the computed one.
    std::vector<SimTier> m_tier;
    std::vector<u8> m_grant;

    /// Candidate actor indices per plan. Members rather than scratch locals
    /// because they are the hot path's only allocations, and reusing the
    /// capacity keeps a quiet world from churning memory every frame.
    std::vector<u32> m_candidates;
    std::vector<float> m_candidate_d2;
    std::vector<size_t> m_order;
    std::vector<std::vector<u32>> m_buckets;
    std::vector<u8> m_touched;

    float m_full_ai_per_second = 20.0f;
    float m_lightweight_per_second = 60.0f;
    float m_full_ai_acc = 0.0f;
    float m_lightweight_acc = 0.0f;
    u32 m_last_full_grants = 0;
    u32 m_last_light_grants = 0;
    u32 m_next_actor_id = 1;
    u32 m_next_focus_id = 1;
    u32 m_plan_count = 0;

    float m_full_radius = 20.0f;
    float m_light_radius = 50.0f;
    float m_cheap_radius = 120.0f;
    float m_hysteresis = 0.25f;

    float m_sector_size = 32.0f;
    i32 m_sectors_x = 0;
    i32 m_sectors_z = 0;
    Vec3 m_origin;

    std::array<u32, 4> m_tier_counts{};
};

} // namespace nf::ai
