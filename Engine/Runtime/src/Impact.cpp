#include <NF/Runtime/Impact.hpp>

namespace nf::runtime {

void ImpactBuffer::push(ImpactEvent event) {
    if (m_current.size() >= kCapacity) {
        // Oldest first. A burst that overflows the cap keeps the most recent
        // impacts, which are the ones a hit marker and a spark still care about.
        m_current.erase(m_current.begin());
    }
    m_current.push_back(std::move(event));
}

void ImpactBuffer::begin_frame() {
    m_previous = std::move(m_current);
    m_current.clear();
}

void ImpactBuffer::clear() {
    m_current.clear();
    m_previous.clear();
}

} // namespace nf::runtime
