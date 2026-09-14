#include <NF/Animation/AnimationStateMachine.hpp>
#include <algorithm>
#include <cmath>

namespace nf::animation {

void AnimationStateMachine::add_state(const AnimState& state) {
    m_states.push_back(state);
    if (m_current_state.empty()) {
        m_current_state = state.name;
        m_current_time = 0.0f;
    }
}

void AnimationStateMachine::set_param(const std::string& name, ParamValue value) {
    m_params[name] = std::move(value);
}

const ParamValue* AnimationStateMachine::get_param(const std::string& name) const {
    auto it = m_params.find(name);
    if (it == m_params.end()) return nullptr;
    return &it->second;
}

void AnimationStateMachine::reset() {
    if (!m_states.empty()) {
        m_current_state = m_states[0].name;
    }
    m_previous_state.clear();
    m_current_time = 0.0f;
    m_previous_time = 0.0f;
    m_fade_remaining = 0.0f;
}

const AnimState* AnimationStateMachine::find_state(const std::string& name) const {
    for (const auto& s : m_states) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

bool AnimationStateMachine::evaluate_conditions(const std::vector<Condition>& conds) const {
    for (const auto& c : conds) {
        const ParamValue* pv = get_param(c.param);
        if (!pv) return false;

        // We support f32, i32, bool params. Convert threshold and value to f64 for comparison.
        auto to_f64 = [](const ParamValue& v) -> f64 {
            if (std::holds_alternative<f32>(v)) return static_cast<f64>(std::get<f32>(v));
            if (std::holds_alternative<i32>(v)) return static_cast<f64>(std::get<i32>(v));
            if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? 1.0 : 0.0;
            return 0.0;
        };

        f64 lhs = to_f64(*pv);
        f64 rhs = to_f64(c.threshold);

        switch (c.op) {
            case ConditionOp::Greater:      if (!(lhs >  rhs)) return false; break;
            case ConditionOp::Less:         if (!(lhs <  rhs)) return false; break;
            case ConditionOp::GreaterEqual: if (!(lhs >= rhs)) return false; break;
            case ConditionOp::LessEqual:    if (!(lhs <= rhs)) return false; break;
            case ConditionOp::Equals:       if (lhs != rhs) return false; break;
            case ConditionOp::NotEquals:    if (lhs == rhs) return false; break;
        }
    }
    return true;
}

void AnimationStateMachine::check_transitions(
    const Skeleton& /*skel*/,
    const std::unordered_map<std::string, const AnimationClip*>& /*clips*/) {

    const AnimState* cur = find_state(m_current_state);
    if (!cur) return;

    for (const auto& trans : cur->transitions) {
        if (evaluate_conditions(trans.conditions)) {
            m_previous_state = m_current_state;
            m_current_state = trans.target_state;
            m_previous_time = m_current_time;
            m_current_time = 0.0f;
            m_fade_duration = trans.fade_duration;
            m_fade_remaining = trans.fade_duration;
            return;
        }
    }
}

void AnimationStateMachine::update(f32 dt,
                                   const Skeleton& skel,
                                   const std::unordered_map<std::string, const AnimationClip*>& clips,
                                   std::vector<LocalPose>& out_local) {
    // Check transitions first — this may switch the current state and set up
    // the cross-fade, which is then applied in the same frame.
    check_transitions(skel, clips);

    const AnimState* cur = find_state(m_current_state);
    if (!cur) {
        out_local.resize(skel.bones.size());
        return;
    }

    auto clip_it = clips.find(cur->clip_name);
    const AnimationClip* cur_clip = (clip_it != clips.end()) ? clip_it->second : nullptr;

    if (cur_clip) {
        m_current_time += dt * cur->speed;
        if (cur_clip->duration > 0.0f && cur_clip->looping) {
            m_current_time = std::fmod(m_current_time, cur_clip->duration);
            if (m_current_time < 0.0f) m_current_time += cur_clip->duration;
        }
        cur_clip->sample(m_current_time, skel, out_local);
    } else {
        // No clip — rest pose.
        out_local.resize(skel.bones.size());
        for (usize i = 0; i < skel.bones.size(); ++i) {
            out_local[i].translation = skel.bones[i].rest_translation;
            out_local[i].rotation = skel.bones[i].rest_rotation;
            out_local[i].scale = skel.bones[i].rest_scale;
        }
    }

    // Cross-fade with previous state.
    if (m_fade_remaining > 0.0f && !m_previous_state.empty()) {
        const AnimState* prev = find_state(m_previous_state);
        if (prev) {
            auto prev_it = clips.find(prev->clip_name);
            const AnimationClip* prev_clip = (prev_it != clips.end()) ? prev_it->second : nullptr;
            if (prev_clip) {
                m_previous_time += dt * prev->speed;
                if (prev_clip->duration > 0.0f && prev_clip->looping) {
                    m_previous_time = std::fmod(m_previous_time, prev_clip->duration);
                    if (m_previous_time < 0.0f) m_previous_time += prev_clip->duration;
                }

                std::vector<LocalPose> prev_pose;
                prev_clip->sample(m_previous_time, skel, prev_pose);

                f32 fade_t = 1.0f;
                if (m_fade_duration > 1e-8f) {
                    fade_t = 1.0f - (m_fade_remaining / m_fade_duration);
                }
                if (fade_t < 0.0f) fade_t = 0.0f;
                if (fade_t > 1.0f) fade_t = 1.0f;

                // Blend: out = prev * (1 - fade_t) + cur * fade_t
                std::vector<const std::vector<LocalPose>*> pose_ptrs = {&prev_pose, &out_local};
                std::vector<f32> weights = {1.0f - fade_t, fade_t};
                blend_poses(pose_ptrs, weights, out_local);
            }
            m_fade_remaining -= dt;
            if (m_fade_remaining <= 0.0f) {
                m_fade_remaining = 0.0f;
                m_previous_state.clear();
            }
        }
    }

    // Evaluate transitions.
    check_transitions(skel, clips);
}

} // namespace nf::animation
