#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Gameplay/GameplayAttributes.hpp>
#include <NF/Gameplay/Tags.hpp>

#include <array>
#include <string>
#include <vector>

namespace nf::gameplay {

/// Equipment Slots available for character gear
enum class EquipSlot : u8 {
    MainHand = 0,
    OffHand,
    Head,
    Chest,
    Legs,
    Feet,
    Ring1,
    Ring2,
    Amulet,
    Count
};

[[nodiscard]] const char* equip_slot_to_string(EquipSlot slot) noexcept;
[[nodiscard]] EquipSlot string_to_equip_slot(const std::string& str) noexcept;

/// Item rarity tiers
enum class ItemRarity : u8 {
    Common = 0,
    Uncommon,
    Rare,
    Epic,
    Legendary
};

[[nodiscard]] const char* item_rarity_to_string(ItemRarity rarity) noexcept;

/// Stat modifier attached to an equippable item
struct ItemStatModifier {
    std::string attribute_name;
    ModifierOp op = ModifierOp::Add;
    float value = 0.0f;
};

/// Definition of an equippable item
struct EquipItemDef {
    std::string id;
    std::string name;
    EquipSlot slot = EquipSlot::MainHand;
    ItemRarity rarity = ItemRarity::Common;
    float weight = 0.0f;
    float durability = 100.0f;
    std::vector<ItemStatModifier> stat_modifiers;
    std::vector<std::string> granted_tags;
    std::vector<std::string> required_tags;

    bool valid() const noexcept { return !id.empty(); }
};

/// An individual equipment slot state
struct EquippedSlotInfo {
    EquipItemDef item;
    bool is_occupied = false;
};

/// Equipment manager handling item slots, stat modifications on AttributeSet,
/// and tag grants/requirements on TagContainer.
class EquipmentManager {
public:
    static constexpr uint64_t kEquipmentBaseSourceId = 0xEA000000ull;

    EquipmentManager();

    /// Equips an item into its designated slot.
    /// If slot is occupied, un-equips existing item and places it into out_previous.
    /// Applies stat modifiers to attributes and grants tags to tags.
    bool equip(
        EquipItemDef item,
        AttributeSet* attributes = nullptr,
        TagContainer* tags = nullptr,
        EquipItemDef* out_previous = nullptr
    );

    /// Un-equips an item from slot, clearing its stat modifiers and revoking granted tags.
    bool unequip(
        EquipSlot slot,
        AttributeSet* attributes = nullptr,
        TagContainer* tags = nullptr,
        EquipItemDef* out_removed = nullptr
    );

    /// Tests if a slot is currently occupied
    [[nodiscard]] bool is_slot_occupied(EquipSlot slot) const noexcept;

    /// Retrieves pointer to item in slot (or nullptr if empty)
    [[nodiscard]] const EquipItemDef* get_equipped_item(EquipSlot slot) const noexcept;

    /// Total weight of all equipped items
    [[nodiscard]] float total_weight() const noexcept;

    /// Number of currently occupied equipment slots
    [[nodiscard]] size_t equipped_count() const noexcept;

    /// Tests if an item with specific ID is equipped
    [[nodiscard]] bool has_item(const std::string& item_id) const noexcept;

    /// Finds slot where an item is equipped
    [[nodiscard]] bool get_item_slot(const std::string& item_id, EquipSlot* out_slot = nullptr) const noexcept;

    /// Serializes equipment setup to text for saving
    [[nodiscard]] std::string serialize() const;

    /// Deserializes equipment setup from text, re-applying modifiers and tags
    bool parse(const std::string& text, AttributeSet* attributes = nullptr, TagContainer* tags = nullptr);

    /// Clears all equipment, clearing modifiers and revoking tags
    void clear(AttributeSet* attributes = nullptr, TagContainer* tags = nullptr);

private:
    [[nodiscard]] static uint64_t slot_source_id(EquipSlot slot) noexcept {
        return kEquipmentBaseSourceId | static_cast<uint64_t>(slot);
    }

    std::array<EquippedSlotInfo, static_cast<size_t>(EquipSlot::Count)> m_slots;
};

} // namespace nf::gameplay
