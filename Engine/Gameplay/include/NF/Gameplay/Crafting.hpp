#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Gameplay/Inventory.hpp>
#include <NF/Gameplay/Tags.hpp>

#include <string>
#include <vector>

namespace nf::gameplay {

/// Type or tier of crafting station needed to craft an item.
enum class CraftingStationType : u32 {
    Handheld = 0,   // In-inventory crafting (torches, bandages, twine)
    Workbench,      // Tools, basic weapons, construction materials
    Forge,          // Smelting ingots, heavy armor, blades
    AlchemyTable,   // Potions, elixirs, poisons, dyes
    Campfire,       // Cooking food, boiling water
};

/// An input item requirement for a recipe.
struct CraftingIngredient {
    std::string item_id;
    u32 count = 1u;
    bool consumed = true; // false for reusable catalysts / tools (e.g. hammer, mortar)
};

/// Output product created by a recipe.
struct CraftingOutput {
    std::string item_id;
    u32 count = 1u;
    f32 base_quality = 1.0f;
    f32 bonus_chance = 0.0f; // Probability of yielding extra output
    u32 max_stack = 99u;
};

/// A recipe for synthesizing an item.
struct Recipe {
    std::string id;
    std::string display_name;
    CraftingStationType station = CraftingStationType::Handheld;
    std::vector<CraftingIngredient> ingredients;
    CraftingOutput output;
    f32 craft_time = 0.0f; // 0.0 = instant, >0.0 = timed crafting queue
    TagContainer required_player_tags; // e.g. "skill.blacksmithing.apprentice"
    bool unlocked = true;
};

/// An entry in a timed crafting station queue.
struct CraftingQueueTask {
    u64 task_id = 0ull;
    std::string recipe_id;
    f32 duration = 0.0f;
    f32 progress = 0.0f;
    u32 count = 1u;
    bool completed = false;
};

/// Outcome of attempting to craft a recipe.
enum class CraftingResult : u32 {
    Success = 0,
    MissingIngredients,
    InventoryFull,
    MissingTags,
    StationMismatch,
    RecipeLocked,
    RecipeNotFound,
};

/// Manages recipe definitions, station validation, instant crafting, and timed crafting queues.
class CraftingManager {
public:
    CraftingManager() = default;

    /// Registers a recipe. Overwrites if same ID already exists.
    void register_recipe(const Recipe& recipe);
    bool unregister_recipe(const std::string& recipe_id);
    const Recipe* find_recipe(const std::string& recipe_id) const;

    /// Recipe discovery / unlocks.
    bool unlock_recipe(const std::string& recipe_id);
    bool lock_recipe(const std::string& recipe_id);
    bool is_recipe_unlocked(const std::string& recipe_id) const;

    /// Checks if the player can craft the recipe given station, inventory, and player tags.
    CraftingResult can_craft(const std::string& recipe_id,
                             CraftingStationType current_station,
                             const Inventory& inventory,
                             const TagContainer& player_tags,
                             u32 batch_count = 1u) const;

    /// Instant crafting: validates, consumes ingredients from inventory, and deposits output.
    CraftingResult craft_instant(const std::string& recipe_id,
                                 CraftingStationType current_station,
                                 Inventory& inventory,
                                 const TagContainer& player_tags,
                                 u32 batch_count = 1u,
                                 f32 crafter_skill_multiplier = 1.0f);

    /// Timed crafting: queues a recipe to be processed over time at a station.
    /// Deducts ingredients immediately; refunds them if cancelled.
    CraftingResult queue_craft(const std::string& recipe_id,
                               CraftingStationType current_station,
                               Inventory& inventory,
                               const TagContainer& player_tags,
                               u32 batch_count = 1u);

    /// Cancels a queued task and refunds its ingredients back to inventory.
    bool cancel_task(u64 task_id, Inventory& inventory);

    /// Advances the crafting queue by dt seconds. When a task completes, outputs are deposited in inventory.
    void update(f32 dt, Inventory& inventory);

    /// Returns all registered recipes.
    const std::vector<Recipe>& recipes() const { return m_recipes; }
    size_t recipe_count() const { return m_recipes.size(); }

    /// Returns active queue tasks.
    const std::vector<CraftingQueueTask>& queue() const { return m_queue; }
    size_t queue_count() const { return m_queue.size(); }

    void clear();

private:
    u64 m_next_task_id = 1ull;
    std::vector<Recipe> m_recipes;
    std::vector<CraftingQueueTask> m_queue;
};

} // namespace nf::gameplay
