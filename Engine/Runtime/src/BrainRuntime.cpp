// NF/Runtime/src/BrainRuntime.cpp — the frame half of Phase 61.
//
// Perception (Phase 60) answers "what do I know". This answers "what do I want
// to do about it", and it is the second half of the same gap: `ai::UtilitySystem`
// and its response curves have been unit-tested since Phase 22 and reachable from
// nothing, because no scene could name a consideration and no frame turned the
// world into the numbers a consideration is a function of.
//
// Three decisions are worth reading before the code:
//
//   * The engine does not invent what an action MEANS. It scores the authored
//     considerations, publishes the winner's name, and stops there — "advance",
//     "flank" and "call for help" are a game's vocabulary, and an engine that
//     guessed at them would be wrong for every game that disagreed. The one
//     effect it does apply is the authored `hold_fire`, because a decision the
//     combat agent ignores is not a decision.
//
//   * The inputs are the world's, not the author's. They are computed here from
//     the components the frame already has — senses, hit points, magazine,
//     suppression, distance — so a curve is a function of the level rather than
//     of a number somebody typed twice.
//
//   * The clock is the only state. A brain re-decides when its authored period
//     elapses (0 means every frame), and between evaluations the decision and its
//     effect stand. A brain that re-decides every frame flickers between two
//     nearly equal utilities, and the rate is the cheapest cure there is.

#include <NF/Runtime/Runtime.hpp>

#include <NF/Core/Logger.hpp>
#include <NF/Scene/Transform.hpp>

#include <algorithm>
#include <cmath>

