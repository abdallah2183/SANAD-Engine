// NF/Gameplay/Inventory.cpp — stacked item inventory.

#include <NF/Gameplay/Inventory.hpp>

#include <algorithm>
#include <sstream>

namespace nf::gameplay {

u32 Inventory::add(const std::string& id, u32 count, u32 max_stack) {
    if (id.empty() || count == 0) return 0;
    if (max_stack == 0) max_stack = 1;
    u32 added = 0;
    // Fill partial stacks of the same id first (sorted order is preserved:
    // existing stacks keep their positions).
    for (auto& s : m_stacks) {
        if (count == 0) break;
        if (s.id != id || s.count >= s.max_stack) continue;
        const u32 room = s.max_stack - s.count;
        const u32 take = room < count ? room : count;
        s.count += take;
        count -= take;
        added += take;
    }
    // Open new stacks while there is room and remainder.
    while (count > 0 && m_stacks.size() < m_max_stacks) {
        ItemStack s;
        s.id = id;
        s.max_stack = max_stack;
        s.count = count < max_stack ? count : max_stack;
        count -= s.count;
        added += s.count;
        // Keep sorted by id for deterministic saves.
        auto pos = m_stacks.begin();
        while (pos != m_stacks.end() && pos->id < id) ++pos;
        m_stacks.insert(pos, s);
    }
    return added;
}

u32 Inventory::remove(const std::string& id, u32 count) {
    if (id.empty() || count == 0) return 0;
    u32 removed = 0;
    for (auto it = m_stacks.begin(); it != m_stacks.end() && count > 0;) {
        if (it->id != id) {
            ++it;
            continue;
        }
        const u32 take = it->count < count ? it->count : count;
        it->count -= take;
        count -= take;
        removed += take;
        if (it->count == 0) {
            it = m_stacks.erase(it);
        } else {
            ++it;
        }
    }
    return removed;
}

u32 Inventory::count_of(const std::string& id) const {
    u32 total = 0;
    for (const auto& s : m_stacks) {
        if (s.id == id) total += s.count;
    }
    return total;
}

bool Inventory::has(const std::string& id, u32 count) const {
    return count_of(id) >= count;
}

void Inventory::clear() {
    m_stacks.clear();
}

std::string Inventory::serialize() const {
    // one stack per line: id|max|count (ids are single-line by contract of add()).
    std::ostringstream out;
    for (const auto& s : m_stacks) {
        std::string id = s.id;
        for (char& c : id) {
            if (c == '|' || c == '\n') c = '_';
        }
        out << id << '|' << s.max_stack << '|' << s.count << '\n';
    }
    return out.str();
}

bool Inventory::parse(const std::string& text) {
    std::vector<ItemStack> parsed;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const usize sep1 = line.find('|');
        const usize sep2 = line.find('|', sep1 == std::string::npos ? 0 : sep1 + 1);
        if (sep1 == std::string::npos || sep2 == std::string::npos) continue;
        ItemStack s;
        s.id = line.substr(0, sep1);
        if (s.id.empty()) continue;
        try {
            s.max_stack = static_cast<u32>(std::stoul(line.substr(sep1 + 1, sep2 - sep1 - 1)));
            s.count = static_cast<u32>(std::stoul(line.substr(sep2 + 1)));
        } catch (...) {
            continue;
        }
        if (s.max_stack == 0) s.max_stack = 1;
        if (s.count > s.max_stack) s.count = s.max_stack;
        if (s.count == 0) continue;
        if (parsed.size() >= m_max_stacks) break; // respect capacity, drop rest
        parsed.push_back(s);
    }
    std::sort(parsed.begin(), parsed.end(),
              [](const ItemStack& a, const ItemStack& b) { return a.id < b.id; });
    m_stacks = std::move(parsed);
    return true;
}

} // namespace nf::gameplay
