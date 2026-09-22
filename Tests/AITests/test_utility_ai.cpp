// AITests/utility AI — response curves, scoring, and the argmax contract.
//
// Every case is pure CPU and drives the system through a Blackboard. Where a
// curve's arithmetic matters the test pins the number; where the *choice*
// matters it pins the id, because a curve can be right and the tie-break wrong
// and the NPC would still look fine until two actions scored alike.

#include <NF/AI/UtilityAI.hpp>
#include <NF/Test/TestFramework.hpp>

#include <cmath>
#include <vector>

using namespace nf;
using namespace nf::ai;

namespace {

UtilityAction make_action(float weight, ResponseCurve curve,
                          std::function<float(const Blackboard&)> provider,
                          std::function<void(Blackboard&, float)> execute = {}) {
    return UtilityAction(weight, curve, std::move(provider), std::move(execute));
}

} // namespace

NF_TEST(curve_identity_passes_the_input_through) {
    ResponseCurve c;
    NF_CHECK_NEAR(c.evaluate(0.0f), 0.0f, 1e-6f);
    NF_CHECK_NEAR(c.evaluate(0.4f), 0.4f, 1e-6f);
    NF_CHECK_NEAR(c.evaluate(1.0f), 1.0f, 1e-6f);
    // 2.0 is shaped to 2.0 and then clamped to the default 1.0 — the clamp
    // range is part of the identity curve, not something layered on after.
    NF_CHECK_NEAR(c.evaluate(2.0f), 1.0f, 1e-6f);
}

NF_TEST(curve_clamps_to_the_score_range) {
    ResponseCurve c;
    NF_CHECK_NEAR(c.evaluate(-5.0f), 0.0f, 1e-6f);
    NF_CHECK_NEAR(c.evaluate(5.0f), 1.0f, 1e-6f);
}

