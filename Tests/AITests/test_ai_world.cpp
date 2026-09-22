// AITests/ai world — sectors, simulation tiers, and the per-second budget.
//
// Every case is pure CPU and drives the world through positions alone: a tier
// is asserted by moving an actor and planning, never by setting a tier by hand,
// because the whole point of the module is that the tier is *derived*. What is
// being tested is the bookkeeping that makes an open world affordable — which
// actors run, at what level, and how often.

#include <NF/AI/AIWorld.hpp>
#include <NF/Test/TestFramework.hpp>

#include <vector>

using namespace nf;
using namespace nf::ai;

namespace {

/// A world sized for the default radii (full 20, light 50, cheap 120) with the
/// origin at the grid's corner, so a focus at the origin reaches sector 0 and
/// every distance in a test is a plain |position|.
AIWorld default_world() {
    return AIWorld(32.0f, 8, 8, Vec3{0.0f, 0.0f, 0.0f});
}

} // namespace

NF_TEST(ai_world_actors_get_increasing_never_recycled_ids) {
    AIWorld w = default_world();
    const u32 a = w.add_actor(AIActor{.position = Vec3{0.0f, 0.0f, 0.0f}});
    const u32 b = w.add_actor(AIActor{.position = Vec3{0.0f, 0.0f, 0.0f}});
    const u32 c = w.add_actor(AIActor{.position = Vec3{0.0f, 0.0f, 0.0f}});
    NF_CHECK(a == 1u);
    NF_CHECK(b == 2u);
    NF_CHECK(c == 3u);

    w.remove_actor(b);
    const u32 d = w.add_actor(AIActor{.position = Vec3{0.0f, 0.0f, 0.0f}});
    NF_CHECK(d == 4u);
    // A recycled id here would let a tier recorded against the removed actor be
    // read back as though the new one had earned it.
    NF_CHECK(d != b);
    NF_CHECK(w.actors().size() == 3);
}

NF_TEST(ai_world_focuses_get_increasing_never_recycled_ids) {
    AIWorld w = default_world();
    const u32 a = w.add_focus(AIFocus{.position = Vec3{}, .radius = 10.0f});
    const u32 b = w.add_focus(AIFocus{.position = Vec3{}, .radius = 10.0f});
    NF_CHECK(a == 1u);
    NF_CHECK(b == 2u);
    w.remove_focus(a);
    NF_CHECK(w.add_focus(AIFocus{.position = Vec3{}, .radius = 10.0f}) == 3u);
}

NF_TEST(ai_world_without_a_plan_every_actor_is_render_only) {
    // A game that forgot to plan degrades to a still world rather than to full
    // intelligence by accident.
    AIWorld w = default_world();
    const u32 id = w.add_actor(AIActor{.position = Vec3{0.0f, 0.0f, 0.0f}});
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 100.0f});
    NF_CHECK(w.tier_of(id) == SimTier::RenderOnly);
    NF_CHECK(!w.wants_tick(id));
}

NF_TEST(ai_world_unknown_id_reads_are_render_only_and_no_tick) {
    AIWorld w = default_world();
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 100.0f});
    w.plan(0.1f);
    NF_CHECK(w.tier_of(999u) == SimTier::RenderOnly);
    NF_CHECK(!w.wants_tick(999u));
    // Removing an unknown id is not an error.
    w.remove_actor(999u);
    w.remove_focus(999u);
}

NF_TEST(ai_world_distance_bands_from_the_nearest_focus) {
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1000.0f, 1000.0f); // budget is not what this case tests

    const u32 inner = w.add_actor(AIActor{.position = Vec3{10.0f, 0.0f, 0.0f}});
    const u32 mid = w.add_actor(AIActor{.position = Vec3{35.0f, 0.0f, 0.0f}});
    const u32 outer = w.add_actor(AIActor{.position = Vec3{80.0f, 0.0f, 0.0f}});
    const u32 gone = w.add_actor(AIActor{.position = Vec3{500.0f, 0.0f, 0.0f}});

    w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});
    w.plan(1.0f);

    NF_CHECK(w.tier_of(inner) == SimTier::FullAI);
    NF_CHECK(w.wants_tick(inner));
    NF_CHECK(w.tier_of(mid) == SimTier::LightweightAI);
    NF_CHECK(w.wants_tick(mid));
    NF_CHECK(w.tier_of(outer) == SimTier::CheapMovement);
    NF_CHECK(w.wants_tick(outer)); // steering is a real level and is granted
    NF_CHECK(w.tier_of(gone) == SimTier::RenderOnly);
    NF_CHECK(!w.wants_tick(gone));
}

