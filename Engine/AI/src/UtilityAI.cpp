#include <NF/AI/UtilityAI.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace nf::ai {

float ResponseCurve::evaluate(float input) const {
    // A NaN anywhere here makes every comparison in the argmax lie: `x > NaN`
    // is false, so a NaN-scoring action would silently lose to everything
    // *and* stop the actions after it from being compared correctly. Coercing
    // it to 0 makes the poison the problem of the provider that produced it,
    // where a NaN is visible in the input rather than hidden in a score.
    if (std::isnan(input)) input = 0.0f;

    const float shaped = std::pow(input - input_offset, exponent);
    const float out = output_offset + slope * shaped;
    if (out < clamp_min) return clamp_min;
    if (out > clamp_max) return clamp_max;
    return out;
}

UtilityAction::UtilityAction(float weight, ResponseCurve curve,
                             std::function<float(const Blackboard&)> provider,
                             std::function<void(Blackboard&, float)> execute)
    : m_weight(weight), m_curve(curve), m_provider(std::move(provider)),
      m_execute(std::move(execute)) {}

float UtilityAction::score(const Blackboard& bb) const {
    if (!m_provider) return 0.0f; // no input -> no opinion, not an exception
    return m_weight * m_curve.evaluate(m_provider(bb));
}

void UtilityAction::execute(Blackboard& bb, float dt) const {
    if (m_execute) m_execute(bb, dt);
}

u32 UtilitySystem::add_action(UtilityAction action) {
    m_actions.push_back(std::move(action));
    return static_cast<u32>(m_actions.size() - 1);
}

u32 UtilitySystem::tick(Blackboard& bb, float dt) {
    if (dt < 0.0f) dt = 0.0f;
    if (m_actions.empty()) {
        m_last_action = kNone;
        m_last_score = 0.0f;
        m_last_duration = 0;
        return kNone;
    }

    // Strict `>` keeps the earlier index on a tie, so the argmax is a function
    // of the inputs alone. Scoring is a separate pass from execution because the
    // winner's execute may write the blackboard — scoring it in the same pass
    // would let it change a rival's input before that rival was scored.
    u32 best = 0;
    float best_score = m_actions[0].score(bb);
    for (u32 i = 1; i < m_actions.size(); ++i) {
        const float s = m_actions[i].score(bb);
        if (s > best_score) {
            best_score = s;
            best = i;
        }
    }

    // A score of exactly zero is "this action does not want to run at all". It
    // still wins if every action is at zero — an NPC with nothing it wants does
    // nothing, which is the honest answer, and the duration counter below
    // distinguishes that from a real choice.
    m_actions[best].execute(bb, dt);

    if (best == m_last_action) {
        ++m_last_duration;
    } else {
        m_last_action = best;
        m_last_duration = 1;
    }
    m_last_score = best_score;
    return best;
}

} // namespace nf::ai
