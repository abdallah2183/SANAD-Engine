#include <NF/Editor/Selection.hpp>

namespace nf::editor {

void Selection::set_single(ecs::Entity e) {
    m_primary = e;
    m_multi.clear();
    if (e.valid()) {
        m_multi.push_back(e);
    }
}

void Selection::add(ecs::Entity e) {
    if (!e.valid() || contains(e)) {
        return;
    }
    if (!m_primary.valid()) {
        m_primary = e;
    }
    m_multi.push_back(e);
}

void Selection::remove(ecs::Entity e) {
    for (auto it = m_multi.begin(); it != m_multi.end();) {
        if (*it == e) {
            it = m_multi.erase(it);
        } else {
            ++it;
        }
    }
    if (m_primary == e) {
        m_primary = m_multi.empty() ? ecs::kInvalidEntity : m_multi.front();
    }
}

void Selection::clear() {
    m_primary = ecs::kInvalidEntity;
    m_multi.clear();
}

bool Selection::contains(ecs::Entity e) const {
    for (const auto& m : m_multi) {
        if (m == e) {
            return true;
        }
    }
    return false;
}

void Selection::prune(const ecs::World& world) {
    for (auto it = m_multi.begin(); it != m_multi.end();) {
        if (!world.is_alive(*it)) {
            it = m_multi.erase(it);
        } else {
            ++it;
        }
    }
    if (m_primary.valid() && !world.is_alive(m_primary)) {
        m_primary = ecs::kInvalidEntity;
    }
    if (!m_primary.valid() && !m_multi.empty()) {
        m_primary = m_multi.front();
    }
    if (m_primary.valid() && !contains(m_primary)) {
        m_multi.insert(m_multi.begin(), m_primary);
    }
}

} // namespace nf::editor