NF_TEST(curve_exponent_shapes_the_rise) {
    // NB: brace-initialising a field with `{}` zeroes it, it does not restore
    // the member default, so clamp_max has to be spelled out or every shaped
    // value collapses to 0.
    const ResponseCurve quad{1.0f, 2.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    NF_CHECK_NEAR(quad.evaluate(0.5f), 0.25f, 1e-6f);
    NF_CHECK_NEAR(quad.evaluate(0.9f), 0.81f, 1e-6f);

    const ResponseCurve root{1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f};
    NF_CHECK_NEAR(root.evaluate(0.25f), 0.5f, 1e-6f); // sqrt: fast out, slow in
    NF_CHECK_NEAR(root.evaluate(0.81f), 0.9f, 1e-6f);
}

NF_TEST(curve_input_offset_is_the_breakpoint) {
    // "Distance starts to matter at 10": everything below the offset scores 0.
    const ResponseCurve c{1.0f, 1.0f, 10.0f, 0.0f, 0.0f, 1.0f};
    NF_CHECK_NEAR(c.evaluate(0.0f), 0.0f, 1e-6f);
    NF_CHECK_NEAR(c.evaluate(9.0f), 0.0f, 1e-6f);
    NF_CHECK_NEAR(c.evaluate(12.0f), 1.0f, 1e-6f); // shaped to 2.0, clamped
}

NF_TEST(curve_negative_slope_inverts_it) {
    // "Closer is worse" for fleeing, lifted back into range by the output offset.
    // Health 0 -> flee at 1.0; health 1 -> flee at 0.0.
    ResponseCurve flee{-1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
    NF_CHECK_NEAR(flee.evaluate(0.0f), 1.0f, 1e-6f);
    NF_CHECK_NEAR(flee.evaluate(1.0f), 0.0f, 1e-6f);
    NF_CHECK_NEAR(flee.evaluate(0.3f), 0.7f, 1e-6f);
}

NF_TEST(curve_nan_input_scores_lowest_instead_of_poisoning) {
    ResponseCurve c;
    NF_CHECK_NEAR(c.evaluate(NAN), 0.0f, 1e-6f);
    // The point: a NaN does not make the comparison lie. Two actions, one fed a
    // NaN, one fed a real 0.2 — the real one wins.
    UtilitySystem sys;
    sys.add_action(make_action(1.0f, ResponseCurve{}, [](const Blackboard&) { return NAN; }));
    sys.add_action(make_action(1.0f, ResponseCurve{}, [](const Blackboard&) { return 0.2f; }));
    Blackboard bb;
    NF_CHECK(sys.tick(bb, 0.1f) == 1u);
}

NF_TEST(utility_action_weight_scales_the_curve) {
    Blackboard bb;
    bb.set_number("hunger", 0.5);
    const UtilityAction plain = make_action(1.0f, ResponseCurve{},
                                            [](const Blackboard& b) { return (float)b.get_number("hunger"); });
    const UtilityAction ravenous = make_action(2.0f, ResponseCurve{},
                                               [](const Blackboard& b) { return (float)b.get_number("hunger"); });
    NF_CHECK_NEAR(plain.score(bb), 0.5f, 1e-6f);
    NF_CHECK_NEAR(ravenous.score(bb), 1.0f, 1e-6f);
}

NF_TEST(utility_action_without_a_provider_scores_zero) {
    Blackboard bb;
    const UtilityAction no_input; // default-constructed
    NF_CHECK(no_input.score(bb) == 0.0f);
    NF_CHECK(no_input.weight() == 1.0f);
}

NF_TEST(utility_scoring_never_writes_the_blackboard) {
    Blackboard bb;
    bb.set_number("x", 1.0);
    UtilitySystem sys;
    // The provider is the only thing that reads during scoring; if scoring wrote
    // back, the second action would see the first one's side effects.
    sys.add_action(make_action(1.0f, ResponseCurve{}, [](const Blackboard& b) {
        return (float)b.get_number("x");
    }));
    sys.tick(bb, 0.1f);
    NF_CHECK_NEAR(bb.get_number("x"), 1.0, 1e-9); // untouched by the score pass
}

NF_TEST(utility_system_runs_only_the_winner) {
    std::vector<std::string> ran;
    Blackboard bb;
    bb.set_number("hunger", 0.9);
    bb.set_number("health", 0.9);

    UtilitySystem sys;
    sys.add_action(make_action(1.0f, ResponseCurve{},
                               [](const Blackboard& b) { return (float)b.get_number("hunger"); },
                               [&](Blackboard&, float) { ran.push_back("eat"); }));
    sys.add_action(make_action(1.0f, ResponseCurve{},
                               [](const Blackboard& b) { return (float)b.get_number("health"); },
                               [&](Blackboard&, float) { ran.push_back("fight"); }));

    NF_CHECK(sys.tick(bb, 0.1f) == 0u); // equal scores, earlier wins
    NF_CHECK(ran.size() == 1);
    NF_CHECK(ran.front() == "eat");
}

NF_TEST(utility_ties_break_to_the_earlier_action) {
    Blackboard bb;
    UtilitySystem sys;
    sys.add_action(make_action(1.0f, ResponseCurve{}, [](const Blackboard&) { return 0.5f; }));
    sys.add_action(make_action(1.0f, ResponseCurve{}, [](const Blackboard&) { return 0.5f; }));
    sys.add_action(make_action(1.0f, ResponseCurve{}, [](const Blackboard&) { return 0.5f; }));

    NF_CHECK(sys.tick(bb, 0.1f) == 0u);
    NF_CHECK(sys.tick(bb, 0.1f) == 0u); // deterministic across ticks, not just once
}

NF_TEST(utility_winner_tracks_the_input_across_ticks) {
    Blackboard bb;
    UtilitySystem sys;
    sys.add_action(make_action(1.0f, ResponseCurve{},
                               [](const Blackboard& b) { return (float)b.get_number("hunger"); }));
    sys.add_action(make_action(1.0f, ResponseCurve{},
                               [](const Blackboard& b) { return (float)b.get_number("threat"); }));

    bb.set_number("hunger", 0.9);
    bb.set_number("threat", 0.1);
    NF_CHECK(sys.tick(bb, 0.1f) == 0u);

    bb.set_number("hunger", 0.1);
    bb.set_number("threat", 0.9);
    NF_CHECK(sys.tick(bb, 0.1f) == 1u);
}

NF_TEST(utility_last_score_reports_the_winners_value) {
    Blackboard bb;
    UtilitySystem sys;
    sys.add_action(make_action(2.0f, ResponseCurve{}, [](const Blackboard&) { return 0.3f; }));
    sys.add_action(make_action(1.0f, ResponseCurve{}, [](const Blackboard&) { return 0.6f; }));

    NF_CHECK(sys.tick(bb, 0.1f) == 0u);      // 0.6 vs 0.6, tie -> earlier
    NF_CHECK_NEAR(sys.last_score(), 0.6f, 1e-6f);
}

NF_TEST(utility_duration_counts_consecutive_wins) {
    Blackboard bb;
    UtilitySystem sys;
    sys.add_action(make_action(1.0f, ResponseCurve{}, [](const Blackboard& b) {
        return (float)b.get_number("a");
    }));
    sys.add_action(make_action(1.0f, ResponseCurve{}, [](const Blackboard& b) {
        return (float)b.get_number("b");
    }));

    bb.set_number("a", 0.9);
    bb.set_number("b", 0.1);
    sys.tick(bb, 0.1f);
    NF_CHECK(sys.last_duration() == 1u);
    sys.tick(bb, 0.1f);
    NF_CHECK(sys.last_duration() == 2u);

    // A flip resets, it does not increment.
    bb.set_number("a", 0.1);
    bb.set_number("b", 0.9);
    sys.tick(bb, 0.1f);
    NF_CHECK(sys.last_action() == 1u);
    NF_CHECK(sys.last_duration() == 1u);
}

NF_TEST(utility_empty_system_runs_nothing) {
    UtilitySystem sys;
    Blackboard bb;
    NF_CHECK(sys.tick(bb, 0.1f) == UtilitySystem::kNone);
    NF_CHECK(sys.last_action() == UtilitySystem::kNone);
    NF_CHECK(sys.last_score() == 0.0f);
    NF_CHECK(sys.last_duration() == 0u);
}

NF_TEST(utility_all_zero_scores_still_pick_an_action) {
    // Nothing is wanted, so the first action wins by the tie rule and its body
    // runs. That is the honest behaviour — kNone means "no actions exist".
    std::vector<std::string> ran;
    Blackboard bb;
    UtilitySystem sys;
    sys.add_action(make_action(1.0f, ResponseCurve{}, [](const Blackboard&) { return 0.0f; },
                               [&](Blackboard&, float) { ran.push_back("idle"); }));
    NF_CHECK(sys.tick(bb, 0.1f) == 0u);
    NF_CHECK(ran.size() == 1);
}

NF_TEST(utility_actions_get_insertion_order_ids) {
    UtilitySystem sys;
    const u32 a = sys.add_action(UtilityAction{});
    const u32 b = sys.add_action(UtilityAction{});
    const u32 c = sys.add_action(UtilityAction{});
    NF_CHECK(a == 0u);
    NF_CHECK(b == 1u);
    NF_CHECK(c == 2u);
    NF_CHECK(sys.actions().size() == 3);
}

NF_TEST(utility_execute_receives_dt) {
    float seen = -1.0f;
    Blackboard bb;
    UtilitySystem sys;
    sys.add_action(make_action(1.0f, ResponseCurve{}, [](const Blackboard&) { return 1.0f; },
                               [&](Blackboard&, float dt) { seen = dt; }));
    sys.tick(bb, 0.25f);
    NF_CHECK_NEAR(seen, 0.25f, 1e-6f);
}

NF_TEST(utility_negative_dt_does_not_run_time_backwards) {
    Blackboard bb;
    UtilitySystem sys;
    sys.add_action(make_action(1.0f, ResponseCurve{}, [](const Blackboard&) { return 1.0f; }));
    NF_CHECK(sys.tick(bb, -100.0f) == 0u); // a caller bug, not a crash
}

NF_TEST(utility_replays_bit_identically) {
    auto run = [] {
        Blackboard bb;
        bb.set_number("hunger", 0.7);
        bb.set_number("health", 0.4);
        bb.set_number("distance", 12.0);

        UtilitySystem sys;
        sys.add_action(make_action(1.0f, ResponseCurve{},
                                   [](const Blackboard& b) { return (float)b.get_number("hunger"); }));
        sys.add_action(make_action(1.5f, ResponseCurve{1.0f, 2.0f, 0.0f, 0.0f, 0.0f, 1.0f},
                                   [](const Blackboard& b) { return 1.0f - (float)b.get_number("health"); }));
        sys.add_action(make_action(0.8f, ResponseCurve{-1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
                                   [](const Blackboard& b) { return (float)b.get_number("distance") / 20.0f; }));

        std::vector<u32> picks;
        std::vector<float> scores;
        for (int i = 0; i < 5; ++i) {
            picks.push_back(sys.tick(bb, 0.1f));
            scores.push_back(sys.last_score());
        }
        return std::pair(picks, scores);
    };

    const auto [p1, s1] = run();
    const auto [p2, s2] = run();
    NF_CHECK(p1.size() == p2.size());
    for (size_t i = 0; i < p1.size(); ++i) {
        NF_CHECK(p1[i] == p2[i]);
        NF_CHECK(s1[i] == s2[i]); // exact equality: same curves, same inputs
    }
}

NF_TEST(utility_two_crossing_curves_flip_on_the_crossing) {
    // The classic tuning bug: two curves that cross make the NPC oscillate one
    // tick in two. This pins the crossing point so the flip is where the curves
    // say it is.
    Blackboard bb;
    UtilitySystem sys;
    // a falls with x, b rises with x; they cross at x = 0.5.
    sys.add_action(make_action(1.0f, ResponseCurve{-1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
                               [](const Blackboard& b) { return (float)b.get_number("x"); }));
    sys.add_action(make_action(1.0f, ResponseCurve{},
                               [](const Blackboard& b) { return (float)b.get_number("x"); }));

    bb.set_number("x", 0.4);
    NF_CHECK(sys.tick(bb, 0.1f) == 0u);
    bb.set_number("x", 0.6);
    NF_CHECK(sys.tick(bb, 0.1f) == 1u);
    bb.set_number("x", 0.5); // exactly crossing: the tie goes to the earlier
    NF_CHECK(sys.tick(bb, 0.1f) == 0u);
}
