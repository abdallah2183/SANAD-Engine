#include <NF/Gameplay/Equipment.hpp>

#include <sstream>

namespace nf::gameplay {

const char* equip_slot_to_string(EquipSlot slot) noexcept {
    switch (slot) {
        case EquipSlot::MainHand: return "MainHand";
        case EquipSlot::OffHand:  return "OffHand";
        case EquipSlot::Head:     return "Head";
        case EquipSlot::Chest:    return "Chest";
        case EquipSlot::Legs:     return "Legs";
        case EquipSlot::Feet:     return "Feet";
        case EquipSlot::Ring1:    return "Ring1";
        case EquipSlot::Ring2:    return "Ring2";
        case EquipSlot::Amulet:   return "Amulet";
        default:                  return "Unknown";
    }
}

EquipSlot string_to_equip_slot(const std::string& str) noexcept {
    if (str == "MainHand") return EquipSlot::MainHand;
    if (str == "OffHand")  return EquipSlot::OffHand;
    if (str == "Head")     return EquipSlot::Head;
    if (str == "Chest")    return EquipSlot::Chest;
    if (str == "Legs")     return EquipSlot::Legs;
    if (str == "Feet")     return EquipSlot::Feet;
    if (str == "Ring1")    return EquipSlot::Ring1;
    if (str == "Ring2")    return EquipSlot::Ring2;
    if (str == "Amulet")   return EquipSlot::Amulet;
    return EquipSlot::Count;
}

const char* item_rarity_to_string(ItemRarity rarity) noexcept {
    switch (rarity) {
        case ItemRarity::Common:    return "Common";
        case ItemRarity::Uncommon:  return "Uncommon";
        case ItemRarity::Rare:      return "Rare";
        case ItemRarity::Epic:      return "Epic";
        case ItemRarity::Legendary: return "Legendary";
        default:                    return "Common";
    }
}

static ItemRarity string_to_item_rarity(const std::string& str) noexcept {
    if (str == "Uncommon")  return ItemRarity::Uncommon;
    if (str == "Rare")      return ItemRarity::Rare;
    if (str == "Epic")      return ItemRarity::Epic;
    if (str == "Legendary") return ItemRarity::Legendary;
    return ItemRarity::Common;
}

EquipmentManager::EquipmentManager() {
    for (auto& s : m_slots) {
        s.is_occupied = false;
    }
}

bool EquipmentManager::equip(
    EquipItemDef item,
    AttributeSet* attributes,
    TagContainer* tags,
    EquipItemDef* out_previous
) {
    if (!item.valid() || item.slot >= EquipSlot::Count) {
        return false;
    }

    // Check required tags
    if (tags && !item.required_tags.empty()) {
        for (const auto& req : item.required_tags) {
            if (!tags->has(req)) {
                return false; // Requirement not met
            }
        }
    }

    const auto slot_idx = static_cast<size_t>(item.slot);

    // If already occupied, unequip first
    if (m_slots[slot_idx].is_occupied) {
        unequip(item.slot, attributes, tags, out_previous);
    }

    // Apply stat modifiers to attributes
    if (attributes) {
        const uint64_t src_id = slot_source_id(item.slot);
        for (const auto& mod : item.stat_modifiers) {
            AttributeModifier am;
            am.op = mod.op;
            am.value = mod.value;
            am.source_id = src_id;
            attributes->add_modifier(mod.attribute_name, am);
        }
    }

    // Grant tags
    if (tags) {
        for (const auto& tag : item.granted_tags) {
            tags->add(tag);
        }
    }

    m_slots[slot_idx].item = std::move(item);
    m_slots[slot_idx].is_occupied = true;
    return true;
}

bool EquipmentManager::unequip(
    EquipSlot slot,
    AttributeSet* attributes,
    TagContainer* tags,
    EquipItemDef* out_removed
) {
    if (slot >= EquipSlot::Count) {
        return false;
    }

    const auto slot_idx = static_cast<size_t>(slot);
    if (!m_slots[slot_idx].is_occupied) {
        return false;
    }

    // Remove stat modifiers
    if (attributes) {
        const uint64_t src_id = slot_source_id(slot);
        for (const auto& mod : m_slots[slot_idx].item.stat_modifiers) {
            attributes->remove_modifiers_by_source(mod.attribute_name, src_id);
        }
    }

    // Revoke granted tags
    if (tags) {
        for (const auto& tag : m_slots[slot_idx].item.granted_tags) {
            tags->remove(tag);
        }
    }

    if (out_removed) {
        *out_removed = m_slots[slot_idx].item;
    }

    m_slots[slot_idx].is_occupied = false;
    m_slots[slot_idx].item = EquipItemDef{};
    return true;
}

bool EquipmentManager::is_slot_occupied(EquipSlot slot) const noexcept {
    if (slot >= EquipSlot::Count) return false;
    return m_slots[static_cast<size_t>(slot)].is_occupied;
}

const EquipItemDef* EquipmentManager::get_equipped_item(EquipSlot slot) const noexcept {
    if (slot >= EquipSlot::Count) return nullptr;
    const auto& s = m_slots[static_cast<size_t>(slot)];
    return s.is_occupied ? &s.item : nullptr;
}

float EquipmentManager::total_weight() const noexcept {
    float w = 0.0f;
    for (const auto& s : m_slots) {
        if (s.is_occupied) {
            w += s.item.weight;
        }
    }
    return w;
}

size_t EquipmentManager::equipped_count() const noexcept {
    size_t count = 0;
    for (const auto& s : m_slots) {
        if (s.is_occupied) {
            count++;
        }
    }
    return count;
}

bool EquipmentManager::has_item(const std::string& item_id) const noexcept {
    if (item_id.empty()) return false;
    for (const auto& s : m_slots) {
        if (s.is_occupied && s.item.id == item_id) {
            return true;
        }
    }
    return false;
}

bool EquipmentManager::get_item_slot(const std::string& item_id, EquipSlot* out_slot) const noexcept {
    if (item_id.empty()) return false;
    for (size_t i = 0; i < m_slots.size(); ++i) {
        if (m_slots[i].is_occupied && m_slots[i].item.id == item_id) {
            if (out_slot) {
                *out_slot = static_cast<EquipSlot>(i);
            }
            return true;
        }
    }
    return false;
}

std::string EquipmentManager::serialize() const {
    std::ostringstream ss;
    for (size_t i = 0; i < m_slots.size(); ++i) {
        const auto& s = m_slots[i];
        if (!s.is_occupied) continue;

        const auto slot = static_cast<EquipSlot>(i);
        ss << "item " << equip_slot_to_string(slot) << " "
           << s.item.id << " "
           << item_rarity_to_string(s.item.rarity) << " "
           << s.item.weight << " "
           << s.item.durability << " "
           << s.item.name << "\n";

        for (const auto& m : s.item.stat_modifiers) {
            ss << "mod " << equip_slot_to_string(slot) << " "
               << m.attribute_name << " "
               << static_cast<int>(m.op) << " "
               << m.value << "\n";
        }

        for (const auto& t : s.item.granted_tags) {
            ss << "gtag " << equip_slot_to_string(slot) << " " << t << "\n";
        }

        for (const auto& r : s.item.required_tags) {
            ss << "rtag " << equip_slot_to_string(slot) << " " << r << "\n";
        }
    }
    return ss.str();
}

bool EquipmentManager::parse(const std::string& text, AttributeSet* attributes, TagContainer* tags) {
    clear(attributes, tags);

    std::istringstream ss(text);
    std::string line;
    std::array<EquipItemDef, static_cast<size_t>(EquipSlot::Count)> parsed_items;
    std::array<bool, static_cast<size_t>(EquipSlot::Count)> has_parsed{};

    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string cmd;
        ls >> cmd;

        if (cmd == "item") {
            std::string slot_str, id, rarity_str;
            float weight = 0.0f, durability = 100.0f;
            ls >> slot_str >> id >> rarity_str >> weight >> durability;
            std::string name;
            std::getline(ls, name);
            if (!name.empty() && name.front() == ' ') {
                name.erase(0, 1);
            }

            const EquipSlot slot = string_to_equip_slot(slot_str);
            if (slot < EquipSlot::Count) {
                const auto idx = static_cast<size_t>(slot);
                parsed_items[idx].id = id;
                parsed_items[idx].name = name;
                parsed_items[idx].slot = slot;
                parsed_items[idx].rarity = string_to_item_rarity(rarity_str);
                parsed_items[idx].weight = weight;
                parsed_items[idx].durability = durability;
                has_parsed[idx] = true;
            }
        } else if (cmd == "mod") {
            std::string slot_str, attr_name;
            int op_int = 0;
            float val = 0.0f;
            ls >> slot_str >> attr_name >> op_int >> val;
            const EquipSlot slot = string_to_equip_slot(slot_str);
            if (slot < EquipSlot::Count) {
                const auto idx = static_cast<size_t>(slot);
                ItemStatModifier m;
                m.attribute_name = attr_name;
                m.op = static_cast<ModifierOp>(op_int);
                m.value = val;
                parsed_items[idx].stat_modifiers.push_back(m);
            }
        } else if (cmd == "gtag") {
            std::string slot_str, tag;
            ls >> slot_str >> tag;
            const EquipSlot slot = string_to_equip_slot(slot_str);
            if (slot < EquipSlot::Count) {
                parsed_items[static_cast<size_t>(slot)].granted_tags.push_back(tag);
            }
        } else if (cmd == "rtag") {
            std::string slot_str, tag;
            ls >> slot_str >> tag;
            const EquipSlot slot = string_to_equip_slot(slot_str);
            if (slot < EquipSlot::Count) {
                parsed_items[static_cast<size_t>(slot)].required_tags.push_back(tag);
            }
        }
    }

    for (size_t i = 0; i < parsed_items.size(); ++i) {
        if (has_parsed[i]) {
            equip(std::move(parsed_items[i]), attributes, tags);
        }
    }

    return true;
}

void EquipmentManager::clear(AttributeSet* attributes, TagContainer* tags) {
    for (size_t i = 0; i < m_slots.size(); ++i) {
        if (m_slots[i].is_occupied) {
            unequip(static_cast<EquipSlot>(i), attributes, tags);
        }
    }
}

} // namespace nf::gameplay