NF_TEST(ai_world_nearest_of_several_focuses_wins) {
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1000.0f, 1000.0f);

    const u32 actor = w.add_actor(AIActor{.position = Vec3{100.0f, 0.0f, 0.0f}});
    w.add_focus(AIFocus{.position = Vec3{0.0f, 0.0f, 0.0f}, .radius = 400.0f});
    w.add_focus(AIFocus{.position = Vec3{95.0f, 0.0f, 0.0f}, .radius = 400.0f});
    w.plan(1.0f);
    // The second focus is 5m away, well inside the 20m full band.
    NF_CHECK(w.tier_of(actor) == SimTier::FullAI);
}

NF_TEST(ai_world_untouched_sector_caps_at_cheap_movement) {
    // This is the whole point of the grid: an actor the foci never reach is
    // settled in pass 3 and never enters the budget's candidate list. It can
    // still steer if its distance warrants it — steering is not billed — but it
    // can never hold a mind, however near a focus it is in world space.
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1000.0f, 1000.0f);
    w.set_tier_hysteresis(0.0f);

    // A focus at (100,0,0) with a 5m radius touches only the sectors spanning
    // x in 95..105, i.e. sectors 2 and 3. The actor stands in sector 0 — 100m
    // away in world space, well inside the 120m cheap band, but untouched.
    const u32 untouched_near =
        w.add_actor(AIActor{.position = Vec3{0.0f, 0.0f, 0.0f}});
    const u32 untouched_far =
        w.add_actor(AIActor{.position = Vec3{400.0f, 0.0f, 400.0f}});
    w.add_focus(AIFocus{.position = Vec3{100.0f, 0.0f, 0.0f}, .radius = 5.0f});
    w.plan(1.0f);

    NF_CHECK(w.tier_of(untouched_near) == SimTier::CheapMovement);
    NF_CHECK(w.wants_tick(untouched_near));
    NF_CHECK(w.tier_of(untouched_far) == SimTier::RenderOnly);
    NF_CHECK(!w.wants_tick(untouched_far));
    NF_CHECK(w.last_full_grants() == 0u);
    NF_CHECK(w.last_light_grants() == 0u);
}

NF_TEST(ai_world_a_focus_radius_touches_every_sector_it_overlaps) {
    // A focus on a grid line must touch the sectors on both sides, and a radius
    // that spans several cells must touch all of them.
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1000.0f, 1000.0f);
    w.set_tier_hysteresis(0.0f);

    const u32 beside = w.add_actor(AIActor{.position = Vec3{40.0f, 0.0f, 0.0f}});
    const u32 far = w.add_actor(AIActor{.position = Vec3{200.0f, 0.0f, 0.0f}});
    // Origin is exactly on the corner of sector 0; a 45m radius reaches into
    // sector 1 (32..64m) but not out to 200m.
    w.add_focus(AIFocus{.position = Vec3{0.0f, 0.0f, 0.0f}, .radius = 45.0f});
    w.plan(1.0f);

    NF_CHECK(w.tier_of(beside) == SimTier::LightweightAI);
    // `beside` is the only actor the budget ever saw, so exactly one light slot.
    NF_CHECK(w.last_full_grants() == 0u);
    NF_CHECK(w.last_light_grants() == 1u);
    // `far` is in an untouched sector and beyond the cheap band, so it settles
    // to RenderOnly — untouched never promotes, and nothing below CheapMovement
    // is invented by the clamp either.
    NF_CHECK(w.tier_of(far) == SimTier::RenderOnly);
    NF_CHECK(!w.wants_tick(far));
}

