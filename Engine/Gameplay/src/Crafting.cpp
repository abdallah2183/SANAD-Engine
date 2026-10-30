#include <NF/Gameplay/Crafting.hpp>
#include <algorithm>
#include <cmath>

namespace nf::gameplay {

void CraftingManager::register_recipe(const Recipe& recipe) {
    for (auto& r : m_recipes) {
        if (r.id == recipe.id) {
            r = recipe;
            return;
        }
    }
    m_recipes.push_back(recipe);
}

bool CraftingManager::unregister_recipe(const std::string& recipe_id) {
    auto it = std::find_if(m_recipes.begin(), m_recipes.end(),
                           [&recipe_id](const Recipe& r) { return r.id == recipe_id; });
    if (it != m_recipes.end()) {
        m_recipes.erase(it);
        return true;
    }
    return false;
}

const Recipe* CraftingManager::find_recipe(const std::string& recipe_id) const {
    for (const auto& r : m_recipes) {
        if (r.id == recipe_id) {
            return &r;
        }
    }
    return nullptr;
}

bool CraftingManager::unlock_recipe(const std::string& recipe_id) {
    for (auto& r : m_recipes) {
        if (r.id == recipe_id) {
            r.unlocked = true;
            return true;
        }
    }
    return false;
}

bool CraftingManager::lock_recipe(const std::string& recipe_id) {
    for (auto& r : m_recipes) {
        if (r.id == recipe_id) {
            r.unlocked = false;
            return true;
        }
    }
    return false;
}

bool CraftingManager::is_recipe_unlocked(const std::string& recipe_id) const {
    const Recipe* r = find_recipe(recipe_id);
    return r != nullptr && r->unlocked;
}

CraftingResult CraftingManager::can_craft(const std::string& recipe_id,
                                         CraftingStationType current_station,
                                         const Inventory& inventory,
                                         const TagContainer& player_tags,
                                         u32 batch_count) const {
    if (batch_count == 0) {
        return CraftingResult::Success;
    }

    const Recipe* recipe = find_recipe(recipe_id);
    if (recipe == nullptr) {
        return CraftingResult::RecipeNotFound;
    }

    if (!recipe->unlocked) {
        return CraftingResult::RecipeLocked;
    }

    if (recipe->station != current_station) {
        return CraftingResult::StationMismatch;
    }

    for (const auto& tag : recipe->required_player_tags.all()) {
        if (!player_tags.has(tag)) {
            return CraftingResult::MissingTags;
        }
    }

    for (const auto& ing : recipe->ingredients) {
        u32 needed = ing.count * batch_count;
        if (inventory.count_of(ing.item_id) < needed) {
            return CraftingResult::MissingIngredients;
        }
    }

    return CraftingResult::Success;
}

CraftingResult CraftingManager::craft_instant(const std::string& recipe_id,
                                             CraftingStationType current_station,
                                             Inventory& inventory,
                                             const TagContainer& player_tags,
                                             u32 batch_count,
                                             f32 crafter_skill_multiplier) {
    if (batch_count == 0) {
        return CraftingResult::Success;
    }

    CraftingResult check = can_craft(recipe_id, current_station, inventory, player_tags, batch_count);
    if (check != CraftingResult::Success) {
        return check;
    }

    const Recipe* recipe = find_recipe(recipe_id);
    if (recipe == nullptr) {
        return CraftingResult::RecipeNotFound;
    }

    // Consume required ingredients
    for (const auto& ing : recipe->ingredients) {
        if (ing.consumed) {
            inventory.remove(ing.item_id, ing.count * batch_count);
        }
    }

    // Determine yield count
    u32 total_yield = recipe->output.count * batch_count;
    if (recipe->output.bonus_chance > 0.0f && crafter_skill_multiplier > 1.0f) {
        f32 bonus_roll = recipe->output.bonus_chance * (crafter_skill_multiplier - 1.0f);
        if (bonus_roll >= 0.5f) {
            total_yield += batch_count;
        }
    }

    u32 added = inventory.add(recipe->output.item_id, total_yield, recipe->output.max_stack);
    if (added < total_yield) {
        // Inventory was partially or completely full
        return CraftingResult::InventoryFull;
    }

    return CraftingResult::Success;
}

CraftingResult CraftingManager::queue_craft(const std::string& recipe_id,
                                           CraftingStationType current_station,
                                           Inventory& inventory,
                                           const TagContainer& player_tags,
                                           u32 batch_count) {
    if (batch_count == 0) {
        return CraftingResult::Success;
    }

    CraftingResult check = can_craft(recipe_id, current_station, inventory, player_tags, batch_count);
    if (check != CraftingResult::Success) {
        return check;
    }

    const Recipe* recipe = find_recipe(recipe_id);
    if (recipe == nullptr) {
        return CraftingResult::RecipeNotFound;
    }

    // Consume ingredients upfront
    for (const auto& ing : recipe->ingredients) {
        if (ing.consumed) {
            inventory.remove(ing.item_id, ing.count * batch_count);
        }
    }

    CraftingQueueTask task;
    task.task_id = m_next_task_id++;
    task.recipe_id = recipe_id;
    task.duration = std::max(0.001f, recipe->craft_time * static_cast<f32>(batch_count));
    task.progress = 0.0f;
    task.count = batch_count;
    task.completed = false;

    m_queue.push_back(task);
    return CraftingResult::Success;
}

bool CraftingManager::cancel_task(u64 task_id, Inventory& inventory) {
    auto it = std::find_if(m_queue.begin(), m_queue.end(),
                           [task_id](const CraftingQueueTask& t) { return t.task_id == task_id; });
    if (it == m_queue.end()) {
        return false;
    }

    const Recipe* recipe = find_recipe(it->recipe_id);
    if (recipe != nullptr) {
        // Refund consumed ingredients
        for (const auto& ing : recipe->ingredients) {
            if (ing.consumed) {
                inventory.add(ing.item_id, ing.count * it->count);
            }
        }
    }

    m_queue.erase(it);
    return true;
}

void CraftingManager::update(f32 dt, Inventory& inventory) {
    if (dt <= 0.0f || m_queue.empty()) {
        return;
    }

    for (auto& task : m_queue) {
        if (task.completed) {
            continue;
        }

        task.progress += dt;
        if (task.progress >= task.duration) {
            task.completed = true;

            const Recipe* recipe = find_recipe(task.recipe_id);
            if (recipe != nullptr) {
                u32 total_yield = recipe->output.count * task.count;
                inventory.add(recipe->output.item_id, total_yield, recipe->output.max_stack);
            }
        }
    }

    m_queue.erase(
        std::remove_if(m_queue.begin(), m_queue.end(),
                       [](const CraftingQueueTask& t) { return t.completed; }),
        m_queue.end());
}

void CraftingManager::clear() {
    m_recipes.clear();
    m_queue.clear();
}

} // namespace nf::gameplay
