#include <NF/Runtime/Shot.hpp>

namespace nf::runtime {

void ShotBuffer::push(ShotEvent event) {
    if (m_current.size() >= kCapacity) {
        // Oldest first, for the same reason the impact buffer does it: a
        // muzzle flash for the most recent shots is what a player can see.
        m_current.erase(m_current.begin());
    }
    m_current.push_back(std::move(event));
}

void ShotBuffer::begin_frame() {
    m_previous = std::move(m_current);
    m_current.clear();
}

void ShotBuffer::clear() {
    m_current.clear();
    m_previous.clear();
}

} // namespace nf::runtime