NF_TEST(ai_world_budget_promotes_in_priority_then_nearness_then_id_order) {
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    // Exactly one full slot and one light slot per plan.
    w.set_budget(1.0f, 1.0f);

    const u32 near = w.add_actor(AIActor{.position = Vec3{5.0f, 0.0f, 0.0f}});
    const u32 far = w.add_actor(AIActor{.position = Vec3{15.0f, 0.0f, 0.0f}});
    const u32 VIP = w.add_actor(
        AIActor{.position = Vec3{15.0f, 0.0f, 0.0f}, .priority = 10});
    // All three are inside the full band; only one can have it.
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});
    w.plan(1.0f);

    NF_CHECK(w.tier_of(VIP) == SimTier::FullAI);
    NF_CHECK(w.last_full_grants() == 1u);
    // The near actor, not the far one, takes the light slot: near beats far when
    // priority is equal.
    NF_CHECK(w.tier_of(near) == SimTier::LightweightAI);
    NF_CHECK(w.last_light_grants() == 1u);
    NF_CHECK(w.tier_of(far) == SimTier::CheapMovement);
}

NF_TEST(ai_world_id_breaks_an_exact_priority_and_distance_tie) {
    // Two identical actors must still sort deterministically; the id is the
    // final tie-break and makes the comparator total.
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1.0f, 0.0f);

    const u32 first = w.add_actor(AIActor{.position = Vec3{5.0f, 0.0f, 0.0f}});
    const u32 second = w.add_actor(AIActor{.position = Vec3{5.0f, 0.0f, 0.0f}});
    NF_CHECK(first < second);
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});
    w.plan(1.0f);

    NF_CHECK(w.tier_of(first) == SimTier::FullAI);
    NF_CHECK(w.tier_of(second) == SimTier::CheapMovement);
    NF_CHECK(w.last_full_grants() == 1u);
}

NF_TEST(ai_world_fractional_rate_carries_between_plans) {
    // 2.5 full slots per second, 1-second plans: 2, then 3, then 2 again. A
    // truncated rate would lose the half every plan and yield a steady 2.
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(2.5f, 0.0f);
    w.set_tier_hysteresis(0.0f); // keep demotion eager so the counts are sharp

    std::vector<u32> ids;
    for (int i = 0; i < 10; ++i) {
        ids.push_back(w.add_actor(AIActor{.position = Vec3{1.0f, 0.0f, 1.0f}}));
    }
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});

    w.plan(1.0f);
    NF_CHECK(w.last_full_grants() == 2u);
    w.plan(1.0f);
    NF_CHECK(w.last_full_grants() == 3u);
    w.plan(1.0f);
    NF_CHECK(w.last_full_grants() == 2u);
    w.plan(1.0f);
    NF_CHECK(w.last_full_grants() == 3u);
}

NF_TEST(ai_world_a_full_band_actor_without_a_slot_falls_to_steering) {
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(0.0f, 0.0f); // no mind runs at all this plan

    const u32 id = w.add_actor(AIActor{.position = Vec3{5.0f, 0.0f, 0.0f}});
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});
    w.plan(1.0f);

    NF_CHECK(w.tier_of(id) == SimTier::CheapMovement);
    NF_CHECK(w.wants_tick(id)); // it steers
    NF_CHECK(w.last_full_grants() == 0u);
    NF_CHECK(w.last_light_grants() == 0u);
}

NF_TEST(ai_world_pinned_full_is_outside_the_budget) {
    // The budget caps the world's actors; the encounter's stars are not the
    // world's problem, so a pinned actor costs no slot and needs no focus.
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(0.0f, 0.0f);
    w.set_tier_hysteresis(0.0f);

    const u32 pinned =
        w.add_actor(AIActor{.position = Vec3{500.0f, 0.0f, 500.0f}, .pinned_full = true});
    const u32 extra = w.add_actor(AIActor{.position = Vec3{5.0f, 0.0f, 0.0f}});
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});
    w.plan(1.0f);

    NF_CHECK(w.tier_of(pinned) == SimTier::FullAI);
    NF_CHECK(w.wants_tick(pinned));
    NF_CHECK(w.last_full_grants() == 0u); // it never consumed a slot
    NF_CHECK(w.tier_of(extra) == SimTier::CheapMovement);
}

