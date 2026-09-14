#pragma once

#include <NF/Animation/AnimationClip.hpp>
#include <NF/Animation/AnimationPlayer.hpp>
#include <NF/Animation/Skeleton.hpp>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace nf::animation {

/// A parameter value for the state machine: float, int, or bool.
using ParamValue = std::variant<f32, i32, bool>;

/// A condition for a state transition. Each condition compares one parameter
/// against a threshold using one of the standard operators.
enum class ConditionOp {
    Greater,
    Less,
    GreaterEqual,
    LessEqual,
    Equals,
    NotEquals,
};

struct Condition {
    std::string param;
    ConditionOp op = ConditionOp::Greater;
    ParamValue threshold = 0.0f;
};

/// A transition from one state to another. Fires when all conditions are
/// true. `fade_duration` is the cross-fade time in seconds.
struct Transition {
    std::string target_state;
    f32 fade_duration = 0.2f;
    std::vector<Condition> conditions;
};

/// A state in the state machine: plays a clip with looping.
struct AnimState {
    std::string name;
    std::string clip_name;
    f32 speed = 1.0f;
    std::vector<Transition> transitions;
};

/// AnimationStateMachine: holds a set of states and transitions, evaluates
/// conditions each update, and produces a blended pose via cross-fade.
class AnimationStateMachine {
public:
    AnimationStateMachine() = default;

    /// Add a state. The first state added is the default/entry state.
    void add_state(const AnimState& state);

    /// Set a parameter value by name.
    void set_param(const std::string& name, ParamValue value);

    /// Get a parameter value (or nullptr if not found).
    const ParamValue* get_param(const std::string& name) const;

    /// Advance the state machine by `dt` seconds, given the clip table.
    /// `clips` maps clip names to AnimationClip objects.
    /// `skel` is the skeleton to sample against.
    /// `out_local` is filled with the blended local pose.
    void update(f32 dt,
                const Skeleton& skel,
                const std::unordered_map<std::string, const AnimationClip*>& clips,
                std::vector<LocalPose>& out_local);

    const std::string& current_state() const { return m_current_state; }
    const std::string& previous_state() const { return m_previous_state; }
    f32 fade_remaining() const { return m_fade_remaining; }

    /// Reset to the entry state.
    void reset();

    usize state_count() const { return m_states.size(); }

private:
    std::vector<AnimState> m_states;
    std::unordered_map<std::string, ParamValue> m_params;
    std::string m_current_state;
    std::string m_previous_state;
    f32 m_fade_remaining = 0.0f;
    f32 m_fade_duration = 0.0f;
    f32 m_current_time = 0.0f;
    f32 m_previous_time = 0.0f;

    const AnimState* find_state(const std::string& name) const;
    bool evaluate_conditions(const std::vector<Condition>& conds) const;
    void check_transitions(const Skeleton& skel,
                           const std::unordered_map<std::string, const AnimationClip*>& clips);
};

} // namespace nf::animation
