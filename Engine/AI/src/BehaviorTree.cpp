// NF/AI/BehaviorTree.cpp — reactive behavior tree nodes.

#include <NF/AI/BehaviorTree.hpp>

namespace nf::ai {

void Blackboard::clear() {
    m_numbers.clear();
    m_flags.clear();
    m_texts.clear();
}

BTStatus BTSequence::tick(Blackboard& bb, float dt) {
    if (m_children.empty()) return BTStatus::Success; // vacuous truth
    for (auto& child : m_children) {
        const BTStatus s = child->tick(bb, dt);
        if (s != BTStatus::Success) return s; // Failure or Running
    }
    return BTStatus::Success;
}

void BTSequence::reset() {
    for (auto& child : m_children) child->reset();
}

BTStatus BTSelector::tick(Blackboard& bb, float dt) {
    if (m_children.empty()) return BTStatus::Failure; // nothing to fall back to
    for (auto& child : m_children) {
        const BTStatus s = child->tick(bb, dt);
        if (s != BTStatus::Failure) return s; // Success or Running
    }
    return BTStatus::Failure;
}

void BTSelector::reset() {
    for (auto& child : m_children) child->reset();
}

BTStatus BTInverter::tick(Blackboard& bb, float dt) {
    if (!m_child) return BTStatus::Failure;
    const BTStatus s = m_child->tick(bb, dt);
    if (s == BTStatus::Success) return BTStatus::Failure;
    if (s == BTStatus::Failure) return BTStatus::Success;
    return BTStatus::Running;
}

void BTInverter::reset() {
    if (m_child) m_child->reset();
}

BTStatus BTRepeat::tick(Blackboard& bb, float dt) {
    if (!m_child) return BTStatus::Failure;
    if (m_times <= 0) {
        // Forever: tick once per frame, never complete (Running survives).
        m_child->tick(bb, dt);
        return BTStatus::Running;
    }
    while (m_done < m_times) {
        const BTStatus s = m_child->tick(bb, dt);
        if (s == BTStatus::Running) return BTStatus::Running;
        if (s == BTStatus::Failure) {
            m_done = 0; // failure restarts the count, still working
            return BTStatus::Running;
        }
        ++m_done;
        if (m_done >= m_times) return BTStatus::Success;
        // Success but more iterations remain: loop again this same tick so a
        // Repeat of instant children finishes in one frame, not N frames.
    }
    return BTStatus::Success;
}

void BTRepeat::reset() {
    m_done = 0;
    if (m_child) m_child->reset();
}

BTStatus BTWait::tick(Blackboard&, float dt) {
    m_elapsed += dt > 0.0f ? dt : 0.0f;
    return m_elapsed >= m_seconds ? BTStatus::Success : BTStatus::Running;
}

void BTWait::reset() {
    m_elapsed = 0.0f;
}

BTStatus BehaviorTree::tick(Blackboard& bb, float dt) {
    if (!m_root) return BTStatus::Failure;
    return m_root->tick(bb, dt);
}

void BehaviorTree::reset() {
    if (m_root) m_root->reset();
}

BTStatus BTParallel::tick(Blackboard& bb, float dt) {
    if (m_children.empty()) return BTStatus::Success;

    size_t success_count = 0;
    size_t failure_count = 0;

    for (auto& child : m_children) {
        const BTStatus s = child->tick(bb, dt);
        if (s == BTStatus::Success) {
            success_count++;
        } else if (s == BTStatus::Failure) {
            failure_count++;
        }
    }

    if (m_policy == BTParallelPolicy::RequireAll) {
        if (failure_count > 0) return BTStatus::Failure;
        if (success_count == m_children.size()) return BTStatus::Success;
        return BTStatus::Running;
    } else {
        if (success_count > 0) return BTStatus::Success;
        if (failure_count == m_children.size()) return BTStatus::Failure;
        return BTStatus::Running;
    }
}

void BTParallel::reset() {
    for (auto& child : m_children) child->reset();
}

BTStatus BTCooldown::tick(Blackboard& bb, float dt) {
    m_time_since_last_success += (dt > 0.0f) ? dt : 0.0f;
    if (m_time_since_last_success < m_cooldown) {
        return BTStatus::Failure;
    }
    if (!m_child) return BTStatus::Failure;
    const BTStatus s = m_child->tick(bb, dt);
    if (s == BTStatus::Success) {
        m_time_since_last_success = 0.0f;
    }
    return s;
}

void BTCooldown::reset() {
    m_time_since_last_success = 1e9f;
    if (m_child) m_child->reset();
}

BTStatus BTRetry::tick(Blackboard& bb, float dt) {
    if (!m_child) return BTStatus::Failure;
    const BTStatus s = m_child->tick(bb, dt);
    if (s == BTStatus::Success) {
        m_current_retries = 0;
        return BTStatus::Success;
    }
    if (s == BTStatus::Running) {
        return BTStatus::Running;
    }

    m_current_retries++;
    if (m_current_retries >= m_max_retries) {
        m_current_retries = 0;
        return BTStatus::Failure;
    }
    m_child->reset();
    return BTStatus::Running;
}

void BTRetry::reset() {
    m_current_retries = 0;
    if (m_child) m_child->reset();
}

BTStatus BTTimeLimit::tick(Blackboard& bb, float dt) {
    if (!m_child) return BTStatus::Failure;
    m_elapsed += (dt > 0.0f) ? dt : 0.0f;
    if (m_elapsed > m_max_seconds) {
        m_child->reset();
        m_elapsed = 0.0f;
        return BTStatus::Failure;
    }
    const BTStatus s = m_child->tick(bb, dt);
    if (s != BTStatus::Running) {
        m_elapsed = 0.0f;
    }
    return s;
}

void BTTimeLimit::reset() {
    m_elapsed = 0.0f;
    if (m_child) m_child->reset();
}

BTStatus BTForceSuccess::tick(Blackboard& bb, float dt) {
    if (!m_child) return BTStatus::Success;
    const BTStatus s = m_child->tick(bb, dt);
    return (s == BTStatus::Running) ? BTStatus::Running : BTStatus::Success;
}

void BTForceSuccess::reset() {
    if (m_child) m_child->reset();
}

BTStatus BTForceFailure::tick(Blackboard& bb, float dt) {
    if (!m_child) return BTStatus::Failure;
    const BTStatus s = m_child->tick(bb, dt);
    return (s == BTStatus::Running) ? BTStatus::Running : BTStatus::Failure;
}

void BTForceFailure::reset() {
    if (m_child) m_child->reset();
}

BTBuilder::BTBuilder() = default;

void BTBuilder::add_child(std::unique_ptr<BTNode> child) {
    if (!m_stack.empty()) {
        m_stack.back().children.push_back(std::move(child));
    } else {
        m_root = std::move(child);
    }
}

BTBuilder& BTBuilder::sequence() {
    m_stack.push_back({FrameType::Sequence, 0.0f, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::selector() {
    m_stack.push_back({FrameType::Selector, 0.0f, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::parallel(BTParallelPolicy policy) {
    m_stack.push_back({(policy == BTParallelPolicy::RequireAll) ? FrameType::ParallelAll : FrameType::ParallelOne, 0.0f, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::inverter() {
    m_stack.push_back({FrameType::Inverter, 0.0f, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::repeat(int times) {
    m_stack.push_back({FrameType::Repeat, 0.0f, times, {}});
    return *this;
}

BTBuilder& BTBuilder::cooldown(float seconds) {
    m_stack.push_back({FrameType::Cooldown, seconds, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::retry(int max_retries) {
    m_stack.push_back({FrameType::Retry, 0.0f, max_retries, {}});
    return *this;
}

BTBuilder& BTBuilder::time_limit(float max_seconds) {
    m_stack.push_back({FrameType::TimeLimit, max_seconds, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::force_success() {
    m_stack.push_back({FrameType::ForceSuccess, 0.0f, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::force_failure() {
    m_stack.push_back({FrameType::ForceFailure, 0.0f, 0, {}});
    return *this;
}

BTBuilder& BTBuilder::wait(float seconds) {
    add_child(std::make_unique<BTWait>(seconds));
    return *this;
}

BTBuilder& BTBuilder::action(std::function<BTStatus(Blackboard&, float)> fn) {
    add_child(std::make_unique<BTAction>(std::move(fn)));
    return *this;
}

BTBuilder& BTBuilder::condition(std::function<bool(const Blackboard&)> fn) {
    add_child(std::make_unique<BTCondition>(std::move(fn)));
    return *this;
}

BTBuilder& BTBuilder::end() {
    if (m_stack.empty()) return *this;

    StackFrame frame = std::move(m_stack.back());
    m_stack.pop_back();

    std::unique_ptr<BTNode> node;
    switch (frame.type) {
        case FrameType::Sequence: {
            auto seq = std::make_unique<BTSequence>();
            for (auto& c : frame.children) seq->add(std::move(c));
            node = std::move(seq);
            break;
        }
        case FrameType::Selector: {
            auto sel = std::make_unique<BTSelector>();
            for (auto& c : frame.children) sel->add(std::move(c));
            node = std::move(sel);
            break;
        }
        case FrameType::ParallelAll: {
            auto par = std::make_unique<BTParallel>(BTParallelPolicy::RequireAll);
            for (auto& c : frame.children) par->add(std::move(c));
            node = std::move(par);
            break;
        }
        case FrameType::ParallelOne: {
            auto par = std::make_unique<BTParallel>(BTParallelPolicy::RequireOne);
            for (auto& c : frame.children) par->add(std::move(c));
            node = std::move(par);
            break;
        }
        case FrameType::Inverter: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTInverter>(std::move(child));
            break;
        }
        case FrameType::Repeat: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTRepeat>(std::move(child), frame.param_i);
            break;
        }
        case FrameType::Cooldown: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTCooldown>(std::move(child), frame.param_f);
            break;
        }
        case FrameType::Retry: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTRetry>(std::move(child), frame.param_i);
            break;
        }
        case FrameType::TimeLimit: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTTimeLimit>(std::move(child), frame.param_f);
            break;
        }
        case FrameType::ForceSuccess: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTForceSuccess>(std::move(child));
            break;
        }
        case FrameType::ForceFailure: {
            auto child = !frame.children.empty() ? std::move(frame.children[0]) : nullptr;
            node = std::make_unique<BTForceFailure>(std::move(child));
            break;
        }
    }

    if (node) {
        add_child(std::move(node));
    }
    return *this;
}

std::unique_ptr<BehaviorTree> BTBuilder::build() {
    while (!m_stack.empty()) {
        end();
    }
    return std::make_unique<BehaviorTree>(std::move(m_root));
}

} // namespace nf::ai