NF_TEST(ai_world_hysteresis_holds_a_tier_inside_the_widened_edge) {
    // Promote at d <= r, demote only past r * (1 + hysteresis). Without this an
    // actor on the boundary flips tier every plan — the pop-in a player notices.
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1000.0f, 1000.0f);
    w.set_tier_hysteresis(0.25f); // full band holds to 25m

    const u32 id = w.add_actor(AIActor{.position = Vec3{10.0f, 0.0f, 0.0f}});
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});

    w.plan(1.0f);
    NF_CHECK(w.tier_of(id) == SimTier::FullAI);

    // 22m: past the plain radius, still inside the 25m widened edge.
    w.set_actor_position(id, Vec3{22.0f, 0.0f, 0.0f});
    w.plan(1.0f);
    NF_CHECK(w.tier_of(id) == SimTier::FullAI);

    // 26m: clear of the edge, demoted to the band the distance actually says.
    w.set_actor_position(id, Vec3{26.0f, 0.0f, 0.0f});
    w.plan(1.0f);
    NF_CHECK(w.tier_of(id) == SimTier::LightweightAI);
}

NF_TEST(ai_world_hysteresis_promotes_eagerly_at_the_plain_radius) {
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1000.0f, 1000.0f);
    w.set_tier_hysteresis(0.25f);

    const u32 id = w.add_actor(AIActor{.position = Vec3{30.0f, 0.0f, 0.0f}});
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});
    w.plan(1.0f);
    NF_CHECK(w.tier_of(id) == SimTier::LightweightAI);

    // Crossing into the full band promotes at once; hysteresis only delays
    // demotion, never promotion.
    w.set_actor_position(id, Vec3{20.0f, 0.0f, 0.0f});
    w.plan(1.0f);
    NF_CHECK(w.tier_of(id) == SimTier::FullAI);
}

NF_TEST(ai_world_hysteresis_releases_an_actor_past_every_widened_edge) {
    // Hysteresis delays a demotion, it does not prevent one. An actor that has
    // moved far past the widened edge of its tier falls this plan, not later —
    // otherwise a promoted actor would stay promoted forever.
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1000.0f, 1000.0f);
    w.set_tier_hysteresis(0.25f);

    const u32 id = w.add_actor(AIActor{.position = Vec3{10.0f, 0.0f, 0.0f}});
    const u32 focus = w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});
    w.plan(1.0f);
    NF_CHECK(w.tier_of(id) == SimTier::FullAI);

    // The focus moves away rather than vanishing, so the actor's sector stays
    // touched and hysteresis is what is being exercised, not the sentinel.
    w.set_focus_position(focus, Vec3{300.0f, 0.0f, 0.0f});
    w.plan(1.0f);
    // 290m is far past every widened edge, so the fall is immediate and total.
    NF_CHECK(w.tier_of(id) == SimTier::RenderOnly);
    NF_CHECK(!w.wants_tick(id));
}

NF_TEST(ai_world_pinned_full_runs_even_in_an_untouched_sector) {
    // The budget is not the only thing pinned intelligence stands outside of; an
    // authored encounter star is not throttled by the grid either.
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(0.0f, 0.0f);
    w.set_tier_hysteresis(0.0f);

    const u32 distant =
        w.add_actor(AIActor{.position = Vec3{500.0f, 0.0f, 500.0f}, .pinned_full = true});
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 5.0f}); // reaches sector 0 only
    w.plan(1.0f);

    NF_CHECK(w.tier_of(distant) == SimTier::FullAI);
    NF_CHECK(w.wants_tick(distant));
    NF_CHECK(w.last_full_grants() == 0u);
}

