// Tests/GameplayTests/test_crafting.cpp — recipe synthesis, crafting stations, and crafting queues.

#include <NF/Gameplay/Crafting.hpp>
#include <NF/Test/TestFramework.hpp>

#include <string>
#include <vector>

namespace {

using nf::f32;
using nf::u32;
using nf::u64;
using nf::gameplay::CraftingIngredient;
using nf::gameplay::CraftingManager;
using nf::gameplay::CraftingOutput;
using nf::gameplay::CraftingResult;
using nf::gameplay::CraftingStationType;
using nf::gameplay::Inventory;
using nf::gameplay::Recipe;
using nf::gameplay::TagContainer;

NF_TEST(recipe_registration_and_lookup) {
    CraftingManager mgr;
    Recipe r;
    r.id = "torch";
    r.display_name = "Wooden Torch";
    r.station = CraftingStationType::Handheld;
    r.ingredients = {{"wood", 1, true}, {"cloth", 1, true}};
    r.output = {"torch", 4, 1.0f, 0.0f, 99};
    mgr.register_recipe(r);

    NF_CHECK_EQ(mgr.recipe_count(), 1u);
    const Recipe* found = mgr.find_recipe("torch");
    NF_CHECK(found != nullptr);
    NF_CHECK_EQ(found->display_name, "Wooden Torch");
    NF_CHECK_EQ(found->output.count, 4u);

    // Unregister
    bool unreg = mgr.unregister_recipe("torch");
    NF_CHECK(unreg);
    NF_CHECK_EQ(mgr.recipe_count(), 0u);
}

NF_TEST(recipe_lock_and_unlock) {
    CraftingManager mgr;
    Recipe r;
    r.id = "steel_sword";
    r.station = CraftingStationType::Forge;
    r.ingredients = {{"steel_ingot", 2, true}};
    r.output = {"steel_sword", 1, 1.0f, 0.0f, 1};
    r.unlocked = false; // locked initially
    mgr.register_recipe(r);

    Inventory inv;
    inv.add("steel_ingot", 10);
    TagContainer tags;

    // Craft attempt fails because recipe is locked
    CraftingResult res = mgr.can_craft("steel_sword", CraftingStationType::Forge, inv, tags);
    NF_CHECK(res == CraftingResult::RecipeLocked);

    // Unlock
    mgr.unlock_recipe("steel_sword");
    res = mgr.can_craft("steel_sword", CraftingStationType::Forge, inv, tags);
    NF_CHECK(res == CraftingResult::Success);
}

NF_TEST(station_validation) {
    CraftingManager mgr;
    Recipe r;
    r.id = "anvil";
    r.station = CraftingStationType::Forge;
    r.ingredients = {{"iron_ingot", 5, true}};
    r.output = {"anvil", 1, 1.0f, 0.0f, 1};
    mgr.register_recipe(r);

    Inventory inv;
    inv.add("iron_ingot", 10);
    TagContainer tags;

    // Wrong station (Workbench instead of Forge)
    CraftingResult res = mgr.can_craft("anvil", CraftingStationType::Workbench, inv, tags);
    NF_CHECK(res == CraftingResult::StationMismatch);

    // Correct station
    res = mgr.can_craft("anvil", CraftingStationType::Forge, inv, tags);
    NF_CHECK(res == CraftingResult::Success);
}

NF_TEST(required_tags_check) {
    CraftingManager mgr;
    Recipe r;
    r.id = "magic_elixir";
    r.station = CraftingStationType::AlchemyTable;
    r.ingredients = {{"herb", 2, true}};
    r.output = {"magic_elixir", 1, 1.0f, 0.0f, 10};
    r.required_player_tags.add("skill.alchemy.master");
    mgr.register_recipe(r);

    Inventory inv;
    inv.add("herb", 5);
    TagContainer tags;

    // Missing required tag
    CraftingResult res = mgr.can_craft("magic_elixir", CraftingStationType::AlchemyTable, inv, tags);
    NF_CHECK(res == CraftingResult::MissingTags);

    // Add matching tag
    tags.add("skill.alchemy.master");
    res = mgr.can_craft("magic_elixir", CraftingStationType::AlchemyTable, inv, tags);
    NF_CHECK(res == CraftingResult::Success);
}

NF_TEST(instant_crafting_consumes_ingredients) {
    CraftingManager mgr;
    Recipe r;
    r.id = "stone_axe";
    r.station = CraftingStationType::Workbench;
    r.ingredients = {{"wood", 3, true}, {"stone", 2, true}};
    r.output = {"stone_axe", 1, 1.0f, 0.0f, 1};
    mgr.register_recipe(r);

    Inventory inv;
    inv.add("wood", 10);
    inv.add("stone", 10);
    TagContainer tags;

    CraftingResult res = mgr.craft_instant("stone_axe", CraftingStationType::Workbench, inv, tags);
    NF_CHECK(res == CraftingResult::Success);

    // Check consumed ingredients
    NF_CHECK_EQ(inv.count_of("wood"), 7u);
    NF_CHECK_EQ(inv.count_of("stone"), 8u);
    NF_CHECK_EQ(inv.count_of("stone_axe"), 1u);
}

NF_TEST(catalyst_tools_not_consumed) {
    CraftingManager mgr;
    Recipe r;
    r.id = "crushed_ore";
    r.station = CraftingStationType::Handheld;
    r.ingredients = {
        {"ore", 2, true},          // consumed
        {"hammer", 1, false}       // reusable tool, NOT consumed
    };
    r.output = {"crushed_ore", 2, 1.0f, 0.0f, 99};
    mgr.register_recipe(r);

    Inventory inv;
    inv.add("ore", 4);
    inv.add("hammer", 1);
    TagContainer tags;

    CraftingResult res = mgr.craft_instant("crushed_ore", CraftingStationType::Handheld, inv, tags);
    NF_CHECK(res == CraftingResult::Success);

    // Hammer should still be in inventory
    NF_CHECK_EQ(inv.count_of("hammer"), 1u);
    // Ore should be reduced by 2
    NF_CHECK_EQ(inv.count_of("ore"), 2u);
    // Output created
    NF_CHECK_EQ(inv.count_of("crushed_ore"), 2u);
}

NF_TEST(batch_crafting_scales_inputs_and_outputs) {
    CraftingManager mgr;
    Recipe r;
    r.id = "arrow";
    r.station = CraftingStationType::Handheld;
    r.ingredients = {{"wood", 1, true}, {"feather", 1, true}};
    r.output = {"arrow", 5, 1.0f, 0.0f, 99};
    mgr.register_recipe(r);

    Inventory inv;
    inv.add("wood", 10);
    inv.add("feather", 10);
    TagContainer tags;

    // Craft 4 batches
    CraftingResult res = mgr.craft_instant("arrow", CraftingStationType::Handheld, inv, tags, 4u);
    NF_CHECK(res == CraftingResult::Success);

    NF_CHECK_EQ(inv.count_of("wood"), 6u);
    NF_CHECK_EQ(inv.count_of("feather"), 6u);
    NF_CHECK_EQ(inv.count_of("arrow"), 20u); // 4 * 5
}

NF_TEST(timed_crafting_queue_lifecycle) {
    CraftingManager mgr;
    Recipe r;
    r.id = "iron_plate";
    r.station = CraftingStationType::Forge;
    r.ingredients = {{"iron_ingot", 2, true}};
    r.output = {"iron_plate", 1, 1.0f, 0.0f, 99};
    r.craft_time = 2.0f; // 2 seconds craft time
    mgr.register_recipe(r);

    Inventory inv;
    inv.add("iron_ingot", 4);
    TagContainer tags;

    // Queue 1 plate
    CraftingResult res = mgr.queue_craft("iron_plate", CraftingStationType::Forge, inv, tags, 1u);
    NF_CHECK(res == CraftingResult::Success);
    NF_CHECK_EQ(mgr.queue_count(), 1u);
    // Ingredients deducted immediately
    NF_CHECK_EQ(inv.count_of("iron_ingot"), 2u);
    NF_CHECK_EQ(inv.count_of("iron_plate"), 0u);

    // Step 1.0s (halfway)
    mgr.update(1.0f, inv);
    NF_CHECK_EQ(mgr.queue_count(), 1u);
    NF_CHECK_EQ(inv.count_of("iron_plate"), 0u);

    // Step another 1.2s (total 2.2s > 2.0s duration) -> completed
    mgr.update(1.2f, inv);
    NF_CHECK_EQ(mgr.queue_count(), 0u);
    NF_CHECK_EQ(inv.count_of("iron_plate"), 1u);
}

NF_TEST(cancel_queued_craft_refunds_ingredients) {
    CraftingManager mgr;
    Recipe r;
    r.id = "gold_ring";
    r.station = CraftingStationType::Workbench;
    r.ingredients = {{"gold_nugget", 3, true}};
    r.output = {"gold_ring", 1, 1.0f, 0.0f, 10};
    r.craft_time = 5.0f;
    mgr.register_recipe(r);

    Inventory inv;
    inv.add("gold_nugget", 6);
    TagContainer tags;

    mgr.queue_craft("gold_ring", CraftingStationType::Workbench, inv, tags, 1u);
    NF_CHECK_EQ(inv.count_of("gold_nugget"), 3u);
    NF_CHECK_EQ(mgr.queue_count(), 1u);

    u64 task_id = mgr.queue().front().task_id;

    // Cancel task
    bool cancelled = mgr.cancel_task(task_id, inv);
    NF_CHECK(cancelled);
    NF_CHECK_EQ(mgr.queue_count(), 0u);

    // Ingredients refunded back into inventory
    NF_CHECK_EQ(inv.count_of("gold_nugget"), 6u);
}

NF_TEST(crafter_skill_bonus_yield) {
    CraftingManager mgr;
    Recipe r;
    r.id = "healing_potion";
    r.station = CraftingStationType::AlchemyTable;
    r.ingredients = {{"water", 1, true}, {"flower", 1, true}};
    r.output = {"healing_potion", 1, 1.0f, 1.0f, 99}; // 100% bonus chance with skill
    mgr.register_recipe(r);

    Inventory inv;
    inv.add("water", 10);
    inv.add("flower", 10);
    TagContainer tags;

    // Normal craft (multiplier 1.0) -> yields 1
    mgr.craft_instant("healing_potion", CraftingStationType::AlchemyTable, inv, tags, 1u, 1.0f);
    NF_CHECK_EQ(inv.count_of("healing_potion"), 1u);

    // Master crafter (multiplier 2.0) -> yields 1 + 1 = 2
    mgr.craft_instant("healing_potion", CraftingStationType::AlchemyTable, inv, tags, 1u, 2.0f);
    NF_CHECK_EQ(inv.count_of("healing_potion"), 3u); // 1 + 2 = 3
}

} // namespace
