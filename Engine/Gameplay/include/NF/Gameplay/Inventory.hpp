#pragma once

// NF/Gameplay/Inventory.hpp — stacked item inventory (design doc 217).
//
// Items stack by id up to max_stack each; the inventory caps total stacks.
// Deterministic order (sorted by id) for saves/diffs. Text encode/decode
// round-trips for GameplayModuleComponent properties.

#include <NF/Core/Types.hpp>

#include <string>
#include <vector>

namespace nf::gameplay {

struct ItemStack {
    std::string id;
    u32 count = 0;
    u32 max_stack = 99;
};

class Inventory {
public:
    explicit Inventory(u32 max_stacks = 24) : m_max_stacks(max_stacks) {}

    /// Adds up to `count` items, filling partial stacks first. Returns the
    /// number actually added (less than count only when full).
    u32 add(const std::string& id, u32 count, u32 max_stack = 99);
    /// Removes up to `count` items. Returns the number actually removed.
    u32 remove(const std::string& id, u32 count);

    u32 count_of(const std::string& id) const;
    bool has(const std::string& id, u32 count = 1) const;
    usize stack_count() const { return m_stacks.size(); }
    u32 max_stacks() const { return m_max_stacks; }
    bool empty() const { return m_stacks.empty(); }
    void clear();

    /// Stacks sorted by id (deterministic).
    const std::vector<ItemStack>& stacks() const { return m_stacks; }

    std::string serialize() const;
    bool parse(const std::string& text);

private:
    std::vector<ItemStack> m_stacks;
    u32 m_max_stacks = 24;
};

} // namespace nf::gameplay
