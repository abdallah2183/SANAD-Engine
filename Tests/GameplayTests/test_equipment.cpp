#include <NF/Test/TestFramework.hpp>
#include <NF/Gameplay/Equipment.hpp>
#include <NF/Gameplay/GameplayAttributes.hpp>
#include <NF/Gameplay/Tags.hpp>

#include <vector>

using namespace nf;
using namespace nf::gameplay;

NF_TEST(equipment_slot_assignment_and_retrieval) {
    EquipmentManager eq;
    NF_CHECK_EQ(eq.equipped_count(), 0u);
    NF_CHECK(!eq.is_slot_occupied(EquipSlot::MainHand));

    EquipItemDef sword;
    sword.id = "sword_iron_01";
    sword.name = "Iron Longsword";
    sword.slot = EquipSlot::MainHand;
    sword.rarity = ItemRarity::Common;
    sword.weight = 3.5f;

    NF_CHECK(eq.equip(sword));
    NF_CHECK_EQ(eq.equipped_count(), 1u);
    NF_CHECK(eq.is_slot_occupied(EquipSlot::MainHand));
    NF_CHECK_NEAR(eq.total_weight(), 3.5f, 1e-4f);

    const EquipItemDef* found = eq.get_equipped_item(EquipSlot::MainHand);
    NF_CHECK(found != nullptr);
    NF_CHECK_EQ(found->id, "sword_iron_01");
    NF_CHECK_EQ(found->name, "Iron Longsword");

    // Unequip
    EquipItemDef removed;
    NF_CHECK(eq.unequip(EquipSlot::MainHand, nullptr, nullptr, &removed));
    NF_CHECK_EQ(removed.id, "sword_iron_01");
    NF_CHECK_EQ(eq.equipped_count(), 0u);
    NF_CHECK(!eq.is_slot_occupied(EquipSlot::MainHand));
}

NF_TEST(equipment_attribute_modifiers_integration) {
    EquipmentManager eq;
    AttributeSet attrs;
    attrs.register_attribute("Attack", 20.0f, 0.0f, 1000.0f);
    attrs.register_attribute("Defense", 10.0f, 0.0f, 1000.0f);

    NF_CHECK_NEAR(attrs.get_current_value("Attack"), 20.0f, 1e-4f);
    NF_CHECK_NEAR(attrs.get_current_value("Defense"), 10.0f, 1e-4f);

    // Equip Sword: +30 Attack
    EquipItemDef sword;
    sword.id = "sword_steel";
    sword.name = "Steel Blade";
    sword.slot = EquipSlot::MainHand;
    sword.stat_modifiers.push_back(ItemStatModifier{"Attack", ModifierOp::Add, 30.0f});
    NF_CHECK(eq.equip(sword, &attrs));

    NF_CHECK_NEAR(attrs.get_current_value("Attack"), 50.0f, 1e-4f);

    // Equip Shield: +15 Defense
    EquipItemDef shield;
    shield.id = "shield_tower";
    shield.name = "Tower Shield";
    shield.slot = EquipSlot::OffHand;
    shield.stat_modifiers.push_back(ItemStatModifier{"Defense", ModifierOp::Add, 15.0f});
    NF_CHECK(eq.equip(shield, &attrs));

    NF_CHECK_NEAR(attrs.get_current_value("Defense"), 25.0f, 1e-4f);

    // Equip Ring of Might: * 1.5 Attack
    EquipItemDef ring;
    ring.id = "ring_might";
    ring.name = "Ring of Might";
    ring.slot = EquipSlot::Ring1;
    ring.stat_modifiers.push_back(ItemStatModifier{"Attack", ModifierOp::Multiply, 1.5f});
    NF_CHECK(eq.equip(ring, &attrs));

    // (20 base + 30 sword) * 1.5 ring = 75.0f Attack
    NF_CHECK_NEAR(attrs.get_current_value("Attack"), 75.0f, 1e-4f);

    // Unequip sword: Attack becomes 20 base * 1.5 ring = 30.0f
    NF_CHECK(eq.unequip(EquipSlot::MainHand, &attrs));
    NF_CHECK_NEAR(attrs.get_current_value("Attack"), 30.0f, 1e-4f);

    // Clear all equipment
    eq.clear(&attrs);
    NF_CHECK_NEAR(attrs.get_current_value("Attack"), 20.0f, 1e-4f);
    NF_CHECK_NEAR(attrs.get_current_value("Defense"), 10.0f, 1e-4f);
}