namespace nf::runtime {

const char* brain_input_name(BrainInput input) noexcept {
    switch (input) {
    case BrainInput::Aware:    return "aware";
    case BrainInput::Sensing:  return "sensing";
    case BrainInput::Distance: return "distance";
    case BrainInput::Health:   return "health";
    case BrainInput::Ammo:     return "ammo";
    case BrainInput::Threat:   return "threat";
    case BrainInput::Enemies:  return "enemies";
    }
    return "unknown";
}

bool brain_input_from_name(std::string_view name, BrainInput& out) noexcept {
    for (u32 i = 0; i < kBrainInputCount; ++i) {
        const auto kind = static_cast<BrainInput>(i);
        if (name == brain_input_name(kind)) {
            out = kind;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

void Runtime::bind_scene_brains() {
    // A scene change takes the decisions with it, like every other per-scene
    // artefact: what an NPC wanted in the level that was just replaced is not a
    // fact about this one.
    m_brains.clear();

    if (!m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();

    for (const ecs::Entity e : world.all_entities()) {
        const auto* component = world.get<BrainComponent>(e);
        if (component == nullptr || !component->enabled || component->actions.empty()) {
            continue;
        }

        BrainBinding binding;
        binding.entity = e;
        for (const BrainAction& action : component->actions) {
            const std::string key = brain_input_name(action.input);
            ai::ResponseCurve curve;
            curve.slope = action.slope;
            curve.exponent = action.exponent;
            curve.input_offset = action.input_offset;
            curve.output_offset = action.output_offset;

            // The provider reads the number the frame wrote for this input. The
            // key is captured by value: a string_view into the action would
            // dangle the moment the component is edited.
            ai::UtilityAction utility;
            utility = ai::UtilityAction(
                action.weight, curve,
                [key](const ai::Blackboard& board) {
                    return static_cast<float>(board.get_number(key, 0.0));
                },
                // Scoring never writes the board, and neither does the body: the
                // decision is recorded by the loop below, where the id-to-name
                // mapping lives.
                [](ai::Blackboard&, float) {});
            binding.system.add_action(std::move(utility));
            binding.action_names.push_back(action.name);
            binding.action_holds_fire.push_back(action.hold_fire);
        }
        m_brains.push_back(std::move(binding));
    }

    if (!m_brains.empty()) {
        NF_LOG_INFO(LogCategory::Core, "Runtime: {} decision maker(s) bound from the scene",
                    m_brains.size());
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool Runtime::brain_decision(ecs::Entity entity, std::string& out_name, f32& out_score) const {
    for (const BrainBinding& binding : m_brains) {
        if (binding.entity == entity) {
            if (binding.decision_name.empty()) {
                return false;
            }
            out_name = binding.decision_name;
            out_score = binding.decision_score;
            return true;
        }
    }
    return false;
}

f32 Runtime::brain_input_value(ecs::Entity entity, BrainInput input) const {
    for (const BrainBinding& binding : m_brains) {
        if (binding.entity == entity) {
            return binding.inputs[static_cast<u32>(input)];
        }
    }
    return 0.0f;
}

bool Runtime::brain_holds_fire(ecs::Entity entity) const {
    for (const BrainBinding& binding : m_brains) {
        if (binding.entity == entity) {
            return binding.holds_fire;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The inputs
// ---------------------------------------------------------------------------

void Runtime::fill_brain_inputs(const ecs::World& world, ecs::Entity entity,
                                std::array<f32, kBrainInputCount>& out) const {
    out.fill(0.0f);

    // --- who is this entity dealing with? ------------------------------------
    // The combat agent's own lock when it has one (that is who it is fighting),
    // otherwise the first actor its senses are aware of. Scenery is not in the
    // cast at all, so "first aware" is stable across frames for a given scene.
    ecs::Entity target = ecs::kInvalidEntity;
    const auto* agent = world.get<CombatAgentComponent>(entity);
    if (agent != nullptr && agent->locked_target.valid()) {
        target = agent->locked_target;
    }
    const ai::Perception* senses = perception_of(entity);
    if (target.valid() && senses == nullptr) {
        // An agent with no senses has a lock but nothing to prove it with; the
        // inputs below report what it can actually perceive, which is nothing.
        target = ecs::kInvalidEntity;
    }
    if (!target.valid() && senses != nullptr) {
        for (const ecs::Entity candidate : m_perception_target_entities) {
            if (candidate == entity) {
                continue;
            }
            const u32 local = perception_local_target_id(entity, candidate);
            if (local != 0u && senses->is_aware_of(local)) {
                target = candidate;
                break;
            }
        }
    }

    // --- the senses ----------------------------------------------------------
    if (senses != nullptr && target.valid()) {
        const u32 local = perception_local_target_id(entity, target);
        if (local != 0u) {
            out[static_cast<u32>(BrainInput::Aware)] = senses->is_aware_of(local) ? 1.0f : 0.0f;
            out[static_cast<u32>(BrainInput::Sensing)] = senses->is_sensing(local) ? 1.0f : 0.0f;
        }
    }

    // --- the geometry --------------------------------------------------------
    if (target.valid()) {
        const auto* self_transform = world.get<scene::Transform>(entity);
        const auto* target_transform = world.get<scene::Transform>(target);
        if (self_transform != nullptr && target_transform != nullptr) {
            const Vec3 delta{target_transform->world_x - self_transform->world_x,
                             target_transform->world_y - self_transform->world_y,
                             target_transform->world_z - self_transform->world_z};
            out[static_cast<u32>(BrainInput::Distance)] = delta.length();
        }
    }

    // --- what the entity is worth -------------------------------------------
    // No Health component means "not something that can be killed", and 1.0 is
    // the reading that keeps a "flee when hurt" consideration from firing on a
    // prop. No weapon means "nothing to spend", and the same argument applies.
    out[static_cast<u32>(BrainInput::Health)] = 1.0f;
    if (const auto* health = world.get<HealthComponent>(entity)) {
        const f32 maximum = health->max_health;
        out[static_cast<u32>(BrainInput::Health)] =
            maximum > 0.0f ? std::clamp(health->health / maximum, 0.0f, 1.0f) : 0.0f;
    }
    out[static_cast<u32>(BrainInput::Ammo)] = 1.0f;
    if (const auto* weapon = world.get<WeaponComponent>(entity)) {
        const f32 capacity = static_cast<f32>(weapon->profile.magazine);
        out[static_cast<u32>(BrainInput::Ammo)] =
            capacity > 0.0f
                ? std::clamp(static_cast<f32>(weapon->state.in_magazine) / capacity, 0.0f, 1.0f)
                : 0.0f;
    }

    out[static_cast<u32>(BrainInput::Threat)] = suppression_of(entity);

    // --- how much company ----------------------------------------------------
    if (senses != nullptr) {
        f32 aware = 0.0f;
        for (const ecs::Entity candidate : m_perception_target_entities) {
            if (candidate == entity) {
                continue;
            }
            const u32 local = perception_local_target_id(entity, candidate);
            if (local != 0u && senses->is_aware_of(local)) {
                aware += 1.0f;
            }
        }
        out[static_cast<u32>(BrainInput::Enemies)] = aware;
    }
}

// ---------------------------------------------------------------------------
// The step
// ---------------------------------------------------------------------------

void Runtime::step_brains(f32 dt) {
    if (m_brains.empty() || !m_scene_data_ptr || !m_scene_data_ptr->scene) {
        return;
    }
    if (!(dt > 0.0f) || !std::isfinite(dt)) {
        dt = 0.0f;
    }
    ecs::World& world = m_scene_data_ptr->scene->world();

    for (BrainBinding& binding : m_brains) {
        const auto* component = world.get<BrainComponent>(binding.entity);
        if (component == nullptr || !component->enabled || component->actions.empty()) {
            binding.holds_fire = false;
            continue;
        }

        // The inputs are refreshed every frame even between evaluations: they are
        // what a debug overlay shows, and they cost one pass over the entity's own
        // components.
        fill_brain_inputs(world, binding.entity, binding.inputs);
        for (u32 i = 0; i < kBrainInputCount; ++i) {
            binding.board.set_number(brain_input_name(static_cast<BrainInput>(i)),
                                    static_cast<double>(binding.inputs[i]));
        }

        // The clock. `decision_duration == 0` means "nothing has been decided
        // yet", so the first frame always evaluates no matter how long the
        // authored period is.
        binding.clock += dt;
        const f32 period = std::max(0.0f, component->rate);
        if (period > 0.0f && binding.clock < period && binding.decision_duration > 0u) {
            continue; // the decision, and its `hold_fire`, stand
        }
        binding.clock = 0.0f;

        const u32 winner = binding.system.tick(binding.board, dt);
        binding.decision_duration = binding.system.last_duration();
        if (winner >= binding.action_names.size()) {
            // An empty system, or a winner the binding does not know about:
            // nothing is decided rather than something wrong.
            binding.decision_name.clear();
            binding.decision_score = 0.0f;
            binding.holds_fire = false;
            continue;
        }
        binding.decision_name = binding.action_names[winner];
        binding.decision_score = binding.system.last_score();
        binding.holds_fire = binding.action_holds_fire[winner];
    }
}

} // namespace nf::runtime
