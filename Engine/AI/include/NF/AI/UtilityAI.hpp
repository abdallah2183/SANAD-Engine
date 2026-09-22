#pragma once

// NF/AI/UtilityAI.hpp — utility AI, or "the NPC that wants things" (design doc
// Section 45, "AI Core: utility AI").
//
// Behavior trees decide by *order*: a sequence's earlier child outranks a later
// one whether or not the later one is the better move right now. That is the
// right tool for a script and the wrong one for a creature. A wolf that is
// hungry, hurt and near a rabbit does not have a priority list — it has three
// considerations pulling against each other, and whichever pulls hardest wins.
//
// This module is that. Every action carries a response curve that turns one
// blackboard number into a 0..1 desirability; the system scores them all each
// tick and runs the winner. The curves are the authoring surface: "how badly do
// I want to flee at 30% health" is a shape, not a branch.
//
// Why it sits next to the tree rather than replacing it
// ------------------------------------------------------
// The two compose: a UtilitySystem is a fine leaf inside a behavior tree, and a
// tree is a fine action body inside a utility action. Use the tree where the
// *sequence* of decisions is the behaviour (a scripted encounter), the utility
// system where the *trade-off* is (an open-world creature that reacts). The
// Blackboard is shared, so neither owns the world.
//
// Determinism
// -----------
// Same contract as the rest of the module (NavGrid, Perception and
// StateMachine document it in more detail):
//   - actions are scored in insertion order and the winner keeps the earliest
//     index on a tie, so two runs pick identically;
//   - the only tie-break is that index — there is no RNG and no wall clock, and
//     an action that needs randomness takes an explicit, seeded source;
//   - scoring never mutates the Blackboard; only the winning action's execute
//     may write. A score that wrote would make the next action's score depend
//     on evaluation order, which is the one thing the ordering rule exists to
//     prevent;
//   - `dt` is the only time input, clamped non-negative.
//
// Units: scores are 0..1, dt is seconds. Curve inputs are whatever the action's
// provider returns — health percent, metres to cover, seconds since last meal.

#include <NF/AI/BehaviorTree.hpp> // Blackboard
#include <NF/Core/Types.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace nf::ai {

/// Shapes one raw input into a 0..1 desirability. All fields default to the
/// identity curve, so `ResponseCurve{}` scores the input unchanged; every other
/// field is a knob a designer turns without touching code.
///
/// The evaluation is `clamp(output_offset + slope * (input - input_offset)^exponent)`.
/// Offset then shape then offset again is enough to express the four curves a
// designer actually reaches for:
///   - linear (slope 1, exponent 1) — "closer is better, steadily";
///   - exponential (exponent 2) — "distance barely matters until it does";
///   - logistic-shaped (slope negative + output_offset 1) — "sated then eager";
///   - thresholded (slope large, input_offset at the breakpoint) — a step.
struct ResponseCurve {
    /// Multiplier on the shaped value. Negative inverts the curve: distance
    /// becomes "closer is worse", which is what fleeing wants.
    float slope = 1.0f;

    /// Power the offset input is raised to. 1 is linear, 2 is quadratic, 0.5 is
    /// a sqrt (fast out, slow in — good for "almost there"). Must stay finite
    /// and non-negative: a negative exponent on a zero input is division by
    /// zero, and a NaN here would poison the whole comparison.
    float exponent = 1.0f;

    /// Subtracted from the input before shaping. This is where a "starts to
    /// matter at 40 metres" breakpoint lives.
    float input_offset = 0.0f;

    /// Added after shaping, before clamping. Pairs with a negative slope to lift
    /// an inverted curve back into 0..1.
    float output_offset = 0.0f;

    /// Shaping bounds. Defaults pin the result to the valid score range; a
    /// caller widening them is responsible for the arithmetic still being
    /// meaningful, because a score outside 0..1 still compares correctly but no
    /// longer reads as a desirability.
    float clamp_min = 0.0f;
    float clamp_max = 1.0f;

    /// Evaluates the curve at `input`. A NaN input is treated as 0: it compares
    /// lowest rather than poisoning the argmax with NaN's false comparisons.
    float evaluate(float input) const;
};

/// "Consider doing this." A provider pulls one number from the Blackboard, a
/// curve shapes it, a weight scales the result, and if the action wins its
/// execute runs. The provider and the execute are functions rather than virtual
/// methods so an action is composable at a call site — the same reason BTAction
/// takes a std::function.
class UtilityAction {
public:
    UtilityAction() = default;
    UtilityAction(float weight, ResponseCurve curve,
                  std::function<float(const Blackboard&)> provider,
                  std::function<void(Blackboard&, float)> execute);

    /// `weight * curve(provider(bb))`. Never writes to `bb` — see the module's
    /// determinism note. The weight is a flat multiplier, not a curve field, so
    /// "this creature cares twice as much about food" is separable from "food
    /// desirability rises steeply with hunger".
    float score(const Blackboard& bb) const;

    /// Runs the action body. Only the winner's execute is called each tick.
    void execute(Blackboard& bb, float dt) const;

    float weight() const { return m_weight; }
    const ResponseCurve& curve() const { return m_curve; }

private:
    float m_weight = 1.0f;
    ResponseCurve m_curve{};
    std::function<float(const Blackboard&)> m_provider;
    std::function<void(Blackboard&, float)> m_execute;
};

class UtilitySystem {
public:
    UtilitySystem() = default;

    /// Appends an action and returns its id. Ids are insertion order and never
    /// recycled — the same contract as PerceptionScene and StateMachine.
    u32 add_action(UtilityAction action);

    /// Scores every action, runs the winner, and returns the winner's id. Ties
    /// go to the earlier action (strict `>` in the argmax), so the choice is
    /// reproducible. An empty system returns kNone and runs nothing.
    u32 tick(Blackboard& bb, float dt);

    /// The id of the action that ran on the most recent tick, or kNone.
    u32 last_action() const { return m_last_action; }

    /// The score the winner posted, for a debug meter or an editor graph. A
    /// replay asserts this alongside last_action: same choice *and* same
    /// magnitude is the cheap proof that the curves are being fed the same
    /// inputs, whereas the id alone would hide a change in how close it was.
    float last_score() const { return m_last_score; }

    /// How many ticks the winning action has held the slot for. The useful
    /// signal here is *change*: an action that flips every tick is an NPC that
    // can't commit, which usually means two curves cross too near each other —
    // a tuning bug, not a runtime one.
    u32 last_duration() const { return m_last_duration; }

    /// Read-only access for debuggers and editors. Insertion order.
    const std::vector<UtilityAction>& actions() const { return m_actions; }

    /// Sentinel for "no action ran": an empty system, or one whose every action
    /// scored zero. Deliberately out of range of any real id.
    static inline constexpr u32 kNone = 0xFFFFFFFFu;

private:
    std::vector<UtilityAction> m_actions;
    u32 m_last_action = kNone;
    float m_last_score = 0.0f;
    u32 m_last_duration = 0;
};

} // namespace nf::ai