NF_TEST(equipment_tag_grants_and_requirements) {
    EquipmentManager eq;
    TagContainer tags;

    EquipItemDef warhammer;
    warhammer.id = "warhammer_heavy";
    warhammer.name = "Heavy Warhammer";
    warhammer.slot = EquipSlot::MainHand;
    warhammer.required_tags.push_back("Proficiency.Warhammer");
    warhammer.granted_tags.push_back("Weapon.TwoHanded");
    warhammer.granted_tags.push_back("DamageType.Bludgeoning");

    // 1. Missing required tag -> Equip fails
    NF_CHECK(!eq.equip(warhammer, nullptr, &tags));
    NF_CHECK(!tags.has("Weapon.TwoHanded"));

    // 2. Grant proficiency tag to player -> Equip succeeds
    tags.add("Proficiency.Warhammer");
    NF_CHECK(eq.equip(warhammer, nullptr, &tags));

    NF_CHECK(tags.has("Weapon.TwoHanded"));
    NF_CHECK(tags.has("DamageType.Bludgeoning"));

    // 3. Unequip -> Granted tags are revoked
    NF_CHECK(eq.unequip(EquipSlot::MainHand, nullptr, &tags));
    NF_CHECK(!tags.has("Weapon.TwoHanded"));
    NF_CHECK(!tags.has("DamageType.Bludgeoning"));
    // Proficiency tag remains
    NF_CHECK(tags.has("Proficiency.Warhammer"));
}

NF_TEST(equipment_slot_swapping_and_replacement) {
    EquipmentManager eq;
    AttributeSet attrs;
    attrs.register_attribute("Speed", 10.0f);

    EquipItemDef boots1;
    boots1.id = "leather_boots";
    boots1.name = "Leather Boots";
    boots1.slot = EquipSlot::Feet;
    boots1.stat_modifiers.push_back(ItemStatModifier{"Speed", ModifierOp::Add, 2.0f});
    NF_CHECK(eq.equip(boots1, &attrs));
    NF_CHECK_NEAR(attrs.get_current_value("Speed"), 12.0f, 1e-4f);

    // Equip Wind Runners over leather boots
    EquipItemDef boots2;
    boots2.id = "wind_runners";
    boots2.name = "Wind Runners";
    boots2.slot = EquipSlot::Feet;
    boots2.stat_modifiers.push_back(ItemStatModifier{"Speed", ModifierOp::Add, 8.0f});

    EquipItemDef previous;
    NF_CHECK(eq.equip(boots2, &attrs, nullptr, &previous));

    // Previous returned correctly
    NF_CHECK_EQ(previous.id, "leather_boots");
    // New speed is 10 base + 8 boots2 = 18.0f (boots1 modifier removed!)
    NF_CHECK_NEAR(attrs.get_current_value("Speed"), 18.0f, 1e-4f);
    NF_CHECK_EQ(eq.get_equipped_item(EquipSlot::Feet)->id, "wind_runners");
}

NF_TEST(equipment_serialization_and_roundtrip) {
    EquipmentManager eq;
    AttributeSet attrs;
    TagContainer tags;

    attrs.register_attribute("HealthMax", 100.0f);
    attrs.register_attribute("Armor", 0.0f);

    EquipItemDef helm;
    helm.id = "helm_dragon";
    helm.name = "Dragon Helm";
    helm.slot = EquipSlot::Head;
    helm.rarity = ItemRarity::Legendary;
    helm.weight = 4.0f;
    helm.stat_modifiers.push_back(ItemStatModifier{"HealthMax", ModifierOp::Add, 50.0f});
    helm.stat_modifiers.push_back(ItemStatModifier{"Armor", ModifierOp::Add, 20.0f});
    helm.granted_tags.push_back("Set.Dragonscale");
    eq.equip(helm, &attrs, &tags);

    EquipItemDef chest;
    chest.id = "chest_dragon";
    chest.name = "Dragon Cuirass";
    chest.slot = EquipSlot::Chest;
    chest.rarity = ItemRarity::Epic;
    chest.weight = 12.0f;
    chest.stat_modifiers.push_back(ItemStatModifier{"Armor", ModifierOp::Add, 45.0f});
    chest.granted_tags.push_back("Set.Dragonscale");
    eq.equip(chest, &attrs, &tags);

    NF_CHECK_NEAR(attrs.get_current_value("HealthMax"), 150.0f, 1e-4f);
    NF_CHECK_NEAR(attrs.get_current_value("Armor"), 65.0f, 1e-4f);

    std::string text = eq.serialize();
    NF_CHECK(!text.empty());

    // Restore into fresh setup
    EquipmentManager eq_loaded;
    AttributeSet attrs_loaded;
    TagContainer tags_loaded;
    attrs_loaded.register_attribute("HealthMax", 100.0f);
    attrs_loaded.register_attribute("Armor", 0.0f);

    NF_CHECK(eq_loaded.parse(text, &attrs_loaded, &tags_loaded));
    NF_CHECK_EQ(eq_loaded.equipped_count(), 2u);
    NF_CHECK_NEAR(eq_loaded.total_weight(), 16.0f, 1e-4f);
    NF_CHECK_NEAR(attrs_loaded.get_current_value("HealthMax"), 150.0f, 1e-4f);
    NF_CHECK_NEAR(attrs_loaded.get_current_value("Armor"), 65.0f, 1e-4f);
    NF_CHECK(tags_loaded.has("Set.Dragonscale"));
}
