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

} // namespace nf::ai
