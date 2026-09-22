#include <NF/AI/AIWorld.hpp>

#include <algorithm>
#include <cmath>

namespace nf::ai {
namespace {

/// The plain distance band for `d2`, ignoring both hysteresis and the sector
/// cap. This is what an actor *wants*; `plan()` decides what it gets.
SimTier band(float d2, float full_r2, float light_r2, float cheap_r2) {
    if (d2 <= full_r2) return SimTier::FullAI;
    if (d2 <= light_r2) return SimTier::LightweightAI;
    if (d2 <= cheap_r2) return SimTier::CheapMovement;
    return SimTier::RenderOnly;
}

/// Numeric level of a tier, for the "at best CheapMovement in an untouched
/// sector" clamp and for population counts.
u8 tier_level(SimTier t) { return static_cast<u8>(t); }

/// Integer clamp. `nf::clamp` is constrained to floating-point, and sector
/// indices are integers; defining this locally keeps the shared math header
/// out of a change that is only about grid bookkeeping.
constexpr i32 clamp_i(i32 x, i32 lo, i32 hi) { return x < lo ? lo : (x > hi ? hi : x); }

} // namespace

// ---------------------------------------------------------------------------
// Construction and registration
// ---------------------------------------------------------------------------

AIWorld::AIWorld(float sector_size, i32 sectors_x, i32 sectors_z, Vec3 origin)
    : m_sector_size(sector_size <= 0.0f ? 1.0f : sector_size)
    , m_sectors_x(sectors_x < 1 ? 1 : sectors_x)
    , m_sectors_z(sectors_z < 1 ? 1 : sectors_z)
    , m_origin(origin) {
    // A degenerate grid would be a crash at every bucket index, so it is
    // repaired here rather than guarded in the hot path. The repair is not
    // silent: sectors_x()/sector_size() report what was actually built.
    m_buckets.resize(static_cast<size_t>(m_sectors_x) * static_cast<size_t>(m_sectors_z));
    m_touched.resize(static_cast<size_t>(m_sectors_x) * static_cast<size_t>(m_sectors_z), u8{0});
}

u32 AIWorld::add_actor(AIActor actor) {
    actor.id = m_next_actor_id++;
    m_actors.push_back(actor);
    m_tier.push_back(SimTier::RenderOnly);
    m_grant.push_back(u8{0});
    return actor.id;
}

void AIWorld::remove_actor(u32 id) {
    for (size_t i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].id == id) {
            m_actors.erase(m_actors.begin() + static_cast<ptrdiff_t>(i));
            m_tier.erase(m_tier.begin() + static_cast<ptrdiff_t>(i));
            m_grant.erase(m_grant.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

void AIWorld::set_actor_position(u32 id, Vec3 position) {
    for (AIActor& a : m_actors) {
        if (a.id == id) {
            a.position = position;
            return;
        }
    }
}

void AIWorld::clear() {
    m_actors.clear();
    m_foci.clear();
    m_tier.clear();
    m_grant.clear();
    m_candidates.clear();
    m_candidate_d2.clear();
    m_order.clear();
    m_full_ai_acc = 0.0f;
    m_lightweight_acc = 0.0f;
    m_last_full_grants = 0;
    m_last_light_grants = 0;
    m_next_actor_id = 1;
    m_next_focus_id = 1;
    m_plan_count = 0;
    m_tier_counts = {};
    for (std::vector<u32>& b : m_buckets) b.clear();
}

u32 AIWorld::add_focus(AIFocus focus) {
    focus.id = m_next_focus_id++;
    m_foci.push_back(focus);
    return focus.id;
}

void AIWorld::remove_focus(u32 id) {
    for (size_t i = 0; i < m_foci.size(); ++i) {
        if (m_foci[i].id == id) {
            m_foci.erase(m_foci.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

void AIWorld::set_focus_position(u32 id, Vec3 position) {
    for (AIFocus& f : m_foci) {
        if (f.id == id) {
            f.position = position;
            return;
        }
    }
}

void AIWorld::clear_focuses() { m_foci.clear(); }

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void AIWorld::set_budget(float full_ai_per_second, float lightweight_per_second) {
    // A negative rate would run the accumulator backwards and starve the world
    // forever after one bad frame; clamp at zero, which is the documented
    // "tier disabled" value anyway.
    m_full_ai_per_second = full_ai_per_second < 0.0f ? 0.0f : full_ai_per_second;
    m_lightweight_per_second = lightweight_per_second < 0.0f ? 0.0f : lightweight_per_second;
}

void AIWorld::set_tier_radii(float full_ai, float lightweight, float cheap) {
    // The bands nest: full <= light <= cheap. Anything else would leave a hole
    // where an actor is neither one tier nor the next, so the values are sorted
    // here rather than trusted at every use site. Negatives clamp to zero
    // first, which is the documented "tier disabled" value anyway.
    float r[3] = {full_ai < 0.0f ? 0.0f : full_ai, lightweight < 0.0f ? 0.0f : lightweight,
                  cheap < 0.0f ? 0.0f : cheap};
    // 3-element sort network, ascending.
    if (r[0] > r[1]) { const float t = r[0]; r[0] = r[1]; r[1] = t; }
    if (r[1] > r[2]) { const float t = r[1]; r[1] = r[2]; r[2] = t; }
    if (r[0] > r[1]) { const float t = r[0]; r[0] = r[1]; r[1] = t; }
    m_full_radius = r[0];
    m_light_radius = r[1];
    m_cheap_radius = r[2];
}

void AIWorld::set_tier_hysteresis(float fraction) {
    m_hysteresis = fraction < 0.0f ? 0.0f : fraction;
}

// ---------------------------------------------------------------------------
// Spatial queries
// ---------------------------------------------------------------------------

i32 AIWorld::sector_index(Vec3 world) const {
    // floor, not truncation: a position behind the origin belongs to the
    // sector at -1, which the clamp then folds onto the edge.
    const float fx = (world.x - m_origin.x) / m_sector_size;
    const float fz = (world.z - m_origin.z) / m_sector_size;
    i32 cx = static_cast<i32>(std::floor(fx));
    i32 cz = static_cast<i32>(std::floor(fz));
    cx = clamp_i(cx, 0, m_sectors_x - 1);
    cz = clamp_i(cz, 0, m_sectors_z - 1);
    return cz * m_sectors_x + cx;
}

bool AIWorld::sector_touched(i32 sector) const {
    if (sector < 0 || sector >= static_cast<i32>(m_touched.size())) return false;
    return m_touched[static_cast<size_t>(sector)] != 0;
}

float AIWorld::demote_edge(SimTier t) const {
    const float widen = 1.0f + m_hysteresis;
    switch (t) {
        case SimTier::FullAI: return m_full_radius * widen;
        case SimTier::LightweightAI: return m_light_radius * widen;
        case SimTier::CheapMovement: return m_cheap_radius * widen;
        default: return 0.0f;
    }
}

/// Hysteresis: promotion uses the plain band, demotion requires clearing the
/// widened edge of the tier the actor currently holds. Without this an actor
/// standing on a band boundary flips tier every plan, which is the pop-in a
/// player notices even when nothing else is wrong.
SimTier AIWorld::hysteresis_tier(SimTier current, SimTier want, float d2) const {
    if (tier_level(want) >= tier_level(current)) return want;
    const float edge = demote_edge(current);
    return (d2 <= edge * edge) ? current : want;
}

float AIWorld::nearest_focus_d2(Vec3 world) const {
    // No foci: nothing is near anything. The sentinel keeps the caller a
    // straight min() with no branch, and every band comparison against it is
    // false, so the whole world lands in RenderOnly — the quiet world the
    // header documents. A focus with a non-positive radius reaches nothing
    // (the touch pass already skips it), so it is inert here too rather than
    // promoting actors by raw distance to a point that demands nothing.
    float best = 1e30f;
    for (const AIFocus& f : m_foci) {
        if (f.radius <= 0.0f) continue;
        const Vec3 d = f.position - world;
        const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
        if (d2 < best) best = d2;
    }
    return best;
}

bool AIWorld::candidate_less(const AIActor& a, const AIActor& b, float a_d2, float b_d2) {
    if (a.priority != b.priority) return a.priority > b.priority;
    if (a_d2 != b_d2) return a_d2 < b_d2;
    return a.id < b.id;
}

// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------

void AIWorld::plan(float dt) {
    if (dt < 0.0f) dt = 0.0f;
    ++m_plan_count;

    // The previous tiers are hysteresis state, so the vectors grow rather than
    // reset: a newly registered actor starts RenderOnly, an existing one keeps
    // whatever it earned last plan until a band says otherwise.
    m_tier.resize(m_actors.size(), SimTier::RenderOnly);
    m_grant.assign(m_actors.size(), u8{0});
    m_tier_counts = {};

    const i32 sector_count = m_sectors_x * m_sectors_z;
    if (sector_count <= 0 || m_actors.empty()) {
        m_last_full_grants = 0;
        m_last_light_grants = 0;
        return;
    }

    // Pass 1 — which sectors a focus reaches. Box overlap in sector space, so a
    // focus sitting exactly on a grid line touches the sectors on both sides.
    m_touched.assign(static_cast<size_t>(sector_count), u8{0});
    for (const AIFocus& f : m_foci) {
        if (f.radius <= 0.0f) continue;
        const float x_lo = f.position.x - f.radius;
        const float x_hi = f.position.x + f.radius;
        const float z_lo = f.position.z - f.radius;
        const float z_hi = f.position.z + f.radius;
        const float inv = 1.0f / m_sector_size;
        i32 x0 = clamp_i(static_cast<i32>(std::floor((x_lo - m_origin.x) * inv)), 0, m_sectors_x - 1);
        i32 x1 = clamp_i(static_cast<i32>(std::floor((x_hi - m_origin.x) * inv)), 0, m_sectors_x - 1);
        i32 z0 = clamp_i(static_cast<i32>(std::floor((z_lo - m_origin.z) * inv)), 0, m_sectors_z - 1);
        i32 z1 = clamp_i(static_cast<i32>(std::floor((z_hi - m_origin.z) * inv)), 0, m_sectors_z - 1);
        for (i32 cz = z0; cz <= z1; ++cz) {
            for (i32 cx = x0; cx <= x1; ++cx) {
                m_touched[static_cast<size_t>(cz * m_sectors_x + cx)] = 1;
            }
        }
    }

    // Pass 2 — bucket the actors by sector, clearing only, so a steady world
    // never reallocates.
    for (std::vector<u32>& b : m_buckets) b.clear();
    for (size_t i = 0; i < m_actors.size(); ++i) {
        m_buckets[static_cast<size_t>(sector_index(m_actors[i].position))].push_back(static_cast<u32>(i));
    }

    const float full_r2 = m_full_radius * m_full_radius;
    const float light_r2 = m_light_radius * m_light_radius;
    const float cheap_r2 = m_cheap_radius * m_cheap_radius;

    // Pass 3 — walk sectors in index order. Untouched ones are settled here and
    // never enter the candidate list; that is the whole point of the grid.
    m_candidates.clear();
    m_candidate_d2.clear();
    for (i32 s = 0; s < sector_count; ++s) {
        const std::vector<u32>& bucket = m_buckets[static_cast<size_t>(s)];
        if (m_touched[static_cast<size_t>(s)] == 0) {
            for (const u32 i : bucket) {
                if (m_actors[i].pinned_full) {
                    // Scripted intelligence is outside the grid as well as
                    // outside the budget: the grid is an optimisation, and an
                    // authored encounter star is not throttled by it any more
                    // than by a rate.
                    m_tier[i] = SimTier::FullAI;
                    m_grant[i] = 1;
                    continue;
                }
                const float d2 = nearest_focus_d2(m_actors[i].position);
                const SimTier want = band(d2, full_r2, light_r2, cheap_r2);
                // Hysteresis applies even in a quiet sector: an actor demoted
                // out of FullAI keeps its band until it clears the widened edge.
                SimTier t = hysteresis_tier(m_tier[i], want, d2);
                m_tier[i] = static_cast<SimTier>(tier_level(t) < tier_level(SimTier::CheapMovement)
                                                     ? t
                                                     : SimTier::CheapMovement);
                // CheapMovement is a real simulation level (§47 Level 1), so it
                // earns a grant — the grid's promise is only that an untouched
                // sector is never billed against the *budget*, and steering is
                // not billed.
                m_grant[i] = tier_level(m_tier[i]) >= tier_level(SimTier::CheapMovement) ? u8{1} : u8{0};
            }
            continue;
        }
        for (const u32 i : bucket) {
            m_candidates.push_back(i);
            m_candidate_d2.push_back(nearest_focus_d2(m_actors[i].position));
        }
    }

    // Pass 4 — the budget. Rates accrue over dt; the integer part is spent this
    // plan and the fraction carries, so 2.5/plane yields 2, 3, 2, 3... The
    // grants are counted after the fact rather than pre-decremented, so a
    // report can always be reconciled against what was actually spent.
    m_full_ai_acc += m_full_ai_per_second * dt;
    m_lightweight_acc += m_lightweight_per_second * dt;

    // Clamp in float space, before the cast. Two reasons: a stall that handed
    // in a huge dt would otherwise bank more slots than the world can ever
    // spend and stay permanently maxed; and converting a float outside u32
    // range is undefined behaviour, not merely a wrong count. The clamp caps
    // the accumulator itself, so unspent excess is discarded rather than
    // carried — a stall does not buy full intelligence for every later plan.
    const float max_slots = static_cast<float>(m_candidates.size());
    if (m_full_ai_acc > max_slots) m_full_ai_acc = max_slots;
    if (m_lightweight_acc > max_slots) m_lightweight_acc = max_slots;

    u32 full_slots = static_cast<u32>(m_full_ai_acc);
    u32 light_slots = static_cast<u32>(m_lightweight_acc);
    m_full_ai_acc -= static_cast<float>(full_slots);
    m_lightweight_acc -= static_cast<float>(light_slots);

    // Sort indices by the total comparator. std::sort is not stable, which is
    // fine and is why the comparator ends in the unique id.
    m_order.resize(m_candidates.size());
    for (size_t k = 0; k < m_order.size(); ++k) m_order[k] = k;
    std::sort(m_order.begin(), m_order.end(), [&](size_t a, size_t b) {
        const u32 ia = m_candidates[a];
        const u32 ib = m_candidates[b];
        return candidate_less(m_actors[ia], m_actors[ib], m_candidate_d2[a], m_candidate_d2[b]);
    });

    u32 full_grants = 0;
    u32 light_grants = 0;
    for (const size_t k : m_order) {
        const u32 i = m_candidates[k];
        const AIActor& actor = m_actors[i];
        const float d2 = m_candidate_d2[k];

        SimTier want = hysteresis_tier(m_tier[i], band(d2, full_r2, light_r2, cheap_r2), d2);

        if (actor.pinned_full) {
            // Scripted intelligence is outside the budget and does not consume
            // a slot; the budget caps the world, not the encounter.
            m_tier[i] = SimTier::FullAI;
            m_grant[i] = 1;
            continue;
        }

        if (want == SimTier::FullAI && full_slots > 0) {
            m_tier[i] = SimTier::FullAI;
            m_grant[i] = 1;
            --full_slots;
            ++full_grants;
        } else if (tier_level(want) >= tier_level(SimTier::LightweightAI) && light_slots > 0) {
            // Either the actor wanted LightweightAI, or it wanted FullAI and the
            // full budget was spent — in both cases it gets the cheaper tier
            // rather than nothing, because near is more important than thorough.
            m_tier[i] = SimTier::LightweightAI;
            m_grant[i] = 1;
            --light_slots;
            ++light_grants;
        } else {
            // No slot this plan. An actor that wanted a mind and did not get one
            // falls to CheapMovement, so it still steers — steering is never
            // billed — but perception and the behaviour tree do not run. An
            // actor that wanted CheapMovement or below never needed a slot and
            // is left where the band put it.
            m_tier[i] = tier_level(want) < tier_level(SimTier::CheapMovement)
                             ? want
                             : SimTier::CheapMovement;
            m_grant[i] = tier_level(m_tier[i]) >= tier_level(SimTier::CheapMovement) ? u8{1} : u8{0};
        }
    }

    m_last_full_grants = full_grants;
    m_last_light_grants = light_grants;

    for (size_t i = 0; i < m_actors.size(); ++i) {
        ++m_tier_counts[tier_level(m_tier[i])];
    }
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

SimTier AIWorld::tier_of(u32 id) const {
    for (size_t i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].id == id) return m_tier[i];
    }
    return SimTier::RenderOnly;
}

bool AIWorld::wants_tick(u32 id) const {
    for (size_t i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].id == id) return m_grant[i] != 0;
    }
    return false;
}

} // namespace nf::ai