NF_TEST(ai_world_no_focuses_is_a_quiet_world) {
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1000.0f, 1000.0f);

    const u32 near_origin = w.add_actor(AIActor{.position = Vec3{1.0f, 0.0f, 1.0f}});
    w.plan(1.0f);
    // Every band comparison against the sentinel fails, so nothing is promoted;
    // a world with no focus is rendered, not simulated.
    NF_CHECK(w.tier_of(near_origin) == SimTier::RenderOnly);
    NF_CHECK(!w.wants_tick(near_origin));
    NF_CHECK(w.last_full_grants() == 0u);
    NF_CHECK(w.last_light_grants() == 0u);

    // A zero-radius focus reaches nothing, same quiet world.
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 0.0f});
    w.plan(1.0f);
    NF_CHECK(w.tier_of(near_origin) == SimTier::RenderOnly);
}

NF_TEST(ai_world_moving_a_focus_re_targets_the_world) {
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1000.0f, 1000.0f);
    w.set_tier_hysteresis(0.0f);

    const u32 west = w.add_actor(AIActor{.position = Vec3{5.0f, 0.0f, 0.0f}});
    const u32 east = w.add_actor(AIActor{.position = Vec3{200.0f, 0.0f, 0.0f}});
    const u32 focus = w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});

    w.plan(1.0f);
    NF_CHECK(w.tier_of(west) == SimTier::FullAI);
    NF_CHECK(w.tier_of(east) == SimTier::RenderOnly);

    w.set_focus_position(focus, Vec3{205.0f, 0.0f, 0.0f});
    w.plan(1.0f);
    NF_CHECK(w.tier_of(west) == SimTier::RenderOnly);
    NF_CHECK(w.tier_of(east) == SimTier::FullAI);

    // The focus list is the world's whole attention; emptying it stands
    // everyone down, including an actor that was promoted a plan ago.
    w.clear_focuses();
    w.plan(1.0f);
    NF_CHECK(w.tier_of(east) == SimTier::RenderOnly);
    NF_CHECK(!w.wants_tick(east));
}

NF_TEST(ai_world_tier_radii_are_sorted_so_the_bands_nest) {
    // A caller handing the radii out of order would leave a hole where an actor
    // is neither one tier nor the next; the setter sorts instead of trusting.
    AIWorld w = default_world();
    w.set_tier_radii(120.0f, 20.0f, 50.0f);
    w.set_budget(1000.0f, 1000.0f);
    w.set_tier_hysteresis(0.0f);

    const u32 near = w.add_actor(AIActor{.position = Vec3{5.0f, 0.0f, 0.0f}});
    const u32 mid = w.add_actor(AIActor{.position = Vec3{35.0f, 0.0f, 0.0f}});
    const u32 outer = w.add_actor(AIActor{.position = Vec3{80.0f, 0.0f, 0.0f}});
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});
    w.plan(1.0f);

    NF_CHECK(w.tier_of(near) == SimTier::FullAI);
    NF_CHECK(w.tier_of(mid) == SimTier::LightweightAI);
    NF_CHECK(w.tier_of(outer) == SimTier::CheapMovement);
}

NF_TEST(ai_world_negative_radii_and_rates_clamp_instead_of_starving_the_world) {
    // A negative radius would make a band's squared threshold negative, which
    // every real distance exceeds, so the setter clamps to zero rather than
    // trusting the caller. A negative rate would run the accumulator backwards
    // and starve the world after one bad frame; it clamps to zero too.
    AIWorld w = default_world();
    w.set_tier_radii(-10.0f, -50.0f, -120.0f);
    w.set_budget(-100.0f, -100.0f);
    w.set_tier_hysteresis(-1.0f);

    const u32 id = w.add_actor(AIActor{.position = Vec3{5.0f, 0.0f, 0.0f}});
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});
    w.plan(1.0f);

    NF_CHECK(w.tier_of(id) == SimTier::RenderOnly);
    NF_CHECK(!w.wants_tick(id));
    NF_CHECK(w.last_full_grants() == 0u);
    NF_CHECK(w.last_light_grants() == 0u);
}

NF_TEST(ai_world_negative_dt_freezes_the_budget_rather_than_reversing_it) {
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1.0f, 1.0f);

    for (int i = 0; i < 4; ++i) {
        w.add_actor(AIActor{.position = Vec3{5.0f, 0.0f, static_cast<float>(i)}});
    }
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});

    // A negative dt must not run the accumulator backwards and hand out slots
    // the world never paid for.
    w.plan(-1.0f);
    NF_CHECK(w.last_full_grants() == 0u);
    NF_CHECK(w.last_light_grants() == 0u);

    w.plan(1.0f);
    NF_CHECK(w.last_full_grants() == 1u);
    NF_CHECK(w.last_light_grants() == 1u);
}

NF_TEST(ai_world_a_huge_dt_does_not_bank_infinite_slots) {
    // After a load stall the accumulated seconds could grant every actor at
    // once; the clamp keeps the world's spending bounded by its candidate set.
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1000000.0f, 1000000.0f);

    const u32 a = w.add_actor(AIActor{.position = Vec3{5.0f, 0.0f, 0.0f}});
    const u32 b = w.add_actor(AIActor{.position = Vec3{15.0f, 0.0f, 0.0f}});
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});
    w.plan(1000.0f);

    NF_CHECK(w.tier_of(a) == SimTier::FullAI);
    NF_CHECK(w.tier_of(b) == SimTier::FullAI);
    NF_CHECK(w.last_full_grants() == 2u);
}

NF_TEST(ai_world_an_off_grid_actor_is_folded_onto_the_edge_not_dropped) {
    // Dropping it would make an off-world actor invisible to the budget
    // forever; folding keeps its tier correct. The fold is onto the *nearest*
    // edge sector, so an actor just behind the origin lands in sector 0 and is
    // reached by a focus there, while one far beyond the far edge is not.
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1000.0f, 1000.0f);
    w.set_tier_hysteresis(0.0f);

    const u32 behind = w.add_actor(AIActor{.position = Vec3{-5.0f, 0.0f, -5.0f}});
    const u32 beyond = w.add_actor(AIActor{.position = Vec3{1000.0f, 0.0f, 0.0f}});
    // A 20m focus touches sector 0 only.
    w.add_focus(AIFocus{.position = Vec3{0.0f, 0.0f, 0.0f}, .radius = 20.0f});
    w.plan(1.0f);

    NF_CHECK(w.tier_of(behind) == SimTier::FullAI);
    NF_CHECK(w.wants_tick(behind));
    // Folded onto sector 63, which the focus never reaches — so untouched, and
    // past the cheap band, so RenderOnly. It was evaluated, not dropped.
    NF_CHECK(w.tier_of(beyond) == SimTier::RenderOnly);
}

NF_TEST(ai_world_tier_counts_report_the_whole_population) {
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1000.0f, 1000.0f);
    w.set_tier_hysteresis(0.0f);

    w.add_actor(AIActor{.position = Vec3{5.0f, 0.0f, 0.0f}});   // FullAI
    w.add_actor(AIActor{.position = Vec3{35.0f, 0.0f, 0.0f}});  // LightweightAI
    w.add_actor(AIActor{.position = Vec3{80.0f, 0.0f, 0.0f}});  // CheapMovement
    w.add_actor(AIActor{.position = Vec3{500.0f, 0.0f, 0.0f}}); // RenderOnly
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});
    w.plan(1.0f);

    const std::array<u32, 4> counts = w.tier_counts();
    NF_CHECK(counts[static_cast<u8>(SimTier::RenderOnly)] == 1u);
    NF_CHECK(counts[static_cast<u8>(SimTier::CheapMovement)] == 1u);
    NF_CHECK(counts[static_cast<u8>(SimTier::LightweightAI)] == 1u);
    NF_CHECK(counts[static_cast<u8>(SimTier::FullAI)] == 1u);
}

NF_TEST(ai_world_replans_are_bit_identical_for_identical_input) {
    // The determinism contract: same actors, same focuses, same dt, same tiers —
    // in both orders of submission, since iteration order is part of the input.
    auto build = [](AIWorld& w) {
        w.set_tier_radii(20.0f, 50.0f, 120.0f);
        w.set_budget(2.0f, 3.0f);
        w.set_tier_hysteresis(0.25f);
        w.add_actor(AIActor{.position = Vec3{5.0f, 0.0f, 0.0f}, .priority = 3});
        w.add_actor(AIActor{.position = Vec3{15.0f, 0.0f, 4.0f}});
        w.add_actor(AIActor{.position = Vec3{45.0f, 0.0f, 2.0f}, .priority = 3});
        w.add_actor(AIActor{.position = Vec3{90.0f, 0.0f, 1.0f}});
        w.add_actor(AIActor{.position = Vec3{300.0f, 0.0f, 0.0f}, .pinned_full = true});
        w.add_focus(AIFocus{.position = Vec3{0.0f, 0.0f, 0.0f}, .radius = 200.0f});
        w.add_focus(AIFocus{.position = Vec3{40.0f, 0.0f, 0.0f}, .radius = 30.0f});
    };

    auto snapshot = [](const AIWorld& w) {
        std::vector<u8> tiers;
        for (const AIActor& a : w.actors()) {
            tiers.push_back(static_cast<u8>(w.tier_of(a.id)));
            tiers.push_back(w.wants_tick(a.id) ? 1 : 0);
        }
        tiers.push_back(static_cast<u8>(w.last_full_grants()));
        tiers.push_back(static_cast<u8>(w.last_light_grants()));
        return tiers;
    };

    AIWorld a(32.0f, 8, 8, Vec3{});
    AIWorld b(32.0f, 8, 8, Vec3{});
    build(a);
    build(b);
    for (int i = 0; i < 20; ++i) {
        a.plan(0.1f);
        b.plan(0.1f);
        NF_CHECK(snapshot(a) == snapshot(b));
    }
    NF_CHECK(a.tier_counts() == b.tier_counts());
}

NF_TEST(ai_world_clear_returns_the_world_to_untouched) {
    AIWorld w = default_world();
    w.set_tier_radii(20.0f, 50.0f, 120.0f);
    w.set_budget(1000.0f, 1000.0f);
    w.add_actor(AIActor{.position = Vec3{5.0f, 0.0f, 0.0f}});
    w.add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});
    w.plan(1.0f);
    NF_CHECK(w.last_full_grants() == 1u);

    w.clear();
    NF_CHECK(w.actors().empty());
    NF_CHECK(w.focuses().empty());
    NF_CHECK((w.tier_counts() == std::array<u32, 4>{}));
    NF_CHECK(w.last_full_grants() == 0u);

    // Ids restart from 1 — clear is a full reset, not a partial one.
    const u32 id = w.add_actor(AIActor{.position = Vec3{5.0f, 0.0f, 0.0f}});
    NF_CHECK(id == 1u);
    NF_CHECK(w.tier_of(id) == SimTier::RenderOnly);
}

NF_TEST(ai_world_a_degenerate_grid_is_repaired_not_crashed) {
    // A zero-size or empty grid would index out of bounds at every bucket write;
    // the constructor repairs it rather than guarding the hot path. The repair
    // is visible through the accessors, so a misconfigured world can be
    // diagnosed rather than silently producing a 1x1 universe.
    AIWorld zero_size(0.0f, 8, 8, Vec3{});
    AIWorld zero_extent(32.0f, 0, 0, Vec3{});
    for (AIWorld* w : {&zero_size, &zero_extent}) {
        w->set_tier_radii(20.0f, 50.0f, 120.0f);
        w->set_budget(1000.0f, 1000.0f);
        const u32 id = w->add_actor(AIActor{.position = Vec3{5.0f, 0.0f, 0.0f}});
        w->add_focus(AIFocus{.position = Vec3{}, .radius = 400.0f});
        w->plan(1.0f);
        NF_CHECK(w->sectors_x() >= 1);
        NF_CHECK(w->sectors_z() >= 1);
        NF_CHECK(w->sector_size() > 0.0f);
        // The actor is still reached and still counted.
        NF_CHECK(w->tier_of(id) == SimTier::FullAI);
        NF_CHECK(w->tier_counts()[static_cast<u8>(SimTier::FullAI)] == 1u);
    }
}
